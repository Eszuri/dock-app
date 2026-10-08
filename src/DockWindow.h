#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <vector>
#include "Config.h"
#include "DockItem.h"
#include "TaskManager.h"
#include "WindowAnimator.h"

class DockWindow {
public:
    DockWindow();
    ~DockWindow();

    bool Initialize(HINSTANCE hInstance);
    void Run();

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool HitTest(int x, int y) const;

    void InitDirect2D();
    void CleanupDirect2D();
    void CreateDIBBuffer(int width, int height);
    void CleanupDIBBuffer();

    void RefreshTaskbarItems();
    void ResizeCanvas(size_t itemCount);
    void UpdateLayout(bool checkHover);
    void Render();

    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnLButtonDown(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnRButtonUp(int x, int y);
    void OnTimer();

    void PositionWindow();
    void SaveCustomOrder();
    void LoadCustomOrder(std::vector<PinnedAppInfo>& pinnedApps);

    // Context menus
    void ShowModernAppContextMenu(int itemIndex);
    void ShowModernDockContextMenu();
    void ShowWindowSystemMenu(HWND hWnd, int screenX, int screenY);
    void CloseModernMenu();
    void ExecuteMenuCommand(int cmd, HWND targetHWnd);

    static LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMenuMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void InitMenuWindow(HINSTANCE hInstance);
    void CleanupMenuWindow();
    void CreateMenuDIBBuffer(int width, int height);
    void RenderMenu();
    bool IsMenuOpen() const { return m_isMenuOpen; }
    RECT GetMenuScreenRect() const;

    // Window App Preview Panel (Native Windows style)
    void InitPreviewWindow(HINSTANCE hInstance);
    void CleanupPreviewWindow();
    void CreatePreviewDIBBuffer(int width, int height);
    void ShowPreviewPanel(int itemIndex);
    void HidePreviewPanel();
    void HidePreviewPanelImmediate();
    void RenderPreviewPanel();
    void OnPreviewAnimTimer();
    bool IsPreviewOpen() const { return m_isPreviewOpen; }

    static LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandlePreviewMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

private:
    HINSTANCE m_hInstance = nullptr;
    HWND m_hWnd = nullptr;

    // Direct2D & DirectWrite
    ID2D1Factory* m_pD2DFactory = nullptr;
    IDWriteFactory* m_pDWriteFactory = nullptr;
    IDWriteTextFormat* m_pTextFormat = nullptr;
    ID2D1DCRenderTarget* m_pDCRT = nullptr;

    // Brushes
    ID2D1SolidColorBrush* m_pBgBrush = nullptr;
    ID2D1SolidColorBrush* m_pBorderBrush = nullptr;
    ID2D1SolidColorBrush* m_pIndicatorBrush = nullptr;
    ID2D1SolidColorBrush* m_pTooltipBgBrush = nullptr;
    ID2D1SolidColorBrush* m_pTooltipTextBrush = nullptr;

    // DIB Section for Layered Window
    HDC m_hdcMem = nullptr;
    HBITMAP m_hBitmap = nullptr;
    HBITMAP m_hOldBitmap = nullptr;
    void* m_pvBits = nullptr;

    int m_canvasWidth = 0;
    int m_canvasHeight = 0;
    int m_screenX = 0;
    int m_screenY = 0;

    // Dock Items
    std::vector<DockItem> m_items;
    int m_hoveredIndex = -1;
    bool m_isMouseOverDock = false;
    bool m_isAnimating = false;

    float m_mouseX = -9999.0f;
    float m_mouseY = -9999.0f;

    // Cached pill geometry
    D2D1_ROUNDED_RECT m_dockPillRect = {};

    static constexpr UINT_PTR TIMER_ANIM = 1001;
    static constexpr UINT_PTR TIMER_CHECK_RUNNING = 1002;

    UINT m_shellHookMsg = 0;
    ID2D1SolidColorBrush* m_pActiveIndicatorBrush = nullptr;

    // Drag and drop icon reordering
    bool m_isLButtonDown = false;
    bool m_isDragging = false;
    int m_dragCandidateIndex = -1;
    int m_draggedIndex = -1;
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;
    HWND m_hForegroundBeforeClick = nullptr;
    HWND m_lastActiveHWnd = nullptr;

    // Context menu state & custom Direct2D popup window
    bool m_isMenuOpen = false;

    struct CustomMenuItem {
        int id = 0;
        std::wstring text;
        enum class IconType {
            AppIcon,
            Pin,
            Unpin,
            Close,
            Refresh,
            Exit,
            None
        } iconType = IconType::None;
        ID2D1Bitmap* pBitmap = nullptr;
        bool isSeparator = false;
        HWND targetHWnd = nullptr;
    };

    HWND m_hMenuWnd = nullptr;
    HDC m_hdcMenuMem = nullptr;
    HBITMAP m_hMenuBitmap = nullptr;
    HBITMAP m_hMenuOldBitmap = nullptr;
    IDWriteTextFormat* m_pMenuTextFormat = nullptr;
    IDWriteTextFormat* m_pMenuIconFormat = nullptr;
    ID2D1SolidColorBrush* m_pMenuBgBrush = nullptr;
    ID2D1SolidColorBrush* m_pMenuBorderBrush = nullptr;
    ID2D1SolidColorBrush* m_pMenuHoverBrush = nullptr;
    ID2D1SolidColorBrush* m_pMenuTextBrush = nullptr;
    ID2D1SolidColorBrush* m_pMenuCloseRedBrush = nullptr;
    ID2D1SolidColorBrush* m_pMenuWhiteBrush = nullptr;
    ID2D1SolidColorBrush* m_pMenuSeparatorBrush = nullptr;

    std::vector<CustomMenuItem> m_menuItems;
    int m_menuHoverIndex = -1;
    int m_menuWidth = 0;
    int m_menuHeight = 0;
    int m_menuScreenX = 0;
    int m_menuScreenY = 0;
    int m_menuItemIndex = -1;

    static HHOOK s_hMenuMouseHook;
    static HHOOK s_hMenuKbdHook;
    static DockWindow* s_pMenuInstance;

    // Window App Preview Panel state (Multi-Window & Single-Window support)
    static constexpr UINT_PTR TIMER_PREVIEW_HOVER = 1003;
    static constexpr UINT_PTR TIMER_PREVIEW_CLOSE = 1004;
    static constexpr UINT_PTR TIMER_PREVIEW_ANIM  = 1005;

    struct PreviewCardItem {
        HWND hWnd = nullptr;
        std::wstring title;
        ID2D1Bitmap* pThumbnail = nullptr;
        int thumbSrcW = 0;
        int thumbSrcH = 0;
        D2D1_RECT_F cardRect = {};
        D2D1_RECT_F closeBtnRect = {};
        D2D1_RECT_F thumbRect = {};
    };

    HWND m_hPreviewWnd = nullptr;
    HDC m_hdcPreviewMem = nullptr;
    HBITMAP m_hPreviewBitmap = nullptr;
    HBITMAP m_hPreviewOldBitmap = nullptr;
    void* m_pvPreviewBits = nullptr;

    bool m_isPreviewOpen = false;
    bool m_isPreviewClosing = false;
    int m_previewAppIndex = -1;
    ID2D1Bitmap* m_pPreviewIcon = nullptr;

    std::vector<PreviewCardItem> m_previewCards;
    int m_previewHoveredCard = -1;
    bool m_previewCloseHovered = false;
    bool m_previewThumbHovered = false;

    // Smooth transition / glide animation physics
    float m_previewCurrentX = 0.0f;
    float m_previewTargetX = 0.0f;
    float m_previewCurrentY = 0.0f;
    float m_previewTargetY = 0.0f;
    float m_previewAlpha = 0.0f;
    float m_previewTargetAlpha = 0.0f;

    int m_previewWidth = 236;
    int m_previewHeight = 168;
    int m_previewScreenX = 0;
    int m_previewScreenY = 0;
    int m_previewBufferWidth = 0;
    int m_previewBufferHeight = 0;

    void EnsurePreviewBufferSize(int width, int height);

    WindowAnimator m_animator;
};
