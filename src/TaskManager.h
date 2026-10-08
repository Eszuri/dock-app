#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct PinnedAppInfo {
    std::wstring name;
    std::wstring lnkPath;       // Full path to .lnk
    std::wstring resolvedExe;   // Lowercase target .exe filename for matching
};

struct RunningWindowInfo {
    HWND hWnd = nullptr;
    std::wstring title;
    std::wstring fullExePath;
    std::wstring exeFilename;   // Lowercase exe filename (e.g. "notepad.exe")
    bool isForeground = false;
};

class TaskManager {
public:
    // Read pinned shortcuts from User Pinned TaskBar folder
    static std::vector<PinnedAppInfo> LoadPinnedApps();

    // Enumerate open top-level application windows that belong on taskbar
    static std::vector<RunningWindowInfo> GetOpenWindows(HWND hDockWnd);

    // Helper: Is this window a genuine taskbar app window?
    static bool IsTaskbarWindow(HWND hWnd, HWND hDockWnd);

    // Helper: Get full process executable path and filename for HWND
    static bool GetWindowProcessInfo(HWND hWnd, std::wstring& outFullPath, std::wstring& outExeFilename);

    // Helper: Activate, restore, or minimize window like native Windows Taskbar
    static void ToggleWindowState(HWND hWnd, bool wasForegroundHint = false);

    // Helper: Resolve shortcut target path (.lnk -> .exe)
    static std::wstring ResolveShortcutTarget(const std::wstring& lnkPath);

    // Helper: Pin application to Windows Taskbar folder
    static bool PinApp(const std::wstring& targetPath, const std::wstring& displayName);

    // Helper: Unpin application from Windows Taskbar folder
    static bool UnpinApp(const std::wstring& lnkOrExePath, const std::wstring& exeFilename);

    // Helper: Extract clean application friendly name (e.g. from FileDescription)
    static std::wstring GetAppFriendlyName(const std::wstring& fullExePath, const std::wstring& fallbackTitle);
};

