#include "IconHelper.h"
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <vector>
#include <cmath>
#include <algorithm>

IWICImagingFactory* IconHelper::s_pWicFactory = nullptr;

static std::wstring ResolvePath(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.rfind(L"ms-settings:", 0) == 0) {
        wchar_t sysDir[MAX_PATH] = {};
        GetSystemDirectoryW(sysDir, MAX_PATH);
        return std::wstring(sysDir) + L"\\Shell32.dll";
    }
    if (path.find(L':') != std::wstring::npos || path.find(L'\\') != std::wstring::npos) {
        return path;
    }
    wchar_t resolved[MAX_PATH] = {};
    if (SearchPathW(NULL, path.c_str(), L".exe", MAX_PATH, resolved, NULL) > 0) {
        return resolved;
    }
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    std::wstring candidate = std::wstring(winDir) + L"\\" + path;
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;

    GetSystemDirectoryW(winDir, MAX_PATH);
    candidate = std::wstring(winDir) + L"\\" + path;
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;

    return path;
}

static HICON ExtractJumboIcon(const std::wstring& path) {
    SHFILEINFOW sfi = {};
    DWORD_PTR res = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
    if (!res) return nullptr;

    IImageList* pImageList = nullptr;
    HRESULT hr = SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&pImageList));
    HICON hIcon = nullptr;
    if (SUCCEEDED(hr) && pImageList) {
        pImageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon);
        pImageList->Release();
    }

    if (!hIcon) {
        hr = SHGetImageList(SHIL_EXTRALARGE, IID_PPV_ARGS(&pImageList));
        if (SUCCEEDED(hr) && pImageList) {
            pImageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon);
            pImageList->Release();
        }
    }

    if (!hIcon) {
        hr = SHGetImageList(SHIL_LARGE, IID_PPV_ARGS(&pImageList));
        if (SUCCEEDED(hr) && pImageList) {
            pImageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon);
            pImageList->Release();
        }
    }

    return hIcon;
}

bool IconHelper::Initialize() {
    if (s_pWicFactory) return true;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&s_pWicFactory)
    );
    return SUCCEEDED(hr);
}

void IconHelper::Shutdown() {
    if (s_pWicFactory) {
        s_pWicFactory->Release();
        s_pWicFactory = nullptr;
    }
}

ID2D1Bitmap* IconHelper::WicBitmapToD2D(ID2D1RenderTarget* pRT, IWICBitmapSource* pSource) {
    if (!s_pWicFactory || !pSource || !pRT) return nullptr;

    IWICFormatConverter* pConverter = nullptr;
    HRESULT hr = s_pWicFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) return nullptr;

    hr = pConverter->Initialize(
        pSource,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        NULL,
        0.0f,
        WICBitmapPaletteTypeCustom
    );

    ID2D1Bitmap* pBitmap = nullptr;
    if (SUCCEEDED(hr)) {
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        pRT->CreateBitmapFromWicBitmap(pConverter, &props, &pBitmap);
    }

    pConverter->Release();
    return pBitmap;
}

ID2D1Bitmap* IconHelper::CreateFallbackBitmap(ID2D1RenderTarget* pRT, const std::wstring& label) {
    UINT32 width = 64;
    UINT32 height = 64;
    D2D1_SIZE_U size = D2D1::SizeU(width, height);
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    std::vector<UINT32> pixels(width * height);
    for (UINT32 y = 0; y < height; ++y) {
        for (UINT32 x = 0; x < width; ++x) {
            float dx = (float)x - 32.0f;
            float dy = (float)y - 32.0f;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist <= 28.0f) {
                UINT8 alpha = 230;
                UINT8 r = (UINT8)(40 + y);
                UINT8 g = (UINT8)(120 + y);
                UINT8 b = 230;
                UINT8 pr = (UINT8)((r * alpha) / 255);
                UINT8 pg = (UINT8)((g * alpha) / 255);
                UINT8 pb = (UINT8)((b * alpha) / 255);
                pixels[y * width + x] = ((UINT32)alpha << 24) | ((UINT32)pr << 16) | ((UINT32)pg << 8) | pb;
            } else {
                pixels[y * width + x] = 0x00000000;
            }
        }
    }

    ID2D1Bitmap* pBitmap = nullptr;
    pRT->CreateBitmap(size, pixels.data(), width * 4, props, &pBitmap);
    return pBitmap;
}

ID2D1Bitmap* IconHelper::CreateNormalizedBitmapFromHICON(ID2D1RenderTarget* pRT, HICON hIcon) {
    if (!hIcon || !pRT) return nullptr;

    ICONINFO ii = {};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    BITMAP bm = {};
    GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    int rawW = bm.bmWidth;
    int rawH = bm.bmHeight;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER), rawW, -rawH, 1, 32, BI_RGB };

    std::vector<UINT32> srcPixels(rawW * rawH);
    GetDIBits(hdcMem, ii.hbmColor ? ii.hbmColor : ii.hbmMask, 0, rawH, srcPixels.data(), &bmi, DIB_RGB_COLORS);

    int minX = rawW, maxX = 0, minY = rawH, maxY = 0;
    int solidCount = 0;
    for (int y = 0; y < rawH; ++y) {
        for (int x = 0; x < rawW; ++x) {
            UINT32 px = srcPixels[y * rawW + x];
            UINT8 a = (px >> 24) & 0xFF;
            if (a > 15) {
                solidCount++;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);

    if (solidCount == 0 || maxX < minX || maxY < minY) {
        if (s_pWicFactory) {
            IWICBitmap* pWicBitmap = nullptr;
            if (SUCCEEDED(s_pWicFactory->CreateBitmapFromHICON(hIcon, &pWicBitmap)) && pWicBitmap) {
                ID2D1Bitmap* pD2D = WicBitmapToD2D(pRT, pWicBitmap);
                pWicBitmap->Release();
                return pD2D;
            }
        }
        return nullptr;
    }

    int activeW = maxX - minX + 1;
    int activeH = maxY - minY + 1;

    // Check if the icon is tucked into a small corner/region (e.g. 48x48 inside 256x256)
    bool isCornerTucked = (activeW <= (int)(rawW * 0.70f) && activeH <= (int)(rawH * 0.70f));

    // If icon is already full-bleed / standard size, preserve native WIC bitmap directly
    if (!isCornerTucked && s_pWicFactory) {
        IWICBitmap* pWicBitmap = nullptr;
        if (SUCCEEDED(s_pWicFactory->CreateBitmapFromHICON(hIcon, &pWicBitmap)) && pWicBitmap) {
            ID2D1Bitmap* pD2D = WicBitmapToD2D(pRT, pWicBitmap);
            pWicBitmap->Release();
            if (pD2D) return pD2D;
        }
    }

    // For corner-tucked icons (e.g. pangobright, text-editor), clip to active bounds & scale with Bicubic WIC
    if (isCornerTucked && s_pWicFactory) {
        IWICBitmap* pWicBitmap = nullptr;
        if (SUCCEEDED(s_pWicFactory->CreateBitmapFromHICON(hIcon, &pWicBitmap)) && pWicBitmap) {
            IWICBitmapClipper* pClipper = nullptr;
            if (SUCCEEDED(s_pWicFactory->CreateBitmapClipper(&pClipper)) && pClipper) {
                WICRect rc = { minX, minY, activeW, activeH };
                if (SUCCEEDED(pClipper->Initialize(pWicBitmap, &rc))) {
                    IWICBitmapScaler* pScaler = nullptr;
                    if (SUCCEEDED(s_pWicFactory->CreateBitmapScaler(&pScaler)) && pScaler) {
                        int targetSize = 128;
                        float scale = (float)targetSize / (float)std::max(activeW, activeH);
                        UINT targetW = (UINT)(activeW * scale);
                        UINT targetH = (UINT)(activeH * scale);
                        if (SUCCEEDED(pScaler->Initialize(pClipper, targetW, targetH, WICBitmapInterpolationModeCubic))) {
                            ID2D1Bitmap* pD2D = WicBitmapToD2D(pRT, pScaler);
                            pScaler->Release();
                            pClipper->Release();
                            pWicBitmap->Release();
                            if (pD2D) return pD2D;
                        }
                        pScaler->Release();
                    }
                }
                pClipper->Release();
            }
            pWicBitmap->Release();
        }
    }

    // Bilinear fallback if WIC clipping is unavailable
    int targetSize = 128;
    int pad = 8;
    int innerSize = targetSize - pad * 2;
    float scale = (float)innerSize / (float)std::max(activeW, activeH);
    int drawW = (int)(activeW * scale);
    int drawH = (int)(activeH * scale);
    int startX = (targetSize - drawW) / 2;
    int startY = (targetSize - drawH) / 2;

    std::vector<UINT32> dstPixels(targetSize * targetSize, 0x00000000);
    for (int dy = 0; dy < drawH; ++dy) {
        for (int dx = 0; dx < drawW; ++dx) {
            int sx = minX + (int)((float)dx / scale);
            int sy = minY + (int)((float)dy / scale);
            if (sx >= 0 && sx < rawW && sy >= 0 && sy < rawH) {
                UINT32 px = srcPixels[sy * rawW + sx];
                UINT8 a = (px >> 24) & 0xFF;
                UINT8 r = (px >> 16) & 0xFF;
                UINT8 g = (px >> 8) & 0xFF;
                UINT8 b = px & 0xFF;
                if (a > 0 && a < 255) {
                    r = (r * a) / 255;
                    g = (g * a) / 255;
                    b = (b * a) / 255;
                    px = ((UINT32)a << 24) | ((UINT32)r << 16) | ((UINT32)g << 8) | b;
                }
                dstPixels[(startY + dy) * targetSize + (startX + dx)] = px;
            }
        }
    }

    D2D1_SIZE_U size = D2D1::SizeU(targetSize, targetSize);
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    ID2D1Bitmap* pBitmap = nullptr;
    HRESULT hr = pRT->CreateBitmap(size, dstPixels.data(), targetSize * 4, props, &pBitmap);
    return SUCCEEDED(hr) ? pBitmap : nullptr;
}

ID2D1Bitmap* IconHelper::CreateIconBitmap(
    ID2D1RenderTarget* pRT,
    const std::wstring& targetPath,
    const std::wstring& iconSource,
    const std::wstring& fallbackName
) {
    if (!s_pWicFactory) Initialize();
    if (!pRT) return nullptr;

    std::wstring rawSearchPath = !iconSource.empty() ? iconSource : targetPath;
    std::wstring searchPath = ResolvePath(rawSearchPath);

    // Strategy 1: High-res Shell Image List (256x256 Jumbo, unpadded)
    HICON hJumbo = ExtractJumboIcon(searchPath);
    if (hJumbo) {
        ID2D1Bitmap* pD2D = CreateNormalizedBitmapFromHICON(pRT, hJumbo);
        DestroyIcon(hJumbo);
        if (pD2D) return pD2D;
    }

    // Strategy 2: Direct PrivateExtractIcons from executable
    HICON hPriv = nullptr;
    UINT id = 0;
    if (PrivateExtractIconsW(searchPath.c_str(), 0, 256, 256, &hPriv, &id, 1, LR_LOADFROMFILE) > 0 && hPriv) {
        ID2D1Bitmap* pD2D = CreateNormalizedBitmapFromHICON(pRT, hPriv);
        DestroyIcon(hPriv);
        if (pD2D) return pD2D;
    }

    // Strategy 3: Standard SHGetFileInfo
    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(searchPath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon) {
        ID2D1Bitmap* pD2D = CreateNormalizedBitmapFromHICON(pRT, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        if (pD2D) return pD2D;
    }

    // Fallback if icon cannot be loaded
    return CreateFallbackBitmap(pRT, fallbackName);
}

ID2D1Bitmap* IconHelper::CreateIconFromHWND(
    ID2D1RenderTarget* pRT,
    HWND hWnd,
    const std::wstring& fallbackExePath,
    const std::wstring& fallbackName
) {
    if (!s_pWicFactory) Initialize();
    if (!pRT) return nullptr;

    // 1. If we have the executable path, get the 256x256 Jumbo shell icon
    if (!fallbackExePath.empty()) {
        HICON hJumbo = ExtractJumboIcon(fallbackExePath);
        if (hJumbo) {
            ID2D1Bitmap* pD2D = CreateNormalizedBitmapFromHICON(pRT, hJumbo);
            DestroyIcon(hJumbo);
            if (pD2D) return pD2D;
        }
    }

    // 2. Query HWND directly
    HICON hIcon = nullptr;
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(hWnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &result) && result) {
        hIcon = (HICON)result;
    } else if (SendMessageTimeoutW(hWnd, WM_GETICON, ICON_SMALL2, 0, SMTO_ABORTIFHUNG, 100, &result) && result) {
        hIcon = (HICON)result;
    }

    if (!hIcon) {
        hIcon = (HICON)GetClassLongPtrW(hWnd, GCLP_HICON);
    }
    if (!hIcon) {
        hIcon = (HICON)GetClassLongPtrW(hWnd, GCLP_HICONSM);
    }

    if (hIcon) {
        ID2D1Bitmap* pD2D = CreateNormalizedBitmapFromHICON(pRT, hIcon);
        // Note: Do not DestroyIcon on class or WM_GETICON shared icons
        if (pD2D) return pD2D;
    }

    // 3. Fallback to process executable icon
    return CreateIconBitmap(pRT, fallbackExePath, L"", fallbackName);
}
