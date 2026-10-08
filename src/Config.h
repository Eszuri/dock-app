#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct AppShortcut {
    std::wstring name;
    std::wstring targetPath;
    std::wstring arguments;
    std::wstring iconSource; // Optional: specific path or dll for icon
};

namespace Config {
    // Sizing (Logical pixels)
    constexpr float BASE_ICON_SIZE = 48.0f;
    constexpr float MAX_ICON_SCALE = 1.60f;
    constexpr float INFLUENCE_RADIUS = 135.0f;
    constexpr float DOCK_PADDING_X = 14.0f;
    constexpr float DOCK_PADDING_Y = 10.0f;
    constexpr float ITEM_SPACING = 12.0f;
    constexpr float CORNER_RADIUS = 18.0f;
    constexpr float BOTTOM_MARGIN = 12.0f;

    // Animation physics
    constexpr float LERP_FACTOR = 0.26f; // Smooth spring/lerp factor per frame
    constexpr float SETTLE_THRESHOLD = 0.002f;

    // Visual appearance (RGBA)
    // Dock background: dark translucent frosted glass
    constexpr D2D1_COLOR_F BG_COLOR = { 0.12f, 0.12f, 0.14f, 0.65f };
    constexpr D2D1_COLOR_F BORDER_COLOR = { 1.0f, 1.0f, 1.0f, 0.22f };
    constexpr float BORDER_WIDTH = 1.0f;

    // Indicator dot color (active app)
    constexpr D2D1_COLOR_F INDICATOR_COLOR = { 0.90f, 0.90f, 0.95f, 0.85f };

    // Default applications to pin
    inline std::vector<AppShortcut> GetDefaultApps() {
        return {
            { L"File Explorer", L"explorer.exe", L"", L"shell32.dll" },
            { L"Terminal / CMD", L"cmd.exe", L"", L"" },
            { L"Notepad", L"notepad.exe", L"", L"" },
            { L"Task Manager", L"taskmgr.exe", L"", L"" },
            { L"Calculator", L"calc.exe", L"", L"" },
            { L"Microsoft Edge", L"msedge.exe", L"", L"" },
            { L"Settings", L"ms-settings:", L"", L"shell32.dll" }
        };
    }
}
