#pragma once
#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <string>

class IconHelper {
public:
    static bool Initialize();
    static void Shutdown();

    // Loads or extracts an icon for the given path/name and converts it to ID2D1Bitmap
    static ID2D1Bitmap* CreateIconBitmap(
        ID2D1RenderTarget* pRT,
        const std::wstring& targetPath,
        const std::wstring& iconSource,
        const std::wstring& fallbackName
    );

    // Extract icon directly from an open HWND or fallback to its executable path
    static ID2D1Bitmap* CreateIconFromHWND(
        ID2D1RenderTarget* pRT,
        HWND hWnd,
        const std::wstring& fallbackExePath,
        const std::wstring& fallbackName
    );

    // Auto-crop and normalize any icon to full-bleed square bitmap
    static ID2D1Bitmap* CreateNormalizedBitmapFromHICON(ID2D1RenderTarget* pRT, HICON hIcon);

private:
    static IWICImagingFactory* s_pWicFactory;

    static ID2D1Bitmap* WicBitmapToD2D(ID2D1RenderTarget* pRT, IWICBitmapSource* pSource);
    static ID2D1Bitmap* CreateFallbackBitmap(ID2D1RenderTarget* pRT, const std::wstring& label);
};
