#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <vector>
#include <map>

class WindowAnimator {
public:
    WindowAnimator();
    ~WindowAnimator();

    bool Initialize(HINSTANCE hInstance, ID2D1Factory* pD2DFactory);
    void Cleanup();

    // Start macOS Genie minimize or restore animation
    // iconCenterX, iconCenterY, iconWidth: dock icon position in screen space
    bool AnimateWindow(HWND targetHWnd, float iconCenterX, float iconCenterY, float iconWidth, bool isMinimizing);

    void PrecacheWindowSnapshot(HWND hWnd);
    bool GetSnapshotBitmap(HWND hWnd, ID2D1RenderTarget* pRT, ID2D1Bitmap** ppBitmap, int& outW, int& outH);
    void SetDockHWnd(HWND hDockWnd) { m_hDockWnd = hDockWnd; }
    HWND GetAnimWnd() const { return m_hAnimWnd; }

    void CancelAnimation();

    static LRESULT CALLBACK AnimWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    LRESULT HandleMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnTimer();
    void RenderFrame(float t);
    void FinishAnimation();

    bool CaptureWindow(HWND hWnd, HBITMAP& outBmp, void*& outBits, int& outW, int& outH);
    void CreateAnimDIBBuffer(int width, int height);

private:
    HINSTANCE m_hInstance = nullptr;
    ID2D1Factory* m_pD2DFactory = nullptr;
    HWND m_hAnimWnd = nullptr;
    HWND m_hDockWnd = nullptr;

    HDC m_hdcAnimMem = nullptr;
    HBITMAP m_hAnimBitmap = nullptr;
    HBITMAP m_hAnimOldBitmap = nullptr;
    void* m_pvAnimBits = nullptr;
    int m_bufferWidth = 0;
    int m_bufferHeight = 0;

    ID2D1DCRenderTarget* m_pAnimDCRT = nullptr;
    ID2D1Bitmap* m_pCapturedBitmap = nullptr;

    // Animation state
    bool m_isAnimating = false;
    bool m_isMinimizing = true;
    bool m_wasMaximized = false;
    HWND m_targetHWnd = nullptr;
    RECT m_winRect = {};
    float m_iconCenterX = 0.0f;
    float m_iconCenterY = 0.0f;
    float m_iconWidth = 48.0f;

    int m_bmpWidth = 0;
    int m_bmpHeight = 0;

    int m_animScreenX = 0;
    int m_animScreenY = 0;
    int m_animWidth = 0;
    int m_animHeight = 0;

    ULONGLONG m_animStartTime = 0;
    static constexpr DWORD ANIM_DURATION_MS = 280; // 280ms duration
    static constexpr UINT_PTR TIMER_ANIM_FRAME = 2001;

    struct CachedSnapshot {
        std::vector<BYTE> pixelData;
        int width = 0;
        int height = 0;
        RECT winRect = {};
        bool isMaximized = false;
    };
    std::map<HWND, CachedSnapshot> m_snapshotCache;
};
