#include "WindowAnimator.h"
#include <cmath>
#include <algorithm>
#include <cstdint>

#pragma comment(lib, "dwmapi.lib")

WindowAnimator::WindowAnimator() = default;

WindowAnimator::~WindowAnimator() {
    Cleanup();
}

bool WindowAnimator::Initialize(HINSTANCE hInstance, ID2D1Factory* pD2DFactory) {
    m_hInstance = hInstance;
    m_pD2DFactory = pD2DFactory;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WindowAnimator::AnimWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LiteDockWindowAnimatorClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    m_hAnimWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        wc.lpszClassName,
        L"LiteDockWindowAnimator",
        WS_POPUP,
        0, 0, 1, 1,
        NULL, NULL, hInstance, this
    );

    if (!m_hAnimWnd) return false;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    if (m_pD2DFactory) {
        m_pD2DFactory->CreateDCRenderTarget(&props, &m_pAnimDCRT);
    }

    return (m_pAnimDCRT != nullptr);
}

void WindowAnimator::Cleanup() {
    CancelAnimation();

    if (m_pCapturedBitmap) {
        m_pCapturedBitmap->Release();
        m_pCapturedBitmap = nullptr;
    }
    if (m_pAnimDCRT) {
        m_pAnimDCRT->Release();
        m_pAnimDCRT = nullptr;
    }
    if (m_hAnimBitmap) {
        if (m_hdcAnimMem && m_hAnimOldBitmap) SelectObject(m_hdcAnimMem, m_hAnimOldBitmap);
        DeleteObject(m_hAnimBitmap);
        m_hAnimBitmap = nullptr;
    }
    if (m_hdcAnimMem) {
        DeleteDC(m_hdcAnimMem);
        m_hdcAnimMem = nullptr;
    }
    if (m_hAnimWnd) {
        DestroyWindow(m_hAnimWnd);
        m_hAnimWnd = nullptr;
    }
    m_bufferWidth = 0;
    m_bufferHeight = 0;
    m_snapshotCache.clear();
}

void WindowAnimator::CreateAnimDIBBuffer(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (m_hAnimBitmap && m_bufferWidth == width && m_bufferHeight == height) {
        return; // Already matches
    }

    if (m_hAnimBitmap) {
        if (m_hdcAnimMem && m_hAnimOldBitmap) SelectObject(m_hdcAnimMem, m_hAnimOldBitmap);
        DeleteObject(m_hAnimBitmap);
        m_hAnimBitmap = nullptr;
    }
    if (m_hdcAnimMem) {
        DeleteDC(m_hdcAnimMem);
        m_hdcAnimMem = nullptr;
    }

    HDC hdcScreen = GetDC(NULL);
    m_hdcAnimMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_hAnimBitmap = CreateDIBSection(m_hdcAnimMem, &bmi, DIB_RGB_COLORS, &m_pvAnimBits, NULL, 0);
    m_hAnimOldBitmap = (HBITMAP)SelectObject(m_hdcAnimMem, m_hAnimBitmap);

    ReleaseDC(NULL, hdcScreen);

    m_bufferWidth = width;
    m_bufferHeight = height;

    RECT rc = { 0, 0, width, height };
    if (m_pAnimDCRT) {
        m_pAnimDCRT->BindDC(m_hdcAnimMem, &rc);
    }
}

bool WindowAnimator::CaptureWindow(HWND hWnd, HBITMAP& outBmp, void*& outBits, int& outW, int& outH) {
    if (!hWnd || !IsWindow(hWnd)) return false;

    // Use DWM extended frame bounds to get true visible window rect (strips 8px invisible shadows)
    RECT rc = {};
    if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc)))) {
        GetWindowRect(hWnd, &rc);
    }

    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    GetMonitorInfo(hMon, &mi);

    if (IsZoomed(hWnd)) {
        rc = mi.rcWork;
    } else {
        if (rc.left < mi.rcMonitor.left) rc.left = mi.rcMonitor.left;
        if (rc.top < mi.rcMonitor.top) rc.top = mi.rcMonitor.top;
        if (rc.right > mi.rcMonitor.right) rc.right = mi.rcMonitor.right;
        if (rc.bottom > mi.rcMonitor.bottom) rc.bottom = mi.rcMonitor.bottom;
    }

    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return false;

    outW = width;
    outH = height;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    outBmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &outBits, NULL, 0);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, outBmp);

    BOOL ok = FALSE;
    // If window is visible and unminimized on desktop, BitBlt captures live hardware composition
    if (IsWindowVisible(hWnd) && !IsIconic(hWnd)) {
        ok = BitBlt(hdcMem, 0, 0, width, height, hdcScreen, rc.left, rc.top, SRCCOPY);
    }

    // Fallback to PrintWindow if BitBlt wasn't available or returned empty
    if (!ok) {
        ok = PrintWindow(hWnd, hdcMem, 2 /* PW_RENDERFULLCONTENT */);
        if (!ok) {
            ok = PrintWindow(hWnd, hdcMem, 0);
        }
    }

    // Ultimate fallback: fill clean dark background so capture never fails
    if (!ok && outBits) {
        uint32_t* px = reinterpret_cast<uint32_t*>(outBits);
        int total = width * height;
        for (int p = 0; p < total; ++p) {
            px[p] = 0xFF2A2A2E;
        }
        ok = TRUE;
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);

    // Selective alpha: only mark non-zero RGB pixels as opaque (0xFF000000).
    // Preserves transparency of rounded corners and drop shadows without black side fringing!
    if (outBits) {
        uint32_t* pixels = reinterpret_cast<uint32_t*>(outBits);
        int totalPixels = width * height;
        for (int p = 0; p < totalPixels; ++p) {
            if ((pixels[p] & 0x00FFFFFF) != 0) {
                pixels[p] |= 0xFF000000;
            }
        }
    }

    return ok;
}

void WindowAnimator::PrecacheWindowSnapshot(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || IsIconic(hWnd) || !IsWindowVisible(hWnd)) return;
    if (m_isAnimating) return; // Don't interrupt active animation

    auto it = m_snapshotCache.find(hWnd);
    if (it != m_snapshotCache.end() && !it->second.pixelData.empty()) {
        return; // Already cached
    }

    HBITMAP hCapBmp = nullptr;
    void* pBits = nullptr;
    int bmpW = 0, bmpH = 0;
    if (CaptureWindow(hWnd, hCapBmp, pBits, bmpW, bmpH) && hCapBmp && pBits && bmpW > 0 && bmpH > 0) {
        CachedSnapshot snap;
        snap.width = bmpW;
        snap.height = bmpH;
        if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &snap.winRect, sizeof(snap.winRect)))) {
            GetWindowRect(hWnd, &snap.winRect);
        }
        snap.isMaximized = IsZoomed(hWnd);
        size_t byteCount = (size_t)bmpW * bmpH * 4;
        snap.pixelData.resize(byteCount);
        memcpy(snap.pixelData.data(), pBits, byteCount);
        m_snapshotCache[hWnd] = std::move(snap);
        DeleteObject(hCapBmp);
    }
}

bool WindowAnimator::GetSnapshotBitmap(HWND hWnd, ID2D1RenderTarget* pRT, ID2D1Bitmap** ppBitmap, int& outW, int& outH) {
    if (!hWnd || !IsWindow(hWnd) || !pRT || !ppBitmap) return false;
    *ppBitmap = nullptr;

    int bmpW = 0, bmpH = 0;
    std::vector<BYTE> pixelBytes;

    auto it = m_snapshotCache.find(hWnd);
    bool isIconic = IsIconic(hWnd);
    if (it != m_snapshotCache.end() && !it->second.pixelData.empty() && isIconic) {
        bmpW = it->second.width;
        bmpH = it->second.height;
        pixelBytes = it->second.pixelData;
    } else {
        HBITMAP hCapBmp = nullptr;
        void* pBits = nullptr;
        if (CaptureWindow(hWnd, hCapBmp, pBits, bmpW, bmpH) && hCapBmp && pBits && bmpW > 0 && bmpH > 0) {
            size_t byteCount = (size_t)bmpW * bmpH * 4;
            pixelBytes.resize(byteCount);
            memcpy(pixelBytes.data(), pBits, byteCount);

            CachedSnapshot snap;
            snap.width = bmpW;
            snap.height = bmpH;
            if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &snap.winRect, sizeof(snap.winRect)))) {
                GetWindowRect(hWnd, &snap.winRect);
            }
            snap.isMaximized = IsZoomed(hWnd);
            snap.pixelData = pixelBytes;
            m_snapshotCache[hWnd] = snap;
            DeleteObject(hCapBmp);
        } else if (it != m_snapshotCache.end() && !it->second.pixelData.empty()) {
            bmpW = it->second.width;
            bmpH = it->second.height;
            pixelBytes = it->second.pixelData;
        }
    }

    if (pixelBytes.empty() || bmpW <= 0 || bmpH <= 0) return false;

    outW = bmpW;
    outH = bmpH;

    D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    D2D1_SIZE_U sz = D2D1::SizeU((UINT32)bmpW, (UINT32)bmpH);
    HRESULT hr = pRT->CreateBitmap(sz, pixelBytes.data(), bmpW * 4, bmpProps, ppBitmap);
    return SUCCEEDED(hr) && (*ppBitmap != nullptr);
}

bool WindowAnimator::AnimateWindow(HWND targetHWnd, float iconCenterX, float iconCenterY, float iconWidth, bool isMinimizing) {
    if (!targetHWnd || !IsWindow(targetHWnd)) return false;

    if (m_isAnimating) {
        FinishAnimation();
    }

    // Prune stale cache entries if cache is growing
    if (m_snapshotCache.size() > 16) {
        for (auto it = m_snapshotCache.begin(); it != m_snapshotCache.end(); ) {
            if (!IsWindow(it->first)) {
                it = m_snapshotCache.erase(it);
            } else {
                ++it;
            }
        }
        if (m_snapshotCache.size() > 16) {
            m_snapshotCache.erase(m_snapshotCache.begin());
        }
    }

    m_targetHWnd = targetHWnd;
    m_iconCenterX = iconCenterX;
    m_iconCenterY = iconCenterY;
    m_iconWidth = (iconWidth > 16.0f) ? iconWidth : 48.0f;
    m_isMinimizing = isMinimizing;

    std::vector<BYTE> pixelBytes;
    int bmpW = 0, bmpH = 0;

    if (isMinimizing) {
        // 1. Get current window bounds using extended frame bounds
        if (FAILED(DwmGetWindowAttribute(targetHWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &m_winRect, sizeof(m_winRect)))) {
            GetWindowRect(targetHWnd, &m_winRect);
        }
        int winW = m_winRect.right - m_winRect.left;
        int winH = m_winRect.bottom - m_winRect.top;
        if (winW <= 0 || winH <= 0) return false;

        m_wasMaximized = IsZoomed(targetHWnd);

        // 2. Capture live window bitmap
        HBITMAP hCapBmp = nullptr;
        void* pBits = nullptr;
        if (!CaptureWindow(targetHWnd, hCapBmp, pBits, bmpW, bmpH) || !hCapBmp || !pBits) {
            return false;
        }

        // Cache snapshot for subsequent restore
        size_t byteCount = (size_t)bmpW * bmpH * 4;
        pixelBytes.resize(byteCount);
        memcpy(pixelBytes.data(), pBits, byteCount);

        CachedSnapshot snap;
        snap.width = bmpW;
        snap.height = bmpH;
        snap.winRect = m_winRect;
        snap.isMaximized = m_wasMaximized;
        snap.pixelData = pixelBytes;
        m_snapshotCache[targetHWnd] = snap;

        DeleteObject(hCapBmp);

        // 3. Disable Windows DWM minimize transition for this window
        BOOL disable = TRUE;
        DwmSetWindowAttribute(targetHWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));

        // 4. Instantly minimize the actual window (no jarring Windows animation)
        ShowWindow(targetHWnd, SW_MINIMIZE);

    } else {
        // Restoring
        auto it = m_snapshotCache.find(targetHWnd);
        if (it != m_snapshotCache.end() && !it->second.pixelData.empty()) {
            bmpW = it->second.width;
            bmpH = it->second.height;
            pixelBytes = it->second.pixelData;
            m_winRect = it->second.winRect;
            m_wasMaximized = it->second.isMaximized;
        } else {
            WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
            GetWindowPlacement(targetHWnd, &wp);
            m_wasMaximized = (wp.flags & WPF_RESTORETOMAXIMIZED) != 0 || (wp.showCmd == SW_SHOWMAXIMIZED);

            if (m_wasMaximized) {
                HMONITOR hMon = MonitorFromWindow(targetHWnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(MONITORINFO) };
                GetMonitorInfo(hMon, &mi);
                m_winRect = mi.rcWork;
            } else {
                m_winRect = wp.rcNormalPosition;
            }

            int winW = m_winRect.right - m_winRect.left;
            int winH = m_winRect.bottom - m_winRect.top;
            if (winW <= 0 || winH <= 0) {
                GetWindowRect(targetHWnd, &m_winRect);
                winW = m_winRect.right - m_winRect.left;
                winH = m_winRect.bottom - m_winRect.top;
            }
            if (winW <= 0 || winH <= 0) return false;

            // Disable DWM transition while we capture & play restore animation
            BOOL disable = TRUE;
            DwmSetWindowAttribute(targetHWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));

            // Briefly show to capture snapshot for un-cached window
            ShowWindow(targetHWnd, SW_SHOWNOACTIVATE);
            UpdateWindow(targetHWnd);

            HBITMAP hCapBmp = nullptr;
            void* pBits = nullptr;
            if (CaptureWindow(targetHWnd, hCapBmp, pBits, bmpW, bmpH) && hCapBmp && pBits) {
                size_t byteCount = (size_t)bmpW * bmpH * 4;
                pixelBytes.resize(byteCount);
                memcpy(pixelBytes.data(), pBits, byteCount);

                CachedSnapshot snap;
                snap.width = bmpW;
                snap.height = bmpH;
                snap.winRect = m_winRect;
                snap.isMaximized = m_wasMaximized;
                snap.pixelData = pixelBytes;
                m_snapshotCache[targetHWnd] = snap;

                DeleteObject(hCapBmp);
            }

            ShowWindow(targetHWnd, SW_MINIMIZE);
        }

        if (pixelBytes.empty() || bmpW <= 0 || bmpH <= 0) {
            // Fallback: restore normally
            ShowWindow(targetHWnd, m_wasMaximized ? SW_MAXIMIZE : SW_RESTORE);
            SetForegroundWindow(targetHWnd);
            return false;
        }

        // Disable DWM transition while we play the restore animation
        BOOL disable = TRUE;
        DwmSetWindowAttribute(targetHWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
    }

    m_bmpWidth = bmpW;
    m_bmpHeight = bmpH;

    int winW = m_winRect.right - m_winRect.left;
    int winH = m_winRect.bottom - m_winRect.top;
    if (winW < 4) winW = 4;
    if (winH < 4) winH = 4;

    // Allocate animation DIB buffer once at maximum initial window size
    CreateAnimDIBBuffer(winW, winH);

    // Create D2D GPU bitmap from captured pixels
    if (m_pCapturedBitmap) {
        m_pCapturedBitmap->Release();
        m_pCapturedBitmap = nullptr;
    }

    if (m_pAnimDCRT) {
        D2D1_BITMAP_PROPERTIES bmpProps = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        D2D1_SIZE_U sz = D2D1::SizeU((UINT32)bmpW, (UINT32)bmpH);
        m_pAnimDCRT->CreateBitmap(sz, pixelBytes.data(), bmpW * 4, bmpProps, &m_pCapturedBitmap);
    }

    if (!m_pCapturedBitmap) return false;

    // Position animation layered window directly behind dock window in Z-order
    HWND hWndInsertAfter = HWND_TOPMOST;
    if (m_hDockWnd && IsWindow(m_hDockWnd)) {
        hWndInsertAfter = m_hDockWnd;
    }

    SetWindowPos(
        m_hAnimWnd,
        hWndInsertAfter,
        m_winRect.left, m_winRect.top,
        winW, winH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE
    );

    // Ensure dock stays topmost
    if (m_hDockWnd && IsWindow(m_hDockWnd)) {
        SetWindowPos(m_hDockWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    m_isAnimating = true;
    m_animStartTime = GetTickCount64();

    // Render initial frame
    RenderFrame(0.0f);

    // Start 60+ FPS timer (15ms)
    SetTimer(m_hAnimWnd, TIMER_ANIM_FRAME, 15, NULL);

    return true;
}

void WindowAnimator::OnTimer() {
    if (!m_isAnimating) return;

    ULONGLONG now = GetTickCount64();
    float t = (float)(now - m_animStartTime) / (float)ANIM_DURATION_MS;

    if (t >= 1.0f) {
        RenderFrame(1.0f);
        FinishAnimation();
    } else {
        RenderFrame(t);
    }
}

void WindowAnimator::RenderFrame(float t) {
    if (!m_pAnimDCRT || !m_hdcAnimMem || !m_pCapturedBitmap) return;

    // Progress: if minimizing, t goes 0 -> 1; if restoring, t goes 1 -> 0
    float progress = m_isMinimizing ? t : (1.0f - t);
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    // Smoothstep cubic easing: 3u^2 - 2u^3
    float u = progress * progress * (3.0f - 2.0f * progress);

    float winW = (float)(m_winRect.right - m_winRect.left);
    float winH = (float)(m_winRect.bottom - m_winRect.top);
    float winCenterX = (float)m_winRect.left + (winW / 2.0f);
    float winCenterY = (float)m_winRect.top + (winH / 2.0f);

    float dockCenterX = m_iconCenterX;
    float dockCenterY = m_iconCenterY;
    float dockW = m_iconWidth;
    float dockH = m_iconWidth * 0.75f;

    // Center travels along curved trajectory towards the dock icon
    float curCenterX = (1.0f - u) * winCenterX + u * dockCenterX;
    float curCenterY = (1.0f - u) * winCenterY + u * dockCenterY;

    // macOS Suction Squish: width scales smoothly, height squishes faster with non-linear power
    float curW = (1.0f - u) * winW + u * dockW;
    float hProgress = std::pow(progress, 1.35f);
    float curH = (1.0f - hProgress) * winH + hProgress * dockH;

    if (curW < 4.0f) curW = 4.0f;
    if (curH < 4.0f) curH = 4.0f;

    int scrX = (int)std::floor(curCenterX - (curW / 2.0f));
    int scrY = (int)std::floor(curCenterY - (curH / 2.0f));
    int scrW = (int)std::ceil(curW);
    int scrH = (int)std::ceil(curH);

    // Fade out softly as window reaches dock icon
    float opacity = 1.0f;
    if (progress > 0.82f) {
        opacity = (1.0f - progress) / 0.18f;
        if (opacity < 0.0f) opacity = 0.0f;
    }

    // Bind DC to the exact frame sub-rectangle
    RECT rc = { 0, 0, scrW, scrH };
    m_pAnimDCRT->BindDC(m_hdcAnimMem, &rc);
    m_pAnimDCRT->BeginDraw();
    m_pAnimDCRT->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Hardware bilinear anti-aliased scaling: ZERO jagged steps, ZERO broken pixels!
    D2D1_RECT_F dstRect = D2D1::RectF(0.0f, 0.0f, (float)scrW, (float)scrH);
    D2D1_RECT_F srcRect = D2D1::RectF(0.0f, 0.0f, (float)m_bmpWidth, (float)m_bmpHeight);

    m_pAnimDCRT->DrawBitmap(
        m_pCapturedBitmap,
        dstRect,
        opacity,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
        srcRect
    );

    m_pAnimDCRT->EndDraw();

    // Fast tight-rect layered window update (only transfers scrW x scrH pixels, ~95% faster)
    HDC hdcScreen = GetDC(NULL);
    POINT ptSrc = { 0, 0 };
    SIZE sz = { scrW, scrH };
    POINT ptDst = { scrX, scrY };

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        m_hAnimWnd,
        hdcScreen,
        &ptDst,
        &sz,
        m_hdcAnimMem,
        &ptSrc,
        0,
        &blend,
        ULW_ALPHA
    );

    ReleaseDC(NULL, hdcScreen);

    // Keep animation window positioned directly behind dock
    HWND hWndInsertAfter = (m_hDockWnd && IsWindow(m_hDockWnd)) ? m_hDockWnd : HWND_TOPMOST;
    SetWindowPos(
        m_hAnimWnd,
        hWndInsertAfter,
        scrX, scrY, scrW, scrH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER
    );
}

void WindowAnimator::FinishAnimation() {
    if (!m_isAnimating) return;

    KillTimer(m_hAnimWnd, TIMER_ANIM_FRAME);
    m_isAnimating = false;

    if (m_targetHWnd && IsWindow(m_targetHWnd)) {
        BOOL enable = FALSE;
        if (m_isMinimizing) {
            // Real window is already minimized, re-enable transitions
            DwmSetWindowAttribute(m_targetHWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enable, sizeof(enable));
        } else {
            // Restore actual window
            if (m_wasMaximized) {
                ShowWindow(m_targetHWnd, SW_MAXIMIZE);
            } else {
                ShowWindow(m_targetHWnd, SW_RESTORE);
            }

            // Reliable foreground activation bypassing Windows restrictions
            DWORD curThread = GetCurrentThreadId();
            DWORD targetThread = GetWindowThreadProcessId(m_targetHWnd, NULL);
            if (curThread != targetThread) {
                AttachThreadInput(curThread, targetThread, TRUE);
                SetForegroundWindow(m_targetHWnd);
                SetFocus(m_targetHWnd);
                AttachThreadInput(curThread, targetThread, FALSE);
            } else {
                SetForegroundWindow(m_targetHWnd);
                SetFocus(m_targetHWnd);
            }

            DwmSetWindowAttribute(m_targetHWnd, DWMWA_TRANSITIONS_FORCEDISABLED, &enable, sizeof(enable));
        }
    }

    ShowWindow(m_hAnimWnd, SW_HIDE);

    if (m_pCapturedBitmap) {
        m_pCapturedBitmap->Release();
        m_pCapturedBitmap = nullptr;
    }
}

void WindowAnimator::CancelAnimation() {
    if (m_isAnimating) {
        FinishAnimation();
    }
}

LRESULT CALLBACK WindowAnimator::AnimWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WindowAnimator* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
        pThis = reinterpret_cast<WindowAnimator*>(pCreate->lpCreateParams);
        if (pThis) {
            pThis->m_hAnimWnd = hWnd;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    } else {
        pThis = reinterpret_cast<WindowAnimator*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    }
    if (pThis) {
        return pThis->HandleMessage(hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

LRESULT WindowAnimator::HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_ANIM_FRAME) {
            OnTimer();
            return 0;
        }
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
