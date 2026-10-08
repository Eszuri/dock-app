#include "DockWindow.h"
#include "IconHelper.h"
#include "TaskManager.h"
#include "Logger.h"
#include <windowsx.h>
#include <cmath>
#include <shellapi.h>
#include <algorithm>

DockWindow::DockWindow() = default;

DockWindow::~DockWindow() {
    if (m_hWnd) {
        DeregisterShellHookWindow(m_hWnd);
    }
    CleanupPreviewWindow();
    CleanupMenuWindow();
    m_animator.Cleanup();
    CleanupDirect2D();
    CleanupDIBBuffer();
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}


bool DockWindow::HitTest(int x, int y) const {
    if (x >= (int)m_dockPillRect.rect.left && x <= (int)m_dockPillRect.rect.right &&
        y >= (int)(m_dockPillRect.rect.top - 55.0f) && y <= (int)(m_dockPillRect.rect.bottom + 6.0f)) {
        return true;
    }
    return false;
}

bool DockWindow::Initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    // Register Win32 Window Class
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = DockWindow::WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LiteDockWindowClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    RegisterClassExW(&wc);

    // Initial base dimensions
    size_t count = 6;
    float maxDockWidth = count * (Config::BASE_ICON_SIZE * Config::MAX_ICON_SCALE + Config::ITEM_SPACING)
                       + Config::DOCK_PADDING_X * 2.0f;
    m_canvasWidth = (int)std::ceil(maxDockWidth + 140.0f);
    m_canvasHeight = (int)std::ceil((Config::BASE_ICON_SIZE * Config::MAX_ICON_SCALE) + 65.0f + Config::DOCK_PADDING_Y * 2.0f);

    PositionWindow();

    // Create Layered Topmost Popup Window without stealing foreground activation
    m_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"LiteDock",
        WS_POPUP,
        m_screenX, m_screenY,
        m_canvasWidth, m_canvasHeight,
        NULL, NULL, hInstance, this
    );

    if (!m_hWnd) {
        Log("CreateWindowExW failed");
        return false;
    }

    // Enable Windows 10/11 Dark Mode for popup menus
    HMODULE hUxTheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxTheme) {
        auto fnSetPreferredAppMode = reinterpret_cast<int(WINAPI*)(int)>(
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135)));
        if (fnSetPreferredAppMode) {
            fnSetPreferredAppMode(1); // 1 = AllowDark
        }
        auto fnAllowDarkModeForWindow = reinterpret_cast<bool(WINAPI*)(HWND, bool)>(
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(133)));
        if (fnAllowDarkModeForWindow) {
            fnAllowDarkModeForWindow(m_hWnd, true);
        }
        auto fnFlushMenuThemes = reinterpret_cast<void(WINAPI*)()>(
            GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136)));
        if (fnFlushMenuThemes) {
            fnFlushMenuThemes();
        }
    }

    // Register Windows Shell Hook for instant event-driven taskbar updates
    RegisterShellHookWindow(m_hWnd);
    m_shellHookMsg = RegisterWindowMessageW(L"SHELLHOOK");

    CreateDIBBuffer(m_canvasWidth, m_canvasHeight);
    InitDirect2D();
    InitMenuWindow(hInstance);
    InitPreviewWindow(hInstance);
    m_animator.Initialize(hInstance, m_pD2DFactory);
    m_animator.SetDockHWnd(m_hWnd);

    // Load initial pinned apps & open windows
    RefreshTaskbarItems();

    // Show window without taking focus
    ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hWnd);

    // Periodic fallback refresh (every 1.5 seconds)
    SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 1500, NULL);

    return true;
}

void DockWindow::PositionWindow() {
    RECT rcWork = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

    m_screenX = (rcWork.left + rcWork.right - m_canvasWidth) / 2;
    m_screenY = rcWork.bottom - m_canvasHeight;
}

void DockWindow::ResizeCanvas(size_t itemCount) {
    float neededWidth = itemCount * (Config::BASE_ICON_SIZE * Config::MAX_ICON_SCALE + Config::ITEM_SPACING) + 160.0f;
    int targetW = (int)std::ceil(neededWidth);

    if (targetW != m_canvasWidth) {
        m_canvasWidth = targetW;
        PositionWindow();
        CreateDIBBuffer(m_canvasWidth, m_canvasHeight);

        RECT rc = { 0, 0, m_canvasWidth, m_canvasHeight };
        if (m_pDCRT && m_hdcMem) {
            m_pDCRT->BindDC(m_hdcMem, &rc);
        }

        SetWindowPos(m_hWnd, HWND_TOPMOST, m_screenX, m_screenY, m_canvasWidth, m_canvasHeight, SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

void DockWindow::CreateDIBBuffer(int width, int height) {
    CleanupDIBBuffer();

    HDC hdcScreen = GetDC(NULL);
    m_hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_hBitmap = CreateDIBSection(m_hdcMem, &bmi, DIB_RGB_COLORS, &m_pvBits, NULL, 0);
    m_hOldBitmap = (HBITMAP)SelectObject(m_hdcMem, m_hBitmap);

    ReleaseDC(NULL, hdcScreen);
}

void DockWindow::CleanupDIBBuffer() {
    if (m_hdcMem) {
        if (m_hOldBitmap) SelectObject(m_hdcMem, m_hOldBitmap);
        if (m_hBitmap) DeleteObject(m_hBitmap);
        DeleteDC(m_hdcMem);
        m_hdcMem = nullptr;
        m_hBitmap = nullptr;
        m_pvBits = nullptr;
    }
}

void DockWindow::InitDirect2D() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);

    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_pDWriteFactory)
    );

    if (m_pDWriteFactory) {
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI",
            NULL,
            DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-US",
            &m_pTextFormat
        );
        if (m_pTextFormat) {
            m_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Modern Menu Text (Leading alignment)
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe UI",
            NULL,
            DWRITE_FONT_WEIGHT_REGULAR,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-US",
            &m_pMenuTextFormat
        );
        if (m_pMenuTextFormat) {
            m_pMenuTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_pMenuTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        // Modern Menu Glyph Icons (Segoe MDL2 Assets)
        m_pDWriteFactory->CreateTextFormat(
            L"Segoe MDL2 Assets",
            NULL,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"en-US",
            &m_pMenuIconFormat
        );
        if (m_pMenuIconFormat) {
            m_pMenuIconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_pMenuIconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    m_pD2DFactory->CreateDCRenderTarget(&props, &m_pDCRT);

    RECT rc = { 0, 0, m_canvasWidth, m_canvasHeight };
    if (m_pDCRT && m_hdcMem) {
        m_pDCRT->BindDC(m_hdcMem, &rc);

        m_pDCRT->CreateSolidColorBrush(Config::BG_COLOR, &m_pBgBrush);
        m_pDCRT->CreateSolidColorBrush(Config::BORDER_COLOR, &m_pBorderBrush);
        m_pDCRT->CreateSolidColorBrush(Config::INDICATOR_COLOR, &m_pIndicatorBrush);

        D2D1_COLOR_F activeDot = { 0.40f, 0.80f, 1.0f, 1.0f }; // Glowing cyan for active window
        m_pDCRT->CreateSolidColorBrush(activeDot, &m_pActiveIndicatorBrush);

        D2D1_COLOR_F tooltipBg = { 0.08f, 0.08f, 0.10f, 0.88f };
        m_pDCRT->CreateSolidColorBrush(tooltipBg, &m_pTooltipBgBrush);

        D2D1_COLOR_F tooltipText = { 0.98f, 0.98f, 0.98f, 1.0f };
        m_pDCRT->CreateSolidColorBrush(tooltipText, &m_pTooltipTextBrush);

        // Menu brushes created on m_pDCRT to share resource domain with icon bitmaps
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.96f), &m_pMenuBgBrush);
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.24f, 0.24f, 0.24f, 0.90f), &m_pMenuBorderBrush);
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.22f, 1.0f), &m_pMenuHoverBrush);
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 1.0f), &m_pMenuTextBrush);
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.91f, 0.07f, 0.14f, 1.0f), &m_pMenuCloseRedBrush);
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &m_pMenuWhiteBrush);
        m_pDCRT->CreateSolidColorBrush(D2D1::ColorF(0.22f, 0.22f, 0.22f, 0.8f), &m_pMenuSeparatorBrush);
    }
}

void DockWindow::CleanupDirect2D() {
    if (m_pBgBrush) { m_pBgBrush->Release(); m_pBgBrush = nullptr; }
    if (m_pBorderBrush) { m_pBorderBrush->Release(); m_pBorderBrush = nullptr; }
    if (m_pIndicatorBrush) { m_pIndicatorBrush->Release(); m_pIndicatorBrush = nullptr; }
    if (m_pActiveIndicatorBrush) { m_pActiveIndicatorBrush->Release(); m_pActiveIndicatorBrush = nullptr; }
    if (m_pTooltipBgBrush) { m_pTooltipBgBrush->Release(); m_pTooltipBgBrush = nullptr; }
    if (m_pTooltipTextBrush) { m_pTooltipTextBrush->Release(); m_pTooltipTextBrush = nullptr; }
    if (m_pTextFormat) { m_pTextFormat->Release(); m_pTextFormat = nullptr; }

    // Menu D2D Resources
    if (m_pMenuBgBrush) { m_pMenuBgBrush->Release(); m_pMenuBgBrush = nullptr; }
    if (m_pMenuBorderBrush) { m_pMenuBorderBrush->Release(); m_pMenuBorderBrush = nullptr; }
    if (m_pMenuHoverBrush) { m_pMenuHoverBrush->Release(); m_pMenuHoverBrush = nullptr; }
    if (m_pMenuTextBrush) { m_pMenuTextBrush->Release(); m_pMenuTextBrush = nullptr; }
    if (m_pMenuCloseRedBrush) { m_pMenuCloseRedBrush->Release(); m_pMenuCloseRedBrush = nullptr; }
    if (m_pMenuWhiteBrush) { m_pMenuWhiteBrush->Release(); m_pMenuWhiteBrush = nullptr; }
    if (m_pMenuSeparatorBrush) { m_pMenuSeparatorBrush->Release(); m_pMenuSeparatorBrush = nullptr; }
    if (m_pMenuTextFormat) { m_pMenuTextFormat->Release(); m_pMenuTextFormat = nullptr; }
    if (m_pMenuIconFormat) { m_pMenuIconFormat->Release(); m_pMenuIconFormat = nullptr; }

    if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
    if (m_pDCRT) { m_pDCRT->Release(); m_pDCRT = nullptr; }
    if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
}

void DockWindow::SaveCustomOrder() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring orderFile = exePath;
    size_t lastSlash = orderFile.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        orderFile = orderFile.substr(0, lastSlash + 1) + L"dock_order.txt";
    } else {
        orderFile = L"dock_order.txt";
    }

    FILE* fp = _wfopen(orderFile.c_str(), L"w, ccs=UTF-8");
    if (!fp) return;

    for (const auto& item : m_items) {
        if (item.isPinned) {
            fwprintf(fp, L"%ls\n", item.launchPath.c_str());
        }
    }
    fclose(fp);
}

void DockWindow::LoadCustomOrder(std::vector<PinnedAppInfo>& pinnedApps) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring orderFile = exePath;
    size_t lastSlash = orderFile.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        orderFile = orderFile.substr(0, lastSlash + 1) + L"dock_order.txt";
    } else {
        orderFile = L"dock_order.txt";
    }

    FILE* fp = _wfopen(orderFile.c_str(), L"r, ccs=UTF-8");
    if (!fp) return;

    std::vector<std::wstring> savedPaths;
    wchar_t line[MAX_PATH] = {};
    while (fgetws(line, MAX_PATH, fp)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\r' || line[len - 1] == L'\n')) {
            line[--len] = L'\0';
        }
        if (len > 0) {
            savedPaths.push_back(line);
        }
    }
    fclose(fp);

    if (savedPaths.empty()) return;

    std::vector<PinnedAppInfo> reordered;
    std::vector<bool> used(pinnedApps.size(), false);

    for (const auto& path : savedPaths) {
        for (size_t i = 0; i < pinnedApps.size(); ++i) {
            if (!used[i] && pinnedApps[i].lnkPath == path) {
                reordered.push_back(pinnedApps[i]);
                used[i] = true;
                break;
            }
        }
    }

    for (size_t i = 0; i < pinnedApps.size(); ++i) {
        if (!used[i]) {
            reordered.push_back(pinnedApps[i]);
        }
    }

    pinnedApps = std::move(reordered);
}

void DockWindow::RefreshTaskbarItems() {
    if (m_isDragging || m_isMenuOpen) {
        return; // Never interrupt active drag operation or active context menu
    }

    auto pinnedApps = TaskManager::LoadPinnedApps();
    auto openWindows = TaskManager::GetOpenWindows(m_hWnd);

    if (m_items.empty()) {
        LoadCustomOrder(pinnedApps);
    }

    std::vector<DockItem> newItems;
    newItems.reserve(pinnedApps.size() + openWindows.size());

    std::vector<bool> pinMatched(pinnedApps.size(), false);
    std::vector<bool> windowMatched(openWindows.size(), false);

    // 1. Preserve established items in m_items in their current user-reordered positions
    for (auto& old : m_items) {
        if (old.isPinned) {
            int matchedPin = -1;
            for (size_t p = 0; p < pinnedApps.size(); ++p) {
                if (!pinMatched[p] && (pinnedApps[p].lnkPath == old.launchPath ||
                    (!pinnedApps[p].resolvedExe.empty() && !old.exeFilename.empty() && pinnedApps[p].resolvedExe == old.exeFilename))) {
                    matchedPin = (int)p;
                    break;
                }
            }

            if (matchedPin >= 0) {
                pinMatched[matchedPin] = true;
                const auto& pin = pinnedApps[matchedPin];
                DockItem item;
                item.name = pin.name;
                item.launchPath = pin.lnkPath;
                item.exeFilename = pin.resolvedExe;
                item.isPinned = true;
                item.isRunning = false;
                item.isForeground = false;
                item.hWnd = nullptr;

                for (size_t w = 0; w < openWindows.size(); ++w) {
                    if (!pin.resolvedExe.empty() && openWindows[w].exeFilename == pin.resolvedExe) {
                        item.isRunning = true;
                        DockWindowEntry entry;
                        entry.hWnd = openWindows[w].hWnd;
                        entry.title = openWindows[w].title;
                        entry.isForeground = openWindows[w].isForeground;
                        item.openWindows.push_back(entry);

                        if (!item.hWnd || openWindows[w].isForeground) {
                            item.hWnd = openWindows[w].hWnd;
                            item.windowTitle = openWindows[w].title;
                        }
                        if (openWindows[w].isForeground) {
                            item.isForeground = true;
                        }
                        windowMatched[w] = true;
                    }
                }

                item.pBitmap = old.pBitmap;
                old.pBitmap = nullptr;
                item.currentScale = old.currentScale;
                item.targetScale = old.targetScale;
                item.currentX = old.currentX;
                item.targetX = old.targetX;
                item.x = old.x;
                item.centerX = old.centerX;
                item.bounceY = old.bounceY;
                item.bounceVelocity = old.bounceVelocity;

                newItems.push_back(std::move(item));
            }
        } else {
            // Unpinned running app
            for (size_t w = 0; w < openWindows.size(); ++w) {
                bool match = (openWindows[w].hWnd == old.hWnd);
                if (!match && !old.exeFilename.empty() && !openWindows[w].exeFilename.empty()) {
                    match = (openWindows[w].exeFilename == old.exeFilename);
                }
                if (!windowMatched[w] && match) {
                    DockItem item;
                    item.name = TaskManager::GetAppFriendlyName(openWindows[w].fullExePath, openWindows[w].title);
                    item.windowTitle = openWindows[w].title;
                    item.launchPath = openWindows[w].fullExePath;
                    item.exeFilename = openWindows[w].exeFilename;
                    item.hWnd = openWindows[w].hWnd;
                    item.isPinned = false;
                    item.isRunning = true;
                    item.isForeground = openWindows[w].isForeground;

                    item.pBitmap = old.pBitmap;
                    old.pBitmap = nullptr;
                    item.currentScale = old.currentScale;
                    item.targetScale = old.targetScale;
                    item.currentX = old.currentX;
                    item.targetX = old.targetX;
                    item.x = old.x;
                    item.centerX = old.centerX;
                    item.bounceY = old.bounceY;
                    item.bounceVelocity = old.bounceVelocity;

                    for (size_t k = 0; k < openWindows.size(); ++k) {
                        if (!openWindows[k].exeFilename.empty() && openWindows[k].exeFilename == item.exeFilename) {
                            DockWindowEntry entry;
                            entry.hWnd = openWindows[k].hWnd;
                            entry.title = openWindows[k].title;
                            entry.isForeground = openWindows[k].isForeground;
                            item.openWindows.push_back(entry);

                            if (openWindows[k].isForeground) {
                                item.isForeground = true;
                                item.hWnd = openWindows[k].hWnd;
                                item.windowTitle = openWindows[k].title;
                            }
                            windowMatched[k] = true;
                        }
                    }

                    newItems.push_back(std::move(item));
                    break;
                }
            }
        }
    }

    // 2. Append any pinned apps not yet in m_items (newly pinned)
    for (size_t p = 0; p < pinnedApps.size(); ++p) {
        if (!pinMatched[p]) {
            const auto& pin = pinnedApps[p];
            DockItem item;
            item.name = pin.name;
            item.launchPath = pin.lnkPath;
            item.exeFilename = pin.resolvedExe;
            item.isPinned = true;
            item.isRunning = false;
            item.isForeground = false;
            item.hWnd = nullptr;

            for (size_t w = 0; w < openWindows.size(); ++w) {
                if (!pin.resolvedExe.empty() && openWindows[w].exeFilename == pin.resolvedExe) {
                    item.isRunning = true;
                    DockWindowEntry entry;
                    entry.hWnd = openWindows[w].hWnd;
                    entry.title = openWindows[w].title;
                    entry.isForeground = openWindows[w].isForeground;
                    item.openWindows.push_back(entry);

                    if (!item.hWnd || openWindows[w].isForeground) {
                        item.hWnd = openWindows[w].hWnd;
                        item.windowTitle = openWindows[w].title;
                    }
                    if (openWindows[w].isForeground) {
                        item.isForeground = true;
                    }
                    windowMatched[w] = true;
                }
            }

            item.pBitmap = IconHelper::CreateIconBitmap(m_pDCRT, pin.lnkPath, L"", pin.name);
            newItems.push_back(std::move(item));
        }
    }

    // 3. Append genuinely new open windows to the end
    for (size_t w = 0; w < openWindows.size(); ++w) {
        if (!windowMatched[w]) {
            const auto& win = openWindows[w];
            DockItem item;
            item.name = TaskManager::GetAppFriendlyName(win.fullExePath, win.title);
            item.windowTitle = win.title;
            item.launchPath = win.fullExePath;
            item.exeFilename = win.exeFilename;
            item.hWnd = win.hWnd;
            item.isPinned = false;
            item.isRunning = true;
            item.isForeground = win.isForeground;

            item.pBitmap = IconHelper::CreateIconFromHWND(m_pDCRT, win.hWnd, win.fullExePath, item.name);

            for (size_t k = w; k < openWindows.size(); ++k) {
                if (!openWindows[k].exeFilename.empty() && openWindows[k].exeFilename == item.exeFilename) {
                    DockWindowEntry entry;
                    entry.hWnd = openWindows[k].hWnd;
                    entry.title = openWindows[k].title;
                    entry.isForeground = openWindows[k].isForeground;
                    item.openWindows.push_back(entry);

                    if (openWindows[k].isForeground) {
                        item.isForeground = true;
                        item.hWnd = openWindows[k].hWnd;
                        item.windowTitle = openWindows[k].title;
                    }
                    windowMatched[k] = true;
                }
            }

            newItems.push_back(std::move(item));
        }
    }

    // Pre-cache snapshots of active/visible open windows so animation is instant and ready
    for (const auto& win : openWindows) {
        if (win.hWnd && IsWindow(win.hWnd) && !IsIconic(win.hWnd) && IsWindowVisible(win.hWnd)) {
            m_animator.PrecacheWindowSnapshot(win.hWnd);
        }
    }

    // Check if anything actually changed (item count, running states, foreground states, window count)
    bool hasChanged = (m_items.size() != newItems.size());
    if (!hasChanged) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i].launchPath != newItems[i].launchPath ||
                m_items[i].isRunning != newItems[i].isRunning ||
                m_items[i].isForeground != newItems[i].isForeground ||
                m_items[i].hWnd != newItems[i].hWnd ||
                m_items[i].name != newItems[i].name ||
                m_items[i].openWindows.size() != newItems[i].openWindows.size()) {
                hasChanged = true;
                break;
            }
        }
    }

    // ALWAYS transfer newItems into m_items so pBitmap ownership is never lost
    m_items = std::move(newItems);

    // Adjust canvas size dynamically
    ResizeCanvas(m_items.size());

    // Check if preview target windows are still open and running
    if (m_isPreviewOpen) {
        bool cardRemoved = false;
        for (auto it = m_previewCards.begin(); it != m_previewCards.end(); ) {
            if (!it->hWnd || !IsWindow(it->hWnd)) {
                if (it->pThumbnail) {
                    it->pThumbnail->Release();
                    it->pThumbnail = nullptr;
                }
                it = m_previewCards.erase(it);
                cardRemoved = true;
            } else {
                ++it;
            }
        }
        if (m_previewCards.empty()) {
            HidePreviewPanelImmediate();
        } else if (cardRemoved) {
            RECT rcWork = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

            size_t N = m_previewCards.size();
            float cardW = 216.0f;
            float cardH = 160.0f;
            float pad = 8.0f;
            float gap = 8.0f;
            if (N == 1) {
                cardW = 235.0f;
                cardH = 167.0f;
                pad = 0.5f;
                gap = 0.0f;
            }
            int totalWidth = (int)std::ceil((pad * 2.0f) + ((float)N * cardW) + ((float)(N - 1) * gap));
            int totalHeight = (int)std::ceil((pad * 2.0f) + cardH);

            m_previewWidth = totalWidth;
            m_previewHeight = totalHeight;

            if (m_previewAppIndex >= 0 && m_previewAppIndex < (int)m_items.size()) {
                float iconScreenCenterX = (float)m_screenX + m_items[m_previewAppIndex].centerX;
                int previewLeft = (int)std::round(iconScreenCenterX - (float)totalWidth / 2.0f);
                if (previewLeft < rcWork.left + 8) previewLeft = rcWork.left + 8;
                if (previewLeft + totalWidth > rcWork.right - 8) previewLeft = rcWork.right - 8 - totalWidth;
                m_previewScreenX = previewLeft;
                m_previewCurrentX = (float)previewLeft;
                m_previewTargetX = (float)previewLeft;
            }

            for (size_t i = 0; i < m_previewCards.size(); ++i) {
                float cx = pad + (float)i * (cardW + gap);
                float cy = pad;
                m_previewCards[i].cardRect = D2D1::RectF(cx, cy, cx + cardW, cy + cardH);
                m_previewCards[i].closeBtnRect = D2D1::RectF(cx + cardW - 28.0f, cy + 4.0f, cx + cardW - 4.0f, cy + 26.0f);
                m_previewCards[i].thumbRect = D2D1::RectF(cx + 6.0f, cy + 30.0f, cx + cardW - 6.0f, cy + cardH - 6.0f);
            }

            m_previewHoveredCard = -1;
            m_previewCloseHovered = false;
            m_previewThumbHovered = false;

            SetWindowPos(m_hPreviewWnd, HWND_TOPMOST, m_previewScreenX, m_previewScreenY, m_previewWidth, m_previewHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            RenderPreviewPanel();
        }
    }

    // Update layout
    UpdateLayout(m_isMouseOverDock);

    // Only re-render if state actually changed or we are actively animating
    if (hasChanged || m_isAnimating) {
        Render();
    }
}

void DockWindow::UpdateLayout(bool checkHover) {
    float totalItemsWidth = 0.0f;
    for (const auto& item : m_items) {
        totalItemsWidth += (Config::BASE_ICON_SIZE * item.currentScale);
    }

    float totalSpacing = (m_items.empty() ? 0.0f : (m_items.size() - 1) * Config::ITEM_SPACING);
    float dockWidth = totalItemsWidth + totalSpacing + Config::DOCK_PADDING_X * 2.0f;

    float pillHeight = Config::BASE_ICON_SIZE + Config::DOCK_PADDING_Y * 2.0f;
    float pillBottom = (float)m_canvasHeight - Config::BOTTOM_MARGIN;
    float pillTop = pillBottom - pillHeight;
    float pillLeft = ((float)m_canvasWidth - dockWidth) / 2.0f;
    float pillRight = pillLeft + dockWidth;

    m_dockPillRect = D2D1::RoundedRect(
        D2D1::RectF(pillLeft, pillTop, pillRight, pillBottom),
        Config::CORNER_RADIUS,
        Config::CORNER_RADIUS
    );

    float slotX = pillLeft + Config::DOCK_PADDING_X;
    m_hoveredIndex = -1;

    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& item = m_items[i];
        float itemW = Config::BASE_ICON_SIZE * item.currentScale;
        float itemH = itemW;

        item.targetX = slotX;
        if (item.currentX <= 0.0f) {
            item.currentX = item.targetX;
            item.x = item.targetX;
            item.centerX = item.currentX + (itemW / 2.0f);
        }

        item.width = itemW;
        item.height = itemH;

        float baseBottom = pillBottom - Config::DOCK_PADDING_Y;
        item.y = baseBottom - itemH - item.bounceY;

        if (checkHover && m_isMouseOverDock && !m_isDragging) {
            if (m_mouseX >= item.x && m_mouseX <= (item.x + item.width) &&
                m_mouseY >= item.y && m_mouseY <= (item.y + item.height + Config::DOCK_PADDING_Y)) {
                m_hoveredIndex = (int)i;
            }
        }

        slotX += itemW + Config::ITEM_SPACING;
    }
}

void DockWindow::Render() {
    if (!m_pDCRT) return;

    RECT rc = { 0, 0, m_canvasWidth, m_canvasHeight };
    m_pDCRT->BindDC(m_hdcMem, &rc);

    m_pDCRT->BeginDraw();
    m_pDCRT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // 1. Draw Dock Pill Glass Background
    m_pDCRT->FillRoundedRectangle(&m_dockPillRect, m_pBgBrush);
    m_pDCRT->DrawRoundedRectangle(&m_dockPillRect, m_pBorderBrush, Config::BORDER_WIDTH);

    // 2. Draw Icons and Running Indicators
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_isDragging && (int)i == m_draggedIndex) {
            continue; // Skip dragged item, will be rendered on top
        }

        const auto& item = m_items[i];
        if (item.pBitmap) {
            D2D1_RECT_F dst = D2D1::RectF(item.x, item.y, item.x + item.width, item.y + item.height);
            m_pDCRT->DrawBitmap(item.pBitmap, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        // Indicator dot below running application
        if (item.isRunning) {
            float dotY = m_dockPillRect.rect.bottom - 5.5f;
            if (item.isForeground) {
                // Sleek active pill
                D2D1_ROUNDED_RECT activeCapsule = D2D1::RoundedRect(
                    D2D1::RectF(item.centerX - 5.0f, dotY - 1.5f, item.centerX + 5.0f, dotY + 1.5f),
                    1.5f, 1.5f
                );
                m_pDCRT->FillRoundedRectangle(&activeCapsule, m_pActiveIndicatorBrush);
            } else {
                D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(item.centerX, dotY), 2.2f, 2.2f);
                m_pDCRT->FillEllipse(dot, m_pIndicatorBrush);
            }
        }
    }

    // Draw dragged item on top floating with cursor!
    if (m_isDragging && m_draggedIndex >= 0 && m_draggedIndex < (int)m_items.size()) {
        const auto& item = m_items[m_draggedIndex];
        if (item.pBitmap) {
            float dragW = Config::BASE_ICON_SIZE * 1.25f;
            float dragH = dragW;
            float dragX = m_mouseX - (dragW / 2.0f);
            float baseBottom = m_dockPillRect.rect.bottom - Config::DOCK_PADDING_Y;
            float dragY = baseBottom - dragH - 8.0f; // Floating 8px up
            D2D1_RECT_F dst = D2D1::RectF(dragX, dragY, dragX + dragW, dragY + dragH);
            m_pDCRT->DrawBitmap(item.pBitmap, dst, 0.95f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    }

    // 3. Draw Tooltip Badge for Hovered App (hidden during drag, or for running apps with preview)
    if (!m_isDragging && !m_isPreviewOpen && m_hoveredIndex >= 0 && m_hoveredIndex < (int)m_items.size()) {
        const auto& item = m_items[m_hoveredIndex];
        // Only show text tooltip badge for non-running pinned apps
        if (!item.isRunning || !item.hWnd) {
            const std::wstring& label = item.name;

            // Truncate tooltip if window title is very long
            std::wstring displayLabel = label;
            if (displayLabel.length() > 32) {
                displayLabel = displayLabel.substr(0, 30) + L"...";
            }

            float textW = (float)displayLabel.length() * 8.0f + 22.0f;
            float textH = 24.0f;
            float ttLeft = item.centerX - (textW / 2.0f);
            float ttRight = ttLeft + textW;
            float ttBottom = item.y - 8.0f;
            float ttTop = ttBottom - textH;

            D2D1_ROUNDED_RECT ttRect = D2D1::RoundedRect(
                D2D1::RectF(ttLeft, ttTop, ttRight, ttBottom),
                6.0f, 6.0f
            );

            m_pDCRT->FillRoundedRectangle(&ttRect, m_pTooltipBgBrush);
            m_pDCRT->DrawRoundedRectangle(&ttRect, m_pBorderBrush, 0.8f);

            if (m_pTextFormat) {
                D2D1_RECT_F layoutRect = D2D1::RectF(ttLeft, ttTop, ttRight, ttBottom);
                m_pDCRT->DrawTextW(
                    displayLabel.c_str(),
                    (UINT32)displayLabel.length(),
                    m_pTextFormat,
                    layoutRect,
                    m_pTooltipTextBrush
                );
            }
        }
    }

    HRESULT hr = m_pDCRT->EndDraw();
    if (FAILED(hr)) return;

    // Update Layered Window with Premultiplied Alpha
    HDC hdcScreen = GetDC(NULL);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { m_canvasWidth, m_canvasHeight };
    POINT ptDst = { m_screenX, m_screenY };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        m_hWnd,
        hdcScreen,
        &ptDst,
        &sz,
        m_hdcMem,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );

    ReleaseDC(NULL, hdcScreen);
}

void DockWindow::OnMouseMove(int x, int y) {
    m_mouseX = (float)x;
    m_mouseY = (float)y;

    if (!m_isMouseOverDock) {
        m_isMouseOverDock = true;
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT) };
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hWnd;
        TrackMouseEvent(&tme);

        if (!m_isAnimating) {
            m_isAnimating = true;
            SetTimer(m_hWnd, TIMER_ANIM, 16, NULL); // 60 FPS tick
        }
    }

    // Check if dragging should start
    if (m_isLButtonDown && m_dragCandidateIndex >= 0 && !m_isDragging) {
        float dx = m_mouseX - m_dragStartX;
        float dy = m_mouseY - m_dragStartY;
        if (std::sqrt(dx * dx + dy * dy) > 8.0f) {
            m_isDragging = true;
            m_draggedIndex = m_dragCandidateIndex;
            if (!m_isAnimating) {
                m_isAnimating = true;
                SetTimer(m_hWnd, TIMER_ANIM, 16, NULL);
            }
        }
    }

    // Handle active dragging & real-time slot swapping
    if (m_isDragging && m_draggedIndex >= 0 && m_draggedIndex < (int)m_items.size()) {
        // Dragging to the left: swap when cursor is to the left of the neighbor's slot center
        while (m_draggedIndex > 0) {
            float leftSlotCenter = m_items[m_draggedIndex - 1].targetX + (m_items[m_draggedIndex - 1].width / 2.0f);
            if (m_mouseX < leftSlotCenter) {
                std::swap(m_items[m_draggedIndex], m_items[m_draggedIndex - 1]);
                m_draggedIndex--;
                UpdateLayout(false);
            } else {
                break;
            }
        }

        // Dragging to the right: swap when cursor is to the right of the neighbor's slot center
        while (m_draggedIndex + 1 < (int)m_items.size()) {
            float rightSlotCenter = m_items[m_draggedIndex + 1].targetX + (m_items[m_draggedIndex + 1].width / 2.0f);
            if (m_mouseX > rightSlotCenter) {
                std::swap(m_items[m_draggedIndex], m_items[m_draggedIndex + 1]);
                m_draggedIndex++;
                UpdateLayout(false);
            } else {
                break;
            }
        }
    }

    // Calculate Magnification Target Scales
    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& item = m_items[i];
        if (m_isDragging) {
            if ((int)i == m_draggedIndex) {
                item.targetScale = 1.25f;
            } else {
                item.targetScale = 1.0f;
            }
            continue;
        }

        float dist = std::fabs(m_mouseX - item.centerX);
        if (dist < Config::INFLUENCE_RADIUS) {
            float norm = dist / Config::INFLUENCE_RADIUS;
            float factor = std::cos(norm * 3.14159265f * 0.5f);
            factor = factor * factor; // Cosine-squared curve
            item.targetScale = 1.0f + (Config::MAX_ICON_SCALE - 1.0f) * factor;
        } else {
            item.targetScale = 1.0f;
        }
    }

    // Window App Preview Panel hover tracking (Fluent Windows style)
    if (!m_isDragging && !m_isMenuOpen) {
        int hoveredApp = -1;
        for (size_t i = 0; i < m_items.size(); ++i) {
            const auto& item = m_items[i];
            if (m_mouseX >= item.x && m_mouseX <= (item.x + item.width) &&
                m_mouseY >= item.y && m_mouseY <= (item.y + item.height + Config::DOCK_PADDING_Y)) {
                if (item.isRunning && item.hWnd && IsWindow(item.hWnd)) {
                    hoveredApp = (int)i;
                }
                break;
            }
        }

        if (hoveredApp >= 0) {
            if (m_isPreviewOpen || m_isPreviewClosing) {
                if (hoveredApp != m_previewAppIndex || m_isPreviewClosing) {
                    KillTimer(m_hWnd, TIMER_PREVIEW_CLOSE);
                    ShowPreviewPanel(hoveredApp);
                } else {
                    KillTimer(m_hWnd, TIMER_PREVIEW_CLOSE);
                    float iconScreenCenterX = (float)m_screenX + m_items[hoveredApp].centerX;
                    int previewLeft = (int)std::round(iconScreenCenterX - (float)m_previewWidth / 2.0f);
                    RECT rcWork = {};
                    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
                    if (previewLeft < rcWork.left + 8) previewLeft = rcWork.left + 8;
                    if (previewLeft + m_previewWidth > rcWork.right - 8) previewLeft = rcWork.right - 8 - m_previewWidth;
                    if (std::fabs(m_previewTargetX - (float)previewLeft) > 0.5f) {
                        m_previewTargetX = (float)previewLeft;
                        SetTimer(m_hWnd, TIMER_PREVIEW_ANIM, 16, NULL);
                    }
                }
            } else {
                KillTimer(m_hWnd, TIMER_PREVIEW_CLOSE);
                SetTimer(m_hWnd, TIMER_PREVIEW_HOVER, 250, NULL);
            }
        } else {
            KillTimer(m_hWnd, TIMER_PREVIEW_HOVER);
            if (m_isPreviewOpen && !m_isPreviewClosing) {
                SetTimer(m_hWnd, TIMER_PREVIEW_CLOSE, 200, NULL);
            }
        }
    }
}

void DockWindow::OnMouseLeave() {
    if (m_isDragging) {
        return; // Don't cancel active drag if mouse briefly slips outside
    }

    KillTimer(m_hWnd, TIMER_PREVIEW_HOVER);
    if (m_isPreviewOpen) {
        SetTimer(m_hWnd, TIMER_PREVIEW_CLOSE, 250, NULL);
    }

    m_isMouseOverDock = false;
    m_hoveredIndex = -1;
    m_mouseX = -9999.0f;
    m_mouseY = -9999.0f;

    for (auto& item : m_items) {
        item.targetScale = 1.0f;
    }

    if (!m_isAnimating) {
        m_isAnimating = true;
        SetTimer(m_hWnd, TIMER_ANIM, 16, NULL);
    }
}

void DockWindow::OnTimer() {
    bool stillAnimating = false;

    for (size_t i = 0; i < m_items.size(); ++i) {
        auto& item = m_items[i];

        // 1. Smooth scale animation
        float diff = item.targetScale - item.currentScale;
        if (std::fabs(diff) > Config::SETTLE_THRESHOLD) {
            item.currentScale += diff * Config::LERP_FACTOR;
            stillAnimating = true;
        } else {
            item.currentScale = item.targetScale;
        }

        // 2. Smooth horizontal slide animation (displaced icons gliding smoothly)!
        float diffX = item.targetX - item.currentX;
        if (std::fabs(diffX) > 0.5f) {
            item.currentX += diffX * 0.32f;
            stillAnimating = true;
        } else {
            item.currentX = item.targetX;
        }
        item.x = item.currentX;
        item.centerX = item.currentX + (item.width / 2.0f);

        // 3. Vertical bounce animation
        if (item.bounceY > 0.0f || item.bounceVelocity != 0.0f) {
            item.bounceY += item.bounceVelocity;
            item.bounceVelocity -= 1.8f;
            if (item.bounceY <= 0.0f) {
                item.bounceY = 0.0f;
                item.bounceVelocity = 0.0f;
            } else {
                stillAnimating = true;
            }
        }
    }

    UpdateLayout(!m_isDragging);

    // Keep preview target position smoothly following icon as it scales/slides
    if (m_isPreviewOpen && m_previewAppIndex >= 0 && m_previewAppIndex < (int)m_items.size()) {
        float iconScreenCenterX = (float)m_screenX + m_items[m_previewAppIndex].centerX;
        int previewLeft = (int)std::round(iconScreenCenterX - (float)m_previewWidth / 2.0f);
        RECT rcWork = {};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
        if (previewLeft < rcWork.left + 8) previewLeft = rcWork.left + 8;
        if (previewLeft + m_previewWidth > rcWork.right - 8) previewLeft = rcWork.right - 8 - m_previewWidth;
        if (std::fabs(m_previewTargetX - (float)previewLeft) > 0.5f) {
            m_previewTargetX = (float)previewLeft;
            SetTimer(m_hWnd, TIMER_PREVIEW_ANIM, 16, NULL);
        }
    }

    Render();

    if (!m_isMouseOverDock && !m_isDragging && !stillAnimating) {
        KillTimer(m_hWnd, TIMER_ANIM);
        m_isAnimating = false;
    }
}

void DockWindow::OnLButtonDown(int x, int y) {
    HidePreviewPanelImmediate();
    m_isLButtonDown = true;
    m_dragStartX = (float)x;
    m_dragStartY = (float)y;
    m_dragCandidateIndex = -1;
    m_isDragging = false;
    m_draggedIndex = -1;

    HWND fg = GetForegroundWindow();
    if (fg && fg != m_hWnd && fg != m_hMenuWnd && fg != m_animator.GetAnimWnd()) {
        m_hForegroundBeforeClick = fg;
        m_lastActiveHWnd = GetAncestor(fg, GA_ROOT);
    }

    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];
        if (x >= (int)item.x && x <= (int)(item.x + item.width) &&
            y >= (int)item.y && y <= (int)(item.y + item.height + Config::DOCK_PADDING_Y)) {
            m_dragCandidateIndex = (int)i;
            break;
        }
    }

    if (m_dragCandidateIndex >= 0) {
        SetCapture(m_hWnd);
    }
}

void DockWindow::OnLButtonUp(int x, int y) {
    if (GetCapture() == m_hWnd) {
        ReleaseCapture();
    }
    m_isLButtonDown = false;

    if (m_isDragging) {
        // Dragged item glides smoothly to target slot
        if (m_draggedIndex >= 0 && m_draggedIndex < (int)m_items.size()) {
            m_items[m_draggedIndex].currentX = m_mouseX - (m_items[m_draggedIndex].width / 2.0f);
            m_items[m_draggedIndex].x = m_items[m_draggedIndex].currentX;
        }

        m_isDragging = false;
        m_draggedIndex = -1;
        m_dragCandidateIndex = -1;

        for (auto& item : m_items) {
            item.targetScale = 1.0f;
        }

        SaveCustomOrder();

        UpdateLayout(true);
        if (!m_isAnimating) {
            m_isAnimating = true;
            SetTimer(m_hWnd, TIMER_ANIM, 16, NULL);
        }
        Render();
        return;
    }

    m_dragCandidateIndex = -1;

    int targetIndex = -1;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];
        if (x >= (int)item.x && x <= (int)(item.x + item.width) &&
            y >= (int)item.y && y <= (int)(item.y + item.height + Config::DOCK_PADDING_Y)) {
            targetIndex = (int)i;
            break;
        }
    }
    if (targetIndex < 0 && m_hoveredIndex >= 0 && m_hoveredIndex < (int)m_items.size()) {
        targetIndex = m_hoveredIndex;
    }

    if (targetIndex >= 0 && targetIndex < (int)m_items.size()) {
        auto& item = m_items[targetIndex];

        // If window is open, toggle window state with macOS Genie animation on window app (dock stays still)
        if (item.hWnd && IsWindow(item.hWnd)) {
            bool wasFg = item.isForeground;
            HWND hWnd = item.hWnd;
            float iconX = (float)m_screenX + item.centerX;
            float iconY = (float)m_screenY + item.y + (item.height / 2.0f);
            float iconW = item.width;

            HWND curFg = GetForegroundWindow();
            HWND rootFg = curFg ? GetAncestor(curFg, GA_ROOT) : NULL;
            HWND preFg = m_hForegroundBeforeClick;
            HWND rootPreFg = preFg ? GetAncestor(preFg, GA_ROOT) : NULL;

            bool isCurrentlyActive = (hWnd == curFg || hWnd == rootFg ||
                                      hWnd == preFg || hWnd == rootPreFg ||
                                      hWnd == m_lastActiveHWnd ||
                                      wasFg);

            // Also check if this unminimized window is top-most visible on desktop
            if (!isCurrentlyActive && !IsIconic(hWnd)) {
                HWND topWnd = GetTopWindow(GetDesktopWindow());
                while (topWnd && (!IsWindowVisible(topWnd) || IsIconic(topWnd) || topWnd == m_hWnd || topWnd == m_hMenuWnd || topWnd == m_animator.GetAnimWnd())) {
                    topWnd = GetNextWindow(topWnd, GW_HWNDNEXT);
                }
                if (topWnd == hWnd) {
                    isCurrentlyActive = true;
                }
            }

            bool isMinimized = IsIconic(hWnd);

            if (isMinimized) {
                // Animate window restoring out of dock icon
                if (!m_animator.AnimateWindow(hWnd, iconX, iconY, iconW, false /* isRestoring */)) {
                    TaskManager::ToggleWindowState(hWnd, true);
                }
                m_lastActiveHWnd = hWnd;
                item.isForeground = true;
            } else if (isCurrentlyActive) {
                // Animate window minimizing into dock icon
                if (!m_animator.AnimateWindow(hWnd, iconX, iconY, iconW, true /* isMinimizing */)) {
                    TaskManager::ToggleWindowState(hWnd, true);
                }
                m_lastActiveHWnd = NULL;
                item.isForeground = false;
            } else {
                // Just bring background window to front
                DWORD curThread = GetCurrentThreadId();
                DWORD targetThread = GetWindowThreadProcessId(hWnd, NULL);
                if (curThread != targetThread) {
                    AttachThreadInput(curThread, targetThread, TRUE);
                    SetForegroundWindow(hWnd);
                    SetFocus(hWnd);
                    AttachThreadInput(curThread, targetThread, FALSE);
                } else {
                    SetForegroundWindow(hWnd);
                    SetFocus(hWnd);
                }
                m_lastActiveHWnd = hWnd;
                item.isForeground = true;
            }
            // Brief delay to refresh active dot indicator
            SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 300, NULL);
        } else {
            // Launch application via shortcut or exe path
            ShellExecuteW(
                NULL,
                L"open",
                item.launchPath.c_str(),
                NULL,
                NULL,
                SW_SHOWNORMAL
            );
            // Check for newly opened window shortly after launch
            SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 1000, NULL);
        }
    }
}

enum DockMenuCmd {
    CMD_DOCK_TITLE = 1000,
    CMD_DOCK_REFRESH,
    CMD_DOCK_EXIT,

    CMD_APP_LAUNCH_NEW = 2000,
    CMD_APP_PIN,
    CMD_APP_UNPIN,
    CMD_APP_CLOSE_WINDOW,
    CMD_APP_CLOSE_ALL,

    CMD_WINDOW_SELECT_BASE = 3000,

    CMD_SYS_RESTORE = 4001,
    CMD_SYS_MOVE = 4002,
    CMD_SYS_SIZE = 4003,
    CMD_SYS_MINIMIZE = 4004,
    CMD_SYS_MAXIMIZE = 4005,
    CMD_SYS_CLOSE = 4006
};

HHOOK DockWindow::s_hMenuMouseHook = NULL;
HHOOK DockWindow::s_hMenuKbdHook = NULL;
DockWindow* DockWindow::s_pMenuInstance = nullptr;

RECT DockWindow::GetMenuScreenRect() const {
    RECT rc = {
        m_menuScreenX,
        m_menuScreenY,
        m_menuScreenX + m_menuWidth,
        m_menuScreenY + m_menuHeight
    };
    return rc;
}

LRESULT CALLBACK DockWindow::LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && s_pMenuInstance && s_pMenuInstance->m_isMenuOpen) {
        if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
            wParam == WM_MBUTTONDOWN || wParam == WM_NCLBUTTONDOWN ||
            wParam == WM_NCRBUTTONDOWN || wParam == WM_NCMBUTTONDOWN ||
            wParam == WM_MOUSEWHEEL) {

            auto* pMouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            RECT rcMenu = s_pMenuInstance->GetMenuScreenRect();
            if (!PtInRect(&rcMenu, pMouse->pt)) {
                // Click occurred anywhere outside the context menu!
                s_pMenuInstance->CloseModernMenu();
            }
        }
    }
    return CallNextHookEx(s_hMenuMouseHook, nCode, wParam, lParam);
}

LRESULT CALLBACK DockWindow::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && s_pMenuInstance && s_pMenuInstance->m_isMenuOpen) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            auto* pKbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (pKbd->vkCode == VK_ESCAPE) {
                s_pMenuInstance->CloseModernMenu();
                return 1; // Consume Escape key
            }
        }
    }
    return CallNextHookEx(s_hMenuKbdHook, nCode, wParam, lParam);
}

LRESULT CALLBACK DockWindow::MenuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DockWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<DockWindow*>(pCreate->lpCreateParams);
        if (pThis) {
            pThis->m_hMenuWnd = hWnd;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    } else {
        pThis = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }
    if (pThis) {
        return pThis->HandleMenuMessage(hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT DockWindow::HandleMenuMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);

        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc = { 0, 0, m_menuWidth, m_menuHeight };
        if (PtInRect(&rc, pt)) {
            int newHover = -1;
            float curY = 4.0f;
            for (size_t i = 0; i < m_menuItems.size(); ++i) {
                float h = m_menuItems[i].isSeparator ? 8.0f : 32.0f;
                if ((float)pt.y >= curY && (float)pt.y < curY + h) {
                    if (!m_menuItems[i].isSeparator && m_menuItems[i].id != 0) {
                        newHover = (int)i;
                    }
                    break;
                }
                curY += h;
            }
            if (newHover != m_menuHoverIndex) {
                m_menuHoverIndex = newHover;
                RenderMenu();
            }
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (m_menuHoverIndex != -1) {
            m_menuHoverIndex = -1;
            RenderMenu();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc = { 0, 0, m_menuWidth, m_menuHeight };
        if (PtInRect(&rc, pt)) {
            float curY = 4.0f;
            int clickedCmd = 0;
            HWND targetHWnd = nullptr;
            for (size_t i = 0; i < m_menuItems.size(); ++i) {
                float h = m_menuItems[i].isSeparator ? 8.0f : 32.0f;
                if ((float)pt.y >= curY && (float)pt.y < curY + h) {
                    if (!m_menuItems[i].isSeparator && m_menuItems[i].id != 0) {
                        clickedCmd = m_menuItems[i].id;
                        targetHWnd = m_menuItems[i].targetHWnd;
                    }
                    break;
                }
                curY += h;
            }
            CloseModernMenu();
            if (clickedCmd != 0) {
                ExecuteMenuCommand(clickedCmd, targetHWnd);
            }
        } else {
            CloseModernMenu();
        }
        return 0;
    }

    case WM_RBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc = { 0, 0, m_menuWidth, m_menuHeight };
        if (!PtInRect(&rc, pt)) {
            POINT screenPt = pt;
            ClientToScreen(hWnd, &screenPt);
            CloseModernMenu();
            POINT dockPt = screenPt;
            ScreenToClient(m_hWnd, &dockPt);
            if (HitTest(dockPt.x, dockPt.y)) {
                OnRButtonUp(dockPt.x, dockPt.y);
            }
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc = { 0, 0, m_menuWidth, m_menuHeight };
        if (!PtInRect(&rc, pt)) {
            POINT screenPt = pt;
            ClientToScreen(hWnd, &screenPt);
            CloseModernMenu();
            POINT dockPt = screenPt;
            ScreenToClient(m_hWnd, &dockPt);
            if (HitTest(dockPt.x, dockPt.y)) {
                OnLButtonDown(dockPt.x, dockPt.y);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            CloseModernMenu();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
    case WM_KILLFOCUS:
        CloseModernMenu();
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void DockWindow::InitMenuWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = DockWindow::MenuWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LiteDockMenuWindowClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DROPSHADOW;

    RegisterClassExW(&wc);

    m_hMenuWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"LiteDockMenu",
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, hInstance, this
    );
}

void DockWindow::CleanupMenuWindow() {
    if (s_hMenuMouseHook) {
        UnhookWindowsHookEx(s_hMenuMouseHook);
        s_hMenuMouseHook = NULL;
    }
    if (s_hMenuKbdHook) {
        UnhookWindowsHookEx(s_hMenuKbdHook);
        s_hMenuKbdHook = NULL;
    }
    if (m_hMenuBitmap) {
        if (m_hdcMenuMem && m_hMenuOldBitmap) SelectObject(m_hdcMenuMem, m_hMenuOldBitmap);
        DeleteObject(m_hMenuBitmap);
        m_hMenuBitmap = nullptr;
    }
    if (m_hdcMenuMem) {
        DeleteDC(m_hdcMenuMem);
        m_hdcMenuMem = nullptr;
    }
    if (m_hMenuWnd) {
        DestroyWindow(m_hMenuWnd);
        m_hMenuWnd = nullptr;
    }
}

void DockWindow::CreateMenuDIBBuffer(int width, int height) {
    if (m_hMenuBitmap) {
        if (m_hdcMenuMem && m_hMenuOldBitmap) SelectObject(m_hdcMenuMem, m_hMenuOldBitmap);
        DeleteObject(m_hMenuBitmap);
        m_hMenuBitmap = nullptr;
    }
    if (m_hdcMenuMem) {
        DeleteDC(m_hdcMenuMem);
        m_hdcMenuMem = nullptr;
    }

    HDC hdcScreen = GetDC(NULL);
    m_hdcMenuMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvBits = nullptr;
    m_hMenuBitmap = CreateDIBSection(m_hdcMenuMem, &bmi, DIB_RGB_COLORS, &pvBits, NULL, 0);
    m_hMenuOldBitmap = (HBITMAP)SelectObject(m_hdcMenuMem, m_hMenuBitmap);

    ReleaseDC(NULL, hdcScreen);
}

void DockWindow::CloseModernMenu() {
    if (m_isMenuOpen) {
        if (s_hMenuMouseHook) {
            UnhookWindowsHookEx(s_hMenuMouseHook);
            s_hMenuMouseHook = NULL;
        }
        if (s_hMenuKbdHook) {
            UnhookWindowsHookEx(s_hMenuKbdHook);
            s_hMenuKbdHook = NULL;
        }
        if (GetCapture() == m_hMenuWnd) {
            ReleaseCapture();
        }
        ShowWindow(m_hMenuWnd, SW_HIDE);
        m_isMenuOpen = false;
        m_menuHoverIndex = -1;

        // Restore mouse hover states
        POINT curPt;
        GetCursorPos(&curPt);
        POINT clientPt = curPt;
        ScreenToClient(m_hWnd, &clientPt);
        if (HitTest(clientPt.x, clientPt.y)) {
            m_mouseX = (float)clientPt.x;
            m_mouseY = (float)clientPt.y;
            m_isMouseOverDock = true;
        } else {
            m_isMouseOverDock = false;
            m_hoveredIndex = -1;
            m_mouseX = -9999.0f;
            m_mouseY = -9999.0f;
            for (auto& item : m_items) {
                item.targetScale = 1.0f;
            }
        }
        if (!m_isAnimating) {
            m_isAnimating = true;
            SetTimer(m_hWnd, TIMER_ANIM, 16, NULL);
        }
    }
}

void DockWindow::RenderMenu() {
    if (!m_pDCRT || !m_hdcMenuMem || m_menuWidth <= 0 || m_menuHeight <= 0) return;

    RECT rc = { 0, 0, m_menuWidth, m_menuHeight };
    m_pDCRT->BindDC(m_hdcMenuMem, &rc);
    m_pDCRT->BeginDraw();
    m_pDCRT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // 1. Draw Glass / Acrylic Menu Background (matching native Windows Taskbar dark mode)
    D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, (float)m_menuWidth - 0.5f, (float)m_menuHeight - 0.5f),
        4.0f, 4.0f
    );
    m_pDCRT->FillRoundedRectangle(&bgRect, m_pMenuBgBrush);
    m_pDCRT->DrawRoundedRectangle(&bgRect, m_pMenuBorderBrush, 1.0f);

    // 2. Draw Items
    float curY = 4.0f;
    for (size_t i = 0; i < m_menuItems.size(); ++i) {
        const auto& item = m_menuItems[i];
        if (item.isSeparator) {
            float sepY = curY + 4.0f;
            m_pDCRT->DrawLine(
                D2D1::Point2F(10.0f, sepY),
                D2D1::Point2F((float)m_menuWidth - 10.0f, sepY),
                m_pMenuSeparatorBrush,
                1.0f
            );
            curY += 8.0f;
            continue;
        }

        float rowH = 32.0f;
        if ((int)i == m_menuHoverIndex) {
            D2D1_ROUNDED_RECT hoverRect = D2D1::RoundedRect(
                D2D1::RectF(4.0f, curY + 1.0f, (float)m_menuWidth - 4.0f, curY + rowH - 1.0f),
                3.0f, 3.0f
            );
            m_pDCRT->FillRoundedRectangle(&hoverRect, m_pMenuHoverBrush);
        }

        float iconBoxX = 14.0f;
        float iconBoxY = curY + (rowH - 16.0f) / 2.0f;

        // Draw Icon
        if (item.iconType == CustomMenuItem::IconType::AppIcon) {
            D2D1_RECT_F dst = D2D1::RectF(iconBoxX, iconBoxY, iconBoxX + 16.0f, iconBoxY + 16.0f);
            if (item.pBitmap) {
                m_pDCRT->DrawBitmap(item.pBitmap, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                D2D1_ROUNDED_RECT fallbackBox = D2D1::RoundedRect(dst, 2.0f, 2.0f);
                m_pDCRT->FillRoundedRectangle(&fallbackBox, m_pMenuHoverBrush);
                m_pDCRT->DrawRoundedRectangle(&fallbackBox, m_pMenuBorderBrush, 1.0f);
            }
        } else if (item.iconType == CustomMenuItem::IconType::Pin || item.iconType == CustomMenuItem::IconType::Unpin) {
            if (m_pMenuIconFormat) {
                const wchar_t* glyph = (item.iconType == CustomMenuItem::IconType::Pin) ? L"\uE718" : L"\uE77A";
                D2D1_RECT_F glyphRect = D2D1::RectF(iconBoxX - 2.0f, iconBoxY - 2.0f, iconBoxX + 18.0f, iconBoxY + 18.0f);
                m_pDCRT->DrawTextW(glyph, 1, m_pMenuIconFormat, glyphRect, m_pMenuTextBrush);
            }
        } else if (item.iconType == CustomMenuItem::IconType::Close) {
            // Draw red rounded square with white X (matching user's screenshot exactly!)
            float redW = 14.0f;
            float redX = iconBoxX + 1.0f;
            float redY = iconBoxY + 1.0f;
            D2D1_ROUNDED_RECT redBox = D2D1::RoundedRect(
                D2D1::RectF(redX, redY, redX + redW, redY + redW),
                2.5f, 2.5f
            );
            m_pDCRT->FillRoundedRectangle(&redBox, m_pMenuCloseRedBrush);

            // Draw white X inside
            m_pDCRT->DrawLine(
                D2D1::Point2F(redX + 3.5f, redY + 3.5f),
                D2D1::Point2F(redX + redW - 3.5f, redY + redW - 3.5f),
                m_pMenuWhiteBrush,
                1.3f
            );
            m_pDCRT->DrawLine(
                D2D1::Point2F(redX + redW - 3.5f, redY + 3.5f),
                D2D1::Point2F(redX + 3.5f, redY + redW - 3.5f),
                m_pMenuWhiteBrush,
                1.3f
            );
        } else if (item.iconType == CustomMenuItem::IconType::Refresh) {
            if (m_pMenuIconFormat) {
                D2D1_RECT_F glyphRect = D2D1::RectF(iconBoxX - 2.0f, iconBoxY - 2.0f, iconBoxX + 18.0f, iconBoxY + 18.0f);
                m_pDCRT->DrawTextW(L"\uE72C", 1, m_pMenuIconFormat, glyphRect, m_pMenuTextBrush);
            }
        } else if (item.iconType == CustomMenuItem::IconType::Exit) {
            if (m_pMenuIconFormat) {
                D2D1_RECT_F glyphRect = D2D1::RectF(iconBoxX - 2.0f, iconBoxY - 2.0f, iconBoxX + 18.0f, iconBoxY + 18.0f);
                m_pDCRT->DrawTextW(L"\uE7E8", 1, m_pMenuIconFormat, glyphRect, m_pMenuTextBrush);
            }
        }

        // Draw Label Text
        if (m_pMenuTextFormat) {
            float textX = 38.0f;
            D2D1_RECT_F textRect = D2D1::RectF(textX, curY, (float)m_menuWidth - 12.0f, curY + rowH);
            m_pDCRT->DrawTextW(
                item.text.c_str(),
                (UINT32)item.text.length(),
                m_pMenuTextFormat,
                textRect,
                m_pMenuTextBrush
            );
        }

        curY += rowH;
    }

    m_pDCRT->EndDraw();

    // Update Layered Window with Premultiplied Alpha
    HDC hdcScreen = GetDC(NULL);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { m_menuWidth, m_menuHeight };
    POINT ptDst = { m_menuScreenX, m_menuScreenY };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        m_hMenuWnd,
        hdcScreen,
        &ptDst,
        &sz,
        m_hdcMenuMem,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );

    ReleaseDC(NULL, hdcScreen);
}

void DockWindow::ShowModernAppContextMenu(int itemIndex) {
    if (itemIndex < 0 || itemIndex >= (int)m_items.size()) return;
    m_menuItemIndex = itemIndex;

    const auto& item = m_items[itemIndex];
    std::wstring appName = item.name;
    std::wstring appLaunchPath = item.launchPath;
    std::wstring appExeFilename = item.exeFilename;
    HWND appHWnd = item.hWnd;
    bool appIsPinned = item.isPinned;
    bool appIsRunning = item.isRunning;

    std::vector<RunningWindowInfo> appWindows;
    if (!appExeFilename.empty()) {
        auto allWindows = TaskManager::GetOpenWindows(m_hWnd);
        for (const auto& win : allWindows) {
            if (win.exeFilename == appExeFilename) {
                appWindows.push_back(win);
            }
        }
    }
    if (appWindows.empty() && appHWnd && IsWindow(appHWnd)) {
        wchar_t titleBuf[256] = {};
        GetWindowTextW(appHWnd, titleBuf, 256);
        RunningWindowInfo fallbackWin;
        fallbackWin.hWnd = appHWnd;
        fallbackWin.title = titleBuf;
        fallbackWin.fullExePath = appLaunchPath;
        fallbackWin.exeFilename = appExeFilename;
        fallbackWin.isForeground = true;
        appWindows.push_back(fallbackWin);
    }

    m_menuItems.clear();

    // 1. If multiple windows exist, list them at the top
    if (appWindows.size() > 1) {
        for (size_t i = 0; i < appWindows.size() && i < 12; ++i) {
            std::wstring title = appWindows[i].title;
            if (title.empty()) {
                title = appName + L" (" + std::to_wstring(i + 1) + L")";
            } else if (title.length() > 36) {
                title = title.substr(0, 34) + L"...";
            }
            CustomMenuItem mi;
            mi.id = CMD_WINDOW_SELECT_BASE + (int)i;
            mi.text = title;
            mi.iconType = CustomMenuItem::IconType::AppIcon;
            mi.pBitmap = item.pBitmap;
            mi.targetHWnd = appWindows[i].hWnd;
            m_menuItems.push_back(mi);
        }
        CustomMenuItem sep;
        sep.isSeparator = true;
        m_menuItems.push_back(sep);
    }

    // 2. Application Name (e.g. "Antigravity")
    CustomMenuItem appMi;
    appMi.id = CMD_APP_LAUNCH_NEW;
    appMi.text = appName;
    appMi.iconType = CustomMenuItem::IconType::AppIcon;
    appMi.pBitmap = item.pBitmap;
    m_menuItems.push_back(appMi);

    // 3. Pin / Unpin from taskbar (Matches Windows Taskbar exactly!)
    CustomMenuItem pinMi;
    if (appIsPinned) {
        pinMi.id = CMD_APP_UNPIN;
        pinMi.text = L"Unpin from taskbar";
        pinMi.iconType = CustomMenuItem::IconType::Unpin;
    } else {
        pinMi.id = CMD_APP_PIN;
        pinMi.text = L"Pin to taskbar";
        pinMi.iconType = CustomMenuItem::IconType::Pin;
    }
    m_menuItems.push_back(pinMi);

    // 4. Close window / Close all windows (Matches Windows Taskbar exactly!)
    if (appIsRunning || !appWindows.empty()) {
        CustomMenuItem closeMi;
        if (appWindows.size() > 1) {
            closeMi.id = CMD_APP_CLOSE_ALL;
            closeMi.text = L"Close all windows";
        } else {
            closeMi.id = CMD_APP_CLOSE_WINDOW;
            closeMi.text = L"Close window";
        }
        closeMi.iconType = CustomMenuItem::IconType::Close;
        m_menuItems.push_back(closeMi);
    }

    // Measure max text width accurately using DirectWrite
    float maxTextWidth = 110.0f;
    for (const auto& mi : m_menuItems) {
        if (!mi.isSeparator && !mi.text.empty() && m_pDWriteFactory && m_pMenuTextFormat) {
            IDWriteTextLayout* pLayout = nullptr;
            if (SUCCEEDED(m_pDWriteFactory->CreateTextLayout(
                mi.text.c_str(), (UINT32)mi.text.length(),
                m_pMenuTextFormat, 1000.0f, 100.0f, &pLayout))) {
                DWRITE_TEXT_METRICS tm = {};
                pLayout->GetMetrics(&tm);
                if (tm.width > maxTextWidth) maxTextWidth = tm.width;
                pLayout->Release();
            }
        }
    }
    m_menuWidth = (int)std::ceil(maxTextWidth + 56.0f);
    if (m_menuWidth < 180) m_menuWidth = 180;
    if (m_menuWidth > 340) m_menuWidth = 340;

    int totalH = 8;
    for (const auto& mi : m_menuItems) {
        totalH += mi.isSeparator ? 8 : 32;
    }
    m_menuHeight = totalH;

    // Center menu nicely above the dock item
    m_menuScreenY = m_screenY + (int)m_dockPillRect.rect.top - m_menuHeight - 6;
    m_menuScreenX = m_screenX + (int)item.centerX - (m_menuWidth / 2);

    RECT rcWork = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    if (m_menuScreenX < rcWork.left + 8) m_menuScreenX = rcWork.left + 8;
    if (m_menuScreenX + m_menuWidth > rcWork.right - 8) m_menuScreenX = rcWork.right - m_menuWidth - 8;

    CreateMenuDIBBuffer(m_menuWidth, m_menuHeight);
    m_menuHoverIndex = -1;
    m_isMenuOpen = true;

    RenderMenu();

    SetWindowPos(
        m_hMenuWnd,
        HWND_TOPMOST,
        m_menuScreenX, m_menuScreenY,
        m_menuWidth, m_menuHeight,
        SWP_SHOWWINDOW | SWP_NOACTIVATE
    );

    s_pMenuInstance = this;
    if (!s_hMenuMouseHook) {
        s_hMenuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, m_hInstance, 0);
    }
    if (!s_hMenuKbdHook) {
        s_hMenuKbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, m_hInstance, 0);
    }
}

void DockWindow::ShowModernDockContextMenu() {
    m_menuItemIndex = -1;
    m_menuItems.clear();

    CustomMenuItem exitMi;
    exitMi.id = CMD_DOCK_EXIT;
    exitMi.text = L"Exit LiteDock";
    exitMi.iconType = CustomMenuItem::IconType::Exit;
    m_menuItems.push_back(exitMi);

    // Measure exact text width using DirectWrite
    float maxTextWidth = 80.0f;
    if (m_pDWriteFactory && m_pMenuTextFormat) {
        IDWriteTextLayout* pLayout = nullptr;
        if (SUCCEEDED(m_pDWriteFactory->CreateTextLayout(
            exitMi.text.c_str(), (UINT32)exitMi.text.length(),
            m_pMenuTextFormat, 1000.0f, 100.0f, &pLayout))) {
            DWRITE_TEXT_METRICS tm = {};
            pLayout->GetMetrics(&tm);
            maxTextWidth = tm.width;
            pLayout->Release();
        }
    }
    m_menuWidth = (int)std::ceil(maxTextWidth + 56.0f);
    if (m_menuWidth < 140) m_menuWidth = 140;

    m_menuHeight = 8 + 32; // Exactly 40px: 4px top padding + 32px item + 4px bottom padding

    POINT pt;
    GetCursorPos(&pt);
    m_menuScreenY = m_screenY + (int)m_dockPillRect.rect.top - m_menuHeight - 6;
    m_menuScreenX = pt.x - (m_menuWidth / 2);

    RECT rcWork = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);
    if (m_menuScreenX < rcWork.left + 8) m_menuScreenX = rcWork.left + 8;
    if (m_menuScreenX + m_menuWidth > rcWork.right - 8) m_menuScreenX = rcWork.right - m_menuWidth - 8;

    CreateMenuDIBBuffer(m_menuWidth, m_menuHeight);
    m_menuHoverIndex = -1;
    m_isMenuOpen = true;

    RenderMenu();

    SetWindowPos(
        m_hMenuWnd,
        HWND_TOPMOST,
        m_menuScreenX, m_menuScreenY,
        m_menuWidth, m_menuHeight,
        SWP_SHOWWINDOW | SWP_NOACTIVATE
    );

    s_pMenuInstance = this;
    if (!s_hMenuMouseHook) {
        s_hMenuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, m_hInstance, 0);
    }
    if (!s_hMenuKbdHook) {
        s_hMenuKbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, m_hInstance, 0);
    }
}

void DockWindow::ExecuteMenuCommand(int cmd, HWND targetHWnd) {
    if (cmd == CMD_DOCK_EXIT) {
        PostQuitMessage(0);
        return;
    }
    if (cmd == CMD_DOCK_REFRESH) {
        RefreshTaskbarItems();
        return;
    }

    if (cmd >= CMD_WINDOW_SELECT_BASE) {
        if (targetHWnd && IsWindow(targetHWnd)) {
            if (IsIconic(targetHWnd)) {
                ShowWindow(targetHWnd, SW_RESTORE);
            } else {
                ShowWindow(targetHWnd, SW_SHOW);
            }
            SetForegroundWindow(targetHWnd);
            SetFocus(targetHWnd);
            SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 200, NULL);
        }
        return;
    }

    if (m_menuItemIndex < 0 || m_menuItemIndex >= (int)m_items.size()) return;
    auto& item = m_items[m_menuItemIndex];

    if (cmd == CMD_APP_LAUNCH_NEW) {
        ShellExecuteW(NULL, L"open", item.launchPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 1000, NULL);
    } else if (cmd == CMD_APP_PIN) {
        std::wstring pathToPin = item.launchPath;
        if (TaskManager::PinApp(pathToPin, item.name)) {
            item.isPinned = true;
            wchar_t appData[MAX_PATH] = {};
            GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
            std::wstring lnk = std::wstring(appData) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar\\" + item.name + L".lnk";
            item.launchPath = lnk;
            SaveCustomOrder();
            RefreshTaskbarItems();
        }
    } else if (cmd == CMD_APP_UNPIN) {
        TaskManager::UnpinApp(item.launchPath, item.exeFilename);
        item.isPinned = false;
        if (!item.isRunning) {
            m_items.erase(m_items.begin() + m_menuItemIndex);
            ResizeCanvas(m_items.size());
            UpdateLayout(true);
            Render();
        }
        SaveCustomOrder();
        RefreshTaskbarItems();
    } else if (cmd == CMD_APP_CLOSE_WINDOW) {
        HWND hTarget = item.hWnd;
        if (hTarget && IsWindow(hTarget)) {
            PostMessageW(hTarget, WM_CLOSE, 0, 0);
            SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 300, NULL);
        }
    } else if (cmd == CMD_APP_CLOSE_ALL) {
        auto allWins = TaskManager::GetOpenWindows(m_hWnd);
        for (const auto& win : allWins) {
            if (win.exeFilename == item.exeFilename && win.hWnd && IsWindow(win.hWnd)) {
                PostMessageW(win.hWnd, WM_CLOSE, 0, 0);
            }
        }
        SetTimer(m_hWnd, TIMER_CHECK_RUNNING, 400, NULL);
    }
}

void DockWindow::ShowWindowSystemMenu(HWND hWnd, int screenX, int screenY) {
    if (!hWnd || !IsWindow(hWnd)) return;

    HMENU hMenu = CreatePopupMenu();
    bool isIconic = IsIconic(hWnd);
    bool isZoomed = IsZoomed(hWnd);

    AppendMenuW(hMenu, (isIconic || isZoomed) ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_SYS_RESTORE, L"Pulihkan");
    AppendMenuW(hMenu, (!isIconic && !isZoomed) ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_SYS_MOVE, L"Pindahkan");
    AppendMenuW(hMenu, (!isIconic && !isZoomed) ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_SYS_SIZE, L"Ukuran");
    AppendMenuW(hMenu, !isIconic ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_SYS_MINIMIZE, L"Minimalkan");
    AppendMenuW(hMenu, !isZoomed ? MF_STRING : (MF_STRING | MF_GRAYED), CMD_SYS_MAXIMIZE, L"Maksimalkan");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, CMD_SYS_CLOSE, L"Tutup\tAlt+F4");

    int menuX = screenX;
    int menuY = m_screenY + (int)m_dockPillRect.rect.top - 6;

    m_isMenuOpen = true;
    HWND hPrevFg = GetForegroundWindow();
    SetForegroundWindow(m_hWnd);

    int cmd = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
        menuX, menuY,
        0, m_hWnd, NULL
    );
    PostMessageW(m_hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
    m_isMenuOpen = false;

    if (cmd == 0) {
        if (hPrevFg && IsWindow(hPrevFg)) {
            SetForegroundWindow(hPrevFg);
        }
        return;
    }

    if (cmd == CMD_SYS_RESTORE) {
        PostMessageW(hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);
    } else if (cmd == CMD_SYS_MOVE) {
        PostMessageW(hWnd, WM_SYSCOMMAND, SC_MOVE, 0);
    } else if (cmd == CMD_SYS_SIZE) {
        PostMessageW(hWnd, WM_SYSCOMMAND, SC_SIZE, 0);
    } else if (cmd == CMD_SYS_MINIMIZE) {
        PostMessageW(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    } else if (cmd == CMD_SYS_MAXIMIZE) {
        PostMessageW(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    } else if (cmd == CMD_SYS_CLOSE) {
        PostMessageW(hWnd, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
}

void DockWindow::OnRButtonUp(int x, int y) {
    HidePreviewPanelImmediate();
    if (m_isDragging || m_isMenuOpen) return;

    int clickedIndex = -1;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const auto& item = m_items[i];
        if (x >= (int)item.x && x <= (int)(item.x + item.width) &&
            y >= (int)item.y && y <= (int)(item.y + item.height + Config::DOCK_PADDING_Y)) {
            clickedIndex = (int)i;
            break;
        }
    }

    // Robust fallback: if clicked within active hovered item
    if (clickedIndex < 0 && m_hoveredIndex >= 0 && m_hoveredIndex < (int)m_items.size()) {
        clickedIndex = m_hoveredIndex;
    }

    // Temporarily hide hover tooltip while menu is active
    m_hoveredIndex = -1;
    Render();

    if (clickedIndex >= 0) {
        if ((GetKeyState(VK_SHIFT) & 0x8000) && m_items[clickedIndex].hWnd && IsWindow(m_items[clickedIndex].hWnd)) {
            POINT pt;
            GetCursorPos(&pt);
            ShowWindowSystemMenu(m_items[clickedIndex].hWnd, pt.x, pt.y);
        } else {
            ShowModernAppContextMenu(clickedIndex);
        }
    } else {
        ShowModernDockContextMenu();
    }
}




void DockWindow::Run() {
    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK DockWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DockWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<DockWindow*>(pCreate->lpCreateParams);
        if (pThis) {
            pThis->m_hWnd = hWnd;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    } else {
        pThis = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->HandleMessage(hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT DockWindow::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Windows Shell Hook Event (window created, destroyed, or activated)
    if (m_shellHookMsg != 0 && msg == m_shellHookMsg) {
        RefreshTaskbarItems();
        return 0;
    }

    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(hWnd, &pt);
        if (HitTest(pt.x, pt.y)) {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }

    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_MOUSELEAVE:
        OnMouseLeave();
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_RBUTTONUP:
        OnRButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_ANIM) {
            OnTimer();
        } else if (wParam == TIMER_CHECK_RUNNING) {
            RefreshTaskbarItems();
        } else if (wParam == TIMER_PREVIEW_HOVER) {
            KillTimer(hWnd, TIMER_PREVIEW_HOVER);
            if (!m_isDragging && !m_isMenuOpen && m_isMouseOverDock) {
                int hoveredApp = -1;
                for (size_t i = 0; i < m_items.size(); ++i) {
                    const auto& item = m_items[i];
                    if (m_mouseX >= item.x && m_mouseX <= (item.x + item.width) &&
                        m_mouseY >= item.y && m_mouseY <= (item.y + item.height + Config::DOCK_PADDING_Y)) {
                        if (item.isRunning && item.hWnd && IsWindow(item.hWnd)) {
                            hoveredApp = (int)i;
                        }
                        break;
                    }
                }
                if (hoveredApp >= 0) {
                    ShowPreviewPanel(hoveredApp);
                }
            }
        } else if (wParam == TIMER_PREVIEW_CLOSE) {
            KillTimer(hWnd, TIMER_PREVIEW_CLOSE);
            if (m_isPreviewOpen) {
                POINT pt;
                GetCursorPos(&pt);
                HWND hUnder = WindowFromPoint(pt);
                if (hUnder != m_hPreviewWnd && hUnder != m_hWnd) {
                    HidePreviewPanel();
                }
            }
        } else if (wParam == TIMER_PREVIEW_ANIM) {
            OnPreviewAnimTimer();
        }
        return 0;

    case WM_DISPLAYCHANGE:
        PositionWindow();
        UpdateLayout(false);
        Render();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void DockWindow::InitPreviewWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = DockWindow::PreviewWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LiteDockPreviewWindowClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DROPSHADOW;

    RegisterClassExW(&wc);

    CreatePreviewDIBBuffer(m_previewWidth, m_previewHeight);

    m_hPreviewWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"LiteDockPreview",
        WS_POPUP,
        0, 0, m_previewWidth, m_previewHeight,
        NULL, NULL, hInstance, this
    );

    if (m_hPreviewWnd) {
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(m_hPreviewWnd, 20, &darkMode, sizeof(darkMode)); // DWMWA_USE_IMMERSIVE_DARK_MODE
    }
}

void DockWindow::CleanupPreviewWindow() {
    HidePreviewPanelImmediate();

    if (m_hPreviewBitmap) {
        if (m_hdcPreviewMem && m_hPreviewOldBitmap) SelectObject(m_hdcPreviewMem, m_hPreviewOldBitmap);
        DeleteObject(m_hPreviewBitmap);
        m_hPreviewBitmap = nullptr;
    }
    if (m_hdcPreviewMem) {
        DeleteDC(m_hdcPreviewMem);
        m_hdcPreviewMem = nullptr;
    }
    if (m_hPreviewWnd) {
        DestroyWindow(m_hPreviewWnd);
        m_hPreviewWnd = nullptr;
    }
}

void DockWindow::CreatePreviewDIBBuffer(int width, int height) {
    if (m_hPreviewBitmap) {
        if (m_hdcPreviewMem && m_hPreviewOldBitmap) SelectObject(m_hdcPreviewMem, m_hPreviewOldBitmap);
        DeleteObject(m_hPreviewBitmap);
        m_hPreviewBitmap = nullptr;
    }
    if (m_hdcPreviewMem) {
        DeleteDC(m_hdcPreviewMem);
        m_hdcPreviewMem = nullptr;
    }

    m_previewBufferWidth = width;
    m_previewBufferHeight = height;

    HDC hdcScreen = GetDC(NULL);
    m_hdcPreviewMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_hPreviewBitmap = CreateDIBSection(m_hdcPreviewMem, &bmi, DIB_RGB_COLORS, &m_pvPreviewBits, NULL, 0);
    m_hPreviewOldBitmap = (HBITMAP)SelectObject(m_hdcPreviewMem, m_hPreviewBitmap);

    ReleaseDC(NULL, hdcScreen);
}

void DockWindow::EnsurePreviewBufferSize(int width, int height) {
    if (width > m_previewBufferWidth || height > m_previewBufferHeight) {
        int newW = std::max(width, m_previewBufferWidth);
        int newH = std::max(height, m_previewBufferHeight);
        CreatePreviewDIBBuffer(newW, newH);
    }
}

void DockWindow::ShowPreviewPanel(int itemIndex) {
    if (itemIndex < 0 || itemIndex >= (int)m_items.size()) {
        HidePreviewPanel();
        return;
    }

    const auto& item = m_items[itemIndex];
    if (!item.isRunning) {
        HidePreviewPanel();
        return;
    }

    if (m_isMenuOpen || m_isDragging) {
        return;
    }

    // Gather all valid windows for this app
    std::vector<DockWindowEntry> validWindows;
    for (const auto& win : item.openWindows) {
        if (win.hWnd && IsWindow(win.hWnd)) {
            bool alreadyIn = false;
            for (const auto& existing : validWindows) {
                if (existing.hWnd == win.hWnd) {
                    alreadyIn = true;
                    break;
                }
            }
            if (alreadyIn) continue;

            DockWindowEntry e = win;
            wchar_t titleBuf[256] = {};
            if (GetWindowTextW(e.hWnd, titleBuf, 256) > 0) {
                e.title = titleBuf;
            }
            if (e.title.empty()) {
                e.title = item.name;
            }
            validWindows.push_back(e);
        }
    }
    if (validWindows.empty() && item.hWnd && IsWindow(item.hWnd)) {
        DockWindowEntry e;
        e.hWnd = item.hWnd;
        e.title = !item.windowTitle.empty() ? item.windowTitle : item.name;
        e.isForeground = item.isForeground;
        validWindows.push_back(e);
    }

    if (validWindows.empty()) {
        HidePreviewPanel();
        return;
    }

    RECT rcWork = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

    size_t N = validWindows.size();
    float cardW = 216.0f;
    float cardH = 160.0f;
    float pad = 8.0f;
    float gap = 8.0f;

    if (N == 1) {
        cardW = 235.0f;
        cardH = 167.0f;
        pad = 0.5f;
        gap = 0.0f;
    } else {
        float maxTotalW = (float)(rcWork.right - rcWork.left - 40);
        float neededW = (pad * 2.0f) + ((float)N * cardW) + ((float)(N - 1) * gap);
        if (neededW > maxTotalW) {
            float avail = maxTotalW - (pad * 2.0f) - ((float)(N - 1) * gap);
            cardW = std::max(150.0f, avail / (float)N);
        }
    }

    int totalWidth = (int)std::ceil((pad * 2.0f) + ((float)N * cardW) + ((float)(N - 1) * gap));
    int totalHeight = (int)std::ceil((pad * 2.0f) + cardH);

    float iconScreenCenterX = (float)m_screenX + item.centerX;
    int previewLeft = (int)std::round(iconScreenCenterX - (float)totalWidth / 2.0f);
    if (previewLeft < rcWork.left + 8) previewLeft = rcWork.left + 8;
    if (previewLeft + totalWidth > rcWork.right - 8) previewLeft = rcWork.right - 8 - totalWidth;

    int previewTop = m_screenY + (int)m_dockPillRect.rect.top - totalHeight - 12;
    if (previewTop < rcWork.top + 8) previewTop = rcWork.top + 8;

    EnsurePreviewBufferSize(totalWidth, totalHeight);

    // Free existing preview cards thumbnails
    for (auto& card : m_previewCards) {
        if (card.pThumbnail) {
            card.pThumbnail->Release();
            card.pThumbnail = nullptr;
        }
    }
    m_previewCards.clear();

    // Create cards for each window
    for (size_t i = 0; i < N; ++i) {
        PreviewCardItem card;
        card.hWnd = validWindows[i].hWnd;
        card.title = validWindows[i].title;

        float cx = pad + (float)i * (cardW + gap);
        float cy = pad;
        card.cardRect = D2D1::RectF(cx, cy, cx + cardW, cy + cardH);
        card.closeBtnRect = D2D1::RectF(cx + cardW - 28.0f, cy + 4.0f, cx + cardW - 4.0f, cy + 26.0f);
        card.thumbRect = D2D1::RectF(cx + 6.0f, cy + 30.0f, cx + cardW - 6.0f, cy + cardH - 6.0f);

        if (m_pDCRT) {
            m_animator.GetSnapshotBitmap(card.hWnd, m_pDCRT, &card.pThumbnail, card.thumbSrcW, card.thumbSrcH);
        }
        m_previewCards.push_back(std::move(card));
    }

    bool wasAlreadyVisible = (m_isPreviewOpen || m_isPreviewClosing) && (m_previewAlpha > 0.08f);
    if (!wasAlreadyVisible) {
        m_previewCurrentX = (float)previewLeft;
        m_previewCurrentY = (float)previewTop + 8.0f;
        m_previewAlpha = 0.0f;
        m_previewWidth = totalWidth;
        m_previewHeight = totalHeight;
        m_previewScreenX = previewLeft;
        m_previewScreenY = previewTop + 8;
        SetWindowPos(m_hPreviewWnd, HWND_TOPMOST, previewLeft, previewTop + 8, totalWidth, totalHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        m_previewWidth = totalWidth;
        m_previewHeight = totalHeight;
        SetWindowPos(m_hPreviewWnd, HWND_TOPMOST, m_previewScreenX, m_previewScreenY, totalWidth, totalHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    m_previewTargetX = (float)previewLeft;
    m_previewTargetY = (float)previewTop;
    m_previewTargetAlpha = 1.0f;
    m_previewWidth = totalWidth;
    m_previewHeight = totalHeight;
    m_isPreviewOpen = true;
    m_isPreviewClosing = false;
    m_previewAppIndex = itemIndex;
    m_pPreviewIcon = item.pBitmap;
    m_previewHoveredCard = -1;
    m_previewCloseHovered = false;
    m_previewThumbHovered = false;

    SetTimer(m_hWnd, TIMER_PREVIEW_ANIM, 16, NULL);

    RenderPreviewPanel();
    Render();
}

void DockWindow::HidePreviewPanel() {
    if (!m_isPreviewOpen && !m_isPreviewClosing) return;

    KillTimer(m_hWnd, TIMER_PREVIEW_HOVER);
    KillTimer(m_hWnd, TIMER_PREVIEW_CLOSE);

    m_previewTargetAlpha = 0.0f;
    m_isPreviewClosing = true;

    SetTimer(m_hWnd, TIMER_PREVIEW_ANIM, 16, NULL);
}

void DockWindow::HidePreviewPanelImmediate() {
    KillTimer(m_hWnd, TIMER_PREVIEW_HOVER);
    KillTimer(m_hWnd, TIMER_PREVIEW_CLOSE);
    KillTimer(m_hWnd, TIMER_PREVIEW_ANIM);

    if (m_hPreviewWnd) {
        ShowWindow(m_hPreviewWnd, SW_HIDE);
    }

    for (auto& card : m_previewCards) {
        if (card.pThumbnail) {
            card.pThumbnail->Release();
            card.pThumbnail = nullptr;
        }
    }
    m_previewCards.clear();

    m_pPreviewIcon = nullptr;
    m_isPreviewOpen = false;
    m_isPreviewClosing = false;
    m_previewAppIndex = -1;
    m_previewHoveredCard = -1;
    m_previewCloseHovered = false;
    m_previewThumbHovered = false;
    m_previewAlpha = 0.0f;
    m_previewTargetAlpha = 0.0f;

    Render();
}

void DockWindow::OnPreviewAnimTimer() {
    bool stillAnimating = false;

    // 1. Smooth horizontal glide lerp
    float diffX = m_previewTargetX - m_previewCurrentX;
    if (std::fabs(diffX) > 0.5f) {
        m_previewCurrentX += diffX * 0.32f;
        stillAnimating = true;
    } else {
        m_previewCurrentX = m_previewTargetX;
    }

    // 2. Smooth vertical slide lerp
    float diffY = m_previewTargetY - m_previewCurrentY;
    if (std::fabs(diffY) > 0.5f) {
        m_previewCurrentY += diffY * 0.32f;
        stillAnimating = true;
    } else {
        m_previewCurrentY = m_previewTargetY;
    }

    // 3. Smooth hardware alpha fade
    float diffAlpha = m_previewTargetAlpha - m_previewAlpha;
    if (std::fabs(diffAlpha) > 0.02f) {
        m_previewAlpha += diffAlpha * 0.35f;
        stillAnimating = true;
    } else {
        m_previewAlpha = m_previewTargetAlpha;
    }

    if (m_isPreviewClosing && m_previewAlpha <= 0.04f) {
        HidePreviewPanelImmediate();
        return;
    }

    m_previewScreenX = (int)std::round(m_previewCurrentX);
    m_previewScreenY = (int)std::round(m_previewCurrentY);

    SetWindowPos(m_hPreviewWnd, HWND_TOPMOST, m_previewScreenX, m_previewScreenY, m_previewWidth, m_previewHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);

    RenderPreviewPanel();

    if (!stillAnimating) {
        KillTimer(m_hWnd, TIMER_PREVIEW_ANIM);
    }
}

void DockWindow::RenderPreviewPanel() {
    if (!m_pDCRT || !m_hdcPreviewMem || m_previewWidth <= 0 || m_previewHeight <= 0 || m_previewCards.empty()) return;

    RECT rc = { 0, 0, m_previewWidth, m_previewHeight };
    m_pDCRT->BindDC(m_hdcPreviewMem, &rc);
    m_pDCRT->BeginDraw();
    m_pDCRT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // 0. If multiple cards exist, render modern dark acrylic flyout container
    if (m_previewCards.size() > 1) {
        D2D1_ROUNDED_RECT rcPanel = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, (float)m_previewWidth - 0.5f, (float)m_previewHeight - 0.5f),
            9.0f, 9.0f
        );
        m_pDCRT->FillRoundedRectangle(&rcPanel, m_pMenuBgBrush);
        m_pDCRT->DrawRoundedRectangle(&rcPanel, m_pMenuBorderBrush, 1.0f);
    }

    for (size_t i = 0; i < m_previewCards.size(); ++i) {
        const auto& card = m_previewCards[i];
        bool isCardHovered = ((int)i == m_previewHoveredCard);

        // 1. Card container
        D2D1_ROUNDED_RECT rcCard = D2D1::RoundedRect(card.cardRect, 7.0f, 7.0f);
        if (m_previewCards.size() > 1) {
            m_pDCRT->FillRoundedRectangle(&rcCard, isCardHovered ? m_pMenuHoverBrush : m_pTooltipBgBrush);
            m_pDCRT->DrawRoundedRectangle(&rcCard, isCardHovered ? m_pActiveIndicatorBrush : m_pMenuBorderBrush, isCardHovered ? 1.4f : 1.0f);
        } else {
            m_pDCRT->FillRoundedRectangle(&rcCard, isCardHovered ? m_pMenuHoverBrush : m_pMenuBgBrush);
            m_pDCRT->DrawRoundedRectangle(&rcCard, isCardHovered ? m_pActiveIndicatorBrush : m_pMenuBorderBrush, isCardHovered ? 1.2f : 1.0f);
        }

        // 2. Card Header
        // 2a. App Icon (16x16)
        float iconX = card.cardRect.left + 10.0f;
        float iconY = card.cardRect.top + 7.0f;
        D2D1_RECT_F iconDst = D2D1::RectF(iconX, iconY, iconX + 16.0f, iconY + 16.0f);
        if (m_pPreviewIcon) {
            m_pDCRT->DrawBitmap(m_pPreviewIcon, iconDst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        // 2b. Close Button '×'
        D2D1_ROUNDED_RECT closeBox = D2D1::RoundedRect(card.closeBtnRect, 4.0f, 4.0f);
        bool isCloseHovered = (isCardHovered && m_previewCloseHovered);
        if (isCloseHovered) {
            m_pDCRT->FillRoundedRectangle(&closeBox, m_pMenuCloseRedBrush);
        }
        if (m_pTextFormat) {
            m_pDCRT->DrawTextW(
                L"\u00D7", 1,
                m_pTextFormat,
                card.closeBtnRect,
                isCloseHovered ? m_pMenuWhiteBrush : m_pMenuTextBrush
            );
        }

        // 2c. Window Title
        float titleX = iconX + 22.0f;
        float titleW = card.closeBtnRect.left - titleX - 4.0f;
        D2D1_RECT_F titleRect = D2D1::RectF(titleX, card.cardRect.top + 5.0f, titleX + titleW, card.cardRect.top + 25.0f);

        std::wstring displayTitle = card.title;
        if (displayTitle.length() > 22) {
            displayTitle = displayTitle.substr(0, 20) + L"...";
        }
        if (m_pMenuTextFormat) {
            m_pDCRT->DrawTextW(
                displayTitle.c_str(),
                (UINT32)displayTitle.length(),
                m_pMenuTextFormat,
                titleRect,
                m_pMenuWhiteBrush
            );
        }

        // 3. Recessed Thumbnail Well
        D2D1_ROUNDED_RECT wellBox = D2D1::RoundedRect(card.thumbRect, 6.0f, 6.0f);
        m_pDCRT->FillRoundedRectangle(&wellBox, m_pTooltipBgBrush);
        bool isThumbHovered = (isCardHovered && m_previewThumbHovered);
        m_pDCRT->DrawRoundedRectangle(
            &wellBox,
            isThumbHovered ? m_pActiveIndicatorBrush : m_pMenuSeparatorBrush,
            isThumbHovered ? 1.5f : 1.0f
        );

        // 4. Thumbnail Image inside well
        float wellW = card.thumbRect.right - card.thumbRect.left;
        float wellH = card.thumbRect.bottom - card.thumbRect.top;
        if (card.pThumbnail && card.thumbSrcW > 0 && card.thumbSrcH > 0) {
            float innerPadding = 4.0f;
            float maxThumbW = wellW - (innerPadding * 2.0f);
            float maxThumbH = wellH - (innerPadding * 2.0f);

            float scale = std::min(maxThumbW / (float)card.thumbSrcW, maxThumbH / (float)card.thumbSrcH);
            float thumbW = std::floor((float)card.thumbSrcW * scale);
            float thumbH = std::floor((float)card.thumbSrcH * scale);
            float thumbX = card.thumbRect.left + innerPadding + std::floor((maxThumbW - thumbW) / 2.0f);
            float thumbY = card.thumbRect.top + innerPadding + std::floor((maxThumbH - thumbH) / 2.0f);

            D2D1_RECT_F thumbDst = D2D1::RectF(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH);
            m_pDCRT->DrawBitmap(card.pThumbnail, thumbDst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            m_pDCRT->DrawRectangle(thumbDst, m_pMenuBorderBrush, 1.0f);
        } else {
            float centerIconX = card.thumbRect.left + (wellW - 32.0f) / 2.0f;
            float centerIconY = card.thumbRect.top + (wellH - 32.0f) / 2.0f - 8.0f;
            if (m_pPreviewIcon) {
                D2D1_RECT_F fallbackIconDst = D2D1::RectF(centerIconX, centerIconY, centerIconX + 32.0f, centerIconY + 32.0f);
                m_pDCRT->DrawBitmap(m_pPreviewIcon, fallbackIconDst, 0.7f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            if (m_pTextFormat) {
                D2D1_RECT_F fallbackTextDst = D2D1::RectF(card.thumbRect.left, centerIconY + 36.0f, card.thumbRect.right, centerIconY + 54.0f);
                m_pDCRT->DrawTextW(
                    L"Click to switch", 15,
                    m_pTextFormat,
                    fallbackTextDst,
                    m_pMenuBorderBrush
                );
            }
        }
    }

    HRESULT hr = m_pDCRT->EndDraw();
    if (FAILED(hr)) return;

    // Update Layered Window with Premultiplied Alpha multiplied by hardware m_previewAlpha
    HDC hdcScreen = GetDC(NULL);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { m_previewWidth, m_previewHeight };
    POINT ptDst = { m_previewScreenX, m_previewScreenY };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = (BYTE)std::clamp((int)(m_previewAlpha * 255.0f), 0, 255);
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        m_hPreviewWnd,
        hdcScreen,
        &ptDst,
        &sz,
        m_hdcPreviewMem,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );

    ReleaseDC(NULL, hdcScreen);
}

LRESULT CALLBACK DockWindow::PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DockWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<DockWindow*>(pCreate->lpCreateParams);
        if (pThis) {
            pThis->m_hPreviewWnd = hWnd;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    } else {
        pThis = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->HandlePreviewMessage(hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT DockWindow::HandlePreviewMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_MOUSEMOVE: {
        KillTimer(m_hWnd, TIMER_PREVIEW_CLOSE);

        TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT) };
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hWnd;
        TrackMouseEvent(&tme);

        int mx = GET_X_LPARAM(lParam);
        int my = GET_Y_LPARAM(lParam);

        int newHoveredCard = -1;
        bool newCloseHovered = false;
        bool newThumbHovered = false;

        for (size_t i = 0; i < m_previewCards.size(); ++i) {
            const auto& c = m_previewCards[i];
            if (mx >= c.cardRect.left && mx <= c.cardRect.right &&
                my >= c.cardRect.top && my <= c.cardRect.bottom) {
                newHoveredCard = (int)i;
                if (mx >= c.closeBtnRect.left && mx <= c.closeBtnRect.right &&
                    my >= c.closeBtnRect.top && my <= c.closeBtnRect.bottom) {
                    newCloseHovered = true;
                } else if (mx >= c.thumbRect.left && mx <= c.thumbRect.right &&
                           my >= c.thumbRect.top && my <= c.thumbRect.bottom) {
                    newThumbHovered = true;
                }
                break;
            }
        }

        if (newHoveredCard != m_previewHoveredCard ||
            newCloseHovered != m_previewCloseHovered ||
            newThumbHovered != m_previewThumbHovered) {
            m_previewHoveredCard = newHoveredCard;
            m_previewCloseHovered = newCloseHovered;
            m_previewThumbHovered = newThumbHovered;
            RenderPreviewPanel();
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        m_previewHoveredCard = -1;
        m_previewCloseHovered = false;
        m_previewThumbHovered = false;
        RenderPreviewPanel();

        SetTimer(m_hWnd, TIMER_PREVIEW_CLOSE, 250, NULL);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (m_previewHoveredCard >= 0 && m_previewHoveredCard < (int)m_previewCards.size()) {
            HWND targetHWnd = m_previewCards[m_previewHoveredCard].hWnd;

            if (m_previewCloseHovered) {
                if (targetHWnd && IsWindow(targetHWnd)) {
                    PostMessageW(targetHWnd, WM_CLOSE, 0, 0);
                }

                if (m_previewCards.size() > 1) {
                    if (m_previewCards[m_previewHoveredCard].pThumbnail) {
                        m_previewCards[m_previewHoveredCard].pThumbnail->Release();
                    }
                    m_previewCards.erase(m_previewCards.begin() + m_previewHoveredCard);

                    // Re-layout remaining cards
                    RECT rcWork = {};
                    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0);

                    size_t N = m_previewCards.size();
                    float cardW = 216.0f;
                    float cardH = 160.0f;
                    float pad = 8.0f;
                    float gap = 8.0f;
                    if (N == 1) {
                        cardW = 235.0f;
                        cardH = 167.0f;
                        pad = 0.5f;
                        gap = 0.0f;
                    }
                    int totalWidth = (int)std::ceil((pad * 2.0f) + ((float)N * cardW) + ((float)(N - 1) * gap));
                    int totalHeight = (int)std::ceil((pad * 2.0f) + cardH);

                    m_previewWidth = totalWidth;
                    m_previewHeight = totalHeight;

                    // Re-center flyout above the dock icon
                    if (m_previewAppIndex >= 0 && m_previewAppIndex < (int)m_items.size()) {
                        float iconScreenCenterX = (float)m_screenX + m_items[m_previewAppIndex].centerX;
                        int previewLeft = (int)std::round(iconScreenCenterX - (float)totalWidth / 2.0f);
                        if (previewLeft < rcWork.left + 8) previewLeft = rcWork.left + 8;
                        if (previewLeft + totalWidth > rcWork.right - 8) previewLeft = rcWork.right - 8 - totalWidth;
                        m_previewScreenX = previewLeft;
                        m_previewCurrentX = (float)previewLeft;
                        m_previewTargetX = (float)previewLeft;
                    }

                    for (size_t i = 0; i < m_previewCards.size(); ++i) {
                        float cx = pad + (float)i * (cardW + gap);
                        float cy = pad;
                        m_previewCards[i].cardRect = D2D1::RectF(cx, cy, cx + cardW, cy + cardH);
                        m_previewCards[i].closeBtnRect = D2D1::RectF(cx + cardW - 28.0f, cy + 4.0f, cx + cardW - 4.0f, cy + 26.0f);
                        m_previewCards[i].thumbRect = D2D1::RectF(cx + 6.0f, cy + 30.0f, cx + cardW - 6.0f, cy + cardH - 6.0f);
                    }

                    m_previewHoveredCard = -1;
                    m_previewCloseHovered = false;
                    m_previewThumbHovered = false;

                    SetWindowPos(m_hPreviewWnd, HWND_TOPMOST, m_previewScreenX, m_previewScreenY, m_previewWidth, m_previewHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
                    RenderPreviewPanel();
                } else {
                    HidePreviewPanelImmediate();
                }
                return 0;
            }

            // Clicked thumbnail or card
            int appIdx = m_previewAppIndex;
            HidePreviewPanelImmediate();
            if (targetHWnd && IsWindow(targetHWnd)) {
                if (IsIconic(targetHWnd)) {
                    if (appIdx >= 0 && appIdx < (int)m_items.size()) {
                        float iconX = (float)m_screenX + m_items[appIdx].centerX;
                        float iconY = (float)m_screenY + m_items[appIdx].y + (m_items[appIdx].height / 2.0f);
                        float iconW = m_items[appIdx].width;
                        m_animator.AnimateWindow(targetHWnd, iconX, iconY, iconW, false);
                    } else {
                        ShowWindow(targetHWnd, SW_RESTORE);
                        SetForegroundWindow(targetHWnd);
                    }
                } else {
                    DWORD targetThread = GetWindowThreadProcessId(targetHWnd, NULL);
                    DWORD curThread = GetCurrentThreadId();
                    AttachThreadInput(curThread, targetThread, TRUE);
                    SetForegroundWindow(targetHWnd);
                    BringWindowToTop(targetHWnd);
                    AttachThreadInput(curThread, targetThread, FALSE);
                }
            }
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        int appIdx = m_previewAppIndex;
        HidePreviewPanelImmediate();
        if (appIdx >= 0 && appIdx < (int)m_items.size()) {
            ShowModernAppContextMenu(appIdx);
        }
        return 0;
    }
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

