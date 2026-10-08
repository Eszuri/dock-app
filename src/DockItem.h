#pragma once
#include <windows.h>
#include <d2d1.h>
#include <string>
#include <vector>
#include "Config.h"

struct DockWindowEntry {
    HWND hWnd = nullptr;
    std::wstring title;
    bool isForeground = false;
};

struct DockItem {
    std::wstring name;          // Clean application friendly name
    std::wstring windowTitle;   // Specific window title (document name, page, etc.)
    std::wstring launchPath;    // Path to .lnk or executable
    std::wstring exeFilename;   // Lowercase exe filename (e.g. "notepad.exe")
    HWND hWnd = nullptr;        // Associated window handle
    std::vector<DockWindowEntry> openWindows; // All open windows for this app

    bool isPinned = false;
    bool isRunning = false;
    bool isForeground = false;

    ID2D1Bitmap* pBitmap = nullptr;
    
    // Animation states
    float currentScale = 1.0f;
    float targetScale = 1.0f;
    
    // Bounds & animation in window coordinates
    float currentX = 0.0f;
    float targetX = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float width = Config::BASE_ICON_SIZE;
    float height = Config::BASE_ICON_SIZE;
    float centerX = 0.0f;

    // Bounce animation on click
    float bounceY = 0.0f;
    float bounceVelocity = 0.0f;

    DockItem() = default;

    DockItem(const DockItem&) = delete;
    DockItem& operator=(const DockItem&) = delete;

    DockItem(DockItem&& other) noexcept
        : name(std::move(other.name))
        , windowTitle(std::move(other.windowTitle))
        , launchPath(std::move(other.launchPath))
        , exeFilename(std::move(other.exeFilename))
        , hWnd(other.hWnd)
        , openWindows(std::move(other.openWindows))
        , isPinned(other.isPinned)
        , isRunning(other.isRunning)
        , isForeground(other.isForeground)
        , pBitmap(other.pBitmap)
        , currentScale(other.currentScale)
        , targetScale(other.targetScale)
        , currentX(other.currentX)
        , targetX(other.targetX)
        , x(other.x)
        , y(other.y)
        , width(other.width)
        , height(other.height)
        , centerX(other.centerX)
        , bounceY(other.bounceY)
        , bounceVelocity(other.bounceVelocity)
    {
        other.pBitmap = nullptr;
    }

    DockItem& operator=(DockItem&& other) noexcept {
        if (this != &other) {
            if (pBitmap) pBitmap->Release();
            name = std::move(other.name);
            windowTitle = std::move(other.windowTitle);
            launchPath = std::move(other.launchPath);
            exeFilename = std::move(other.exeFilename);
            hWnd = other.hWnd;
            openWindows = std::move(other.openWindows);
            isPinned = other.isPinned;
            isRunning = other.isRunning;
            isForeground = other.isForeground;
            pBitmap = other.pBitmap;
            other.pBitmap = nullptr;
            currentScale = other.currentScale;
            targetScale = other.targetScale;
            currentX = other.currentX;
            targetX = other.targetX;
            x = other.x;
            y = other.y;
            width = other.width;
            height = other.height;
            centerX = other.centerX;
            bounceY = other.bounceY;
            bounceVelocity = other.bounceVelocity;
        }
        return *this;
    }

    ~DockItem() {
        if (pBitmap) {
            pBitmap->Release();
            pBitmap = nullptr;
        }
    }
};
