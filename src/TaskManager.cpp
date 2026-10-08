#include "TaskManager.h"
#include <shlobj.h>
#include <dwmapi.h>
#include <winver.h>
#include <algorithm>

static std::wstring ToLower(const std::wstring& str) {
    std::wstring s = str;
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

static std::wstring GetFilenameFromPath(const std::wstring& path) {
    size_t lastSlash = path.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return path.substr(lastSlash + 1);
    }
    return path;
}

std::vector<PinnedAppInfo> TaskManager::LoadPinnedApps() {
    std::vector<PinnedAppInfo> pinned;

    wchar_t appData[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    std::wstring folder = std::wstring(appData) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((folder + L"\\*.lnk").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        IShellLinkW* psl = nullptr;
        CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
        IPersistFile* ppf = nullptr;
        if (psl) psl->QueryInterface(IID_PPV_ARGS(&ppf));

        do {
            std::wstring filename = fd.cFileName;
            std::wstring fullPath = folder + L"\\" + filename;
            std::wstring target = L"";

            if (ppf && SUCCEEDED(ppf->Load(fullPath.c_str(), STGM_READ))) {
                wchar_t targetBuf[MAX_PATH] = {};
                psl->GetPath(targetBuf, MAX_PATH, NULL, SLGP_UNCPRIORITY);
                target = targetBuf;
            }

            // Display name without .lnk
            std::wstring displayName = filename;
            if (displayName.length() > 4 && displayName.substr(displayName.length() - 4) == L".lnk") {
                displayName = displayName.substr(0, displayName.length() - 4);
            }

            std::wstring resolvedExe = ToLower(GetFilenameFromPath(target));
            if (resolvedExe.empty()) {
                if (ToLower(displayName).find(L"explorer") != std::wstring::npos) {
                    resolvedExe = L"explorer.exe";
                }
            }

            PinnedAppInfo info;
            info.name = displayName;
            info.lnkPath = fullPath;
            info.resolvedExe = resolvedExe;
            pinned.push_back(info);

        } while (FindNextFileW(hFind, &fd));

        if (ppf) ppf->Release();
        if (psl) psl->Release();
        FindClose(hFind);
    }

    // Fallback if no pinned apps found in user profile
    if (pinned.empty()) {
        pinned.push_back({ L"File Explorer", L"explorer.exe", L"explorer.exe" });
        pinned.push_back({ L"Notepad", L"notepad.exe", L"notepad.exe" });
        pinned.push_back({ L"Terminal", L"cmd.exe", L"cmd.exe" });
        pinned.push_back({ L"Task Manager", L"taskmgr.exe", L"taskmgr.exe" });
    }

    return pinned;
}

bool TaskManager::GetWindowProcessInfo(HWND hWnd, std::wstring& outFullPath, std::wstring& outExeFilename) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    bool ok = false;
    if (QueryFullProcessImageNameW(hProc, 0, path, &size)) {
        outFullPath = path;
        outExeFilename = ToLower(GetFilenameFromPath(path));
        ok = true;
    }
    CloseHandle(hProc);

    // If it's ApplicationFrameHost, resolve real UWP process from child CoreWindow
    if (outExeFilename == L"applicationframehost.exe") {
        DWORD realPid = 0;
        EnumChildWindows(hWnd, [](HWND hChild, LPARAM lParam) -> BOOL {
            wchar_t cls[256] = {};
            GetClassNameW(hChild, cls, 256);
            if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) {
                DWORD childPid = 0;
                GetWindowThreadProcessId(hChild, &childPid);
                if (childPid != 0) {
                    *reinterpret_cast<DWORD*>(lParam) = childPid;
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&realPid));

        if (realPid != 0 && realPid != pid) {
            HANDLE hRealProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, realPid);
            if (hRealProc) {
                wchar_t realPath[MAX_PATH] = {};
                DWORD realSize = MAX_PATH;
                if (QueryFullProcessImageNameW(hRealProc, 0, realPath, &realSize)) {
                    outFullPath = realPath;
                    outExeFilename = ToLower(GetFilenameFromPath(realPath));
                }
                CloseHandle(hRealProc);
            }
        }
    }

    return ok;
}

bool TaskManager::IsTaskbarWindow(HWND hWnd, HWND hDockWnd) {
    if (!hWnd || hWnd == hDockWnd) return false;
    if (!IsWindowVisible(hWnd)) return false;
    if (GetWindowTextLengthW(hWnd) == 0) return false;

    LONG_PTR exStyle = GetWindowLongPtrW(hWnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    HWND hOwner = GetWindow(hWnd, GW_OWNER);
    if (hOwner != NULL && !(exStyle & WS_EX_APPWINDOW)) return false;

    // Check Cloaked (virtual desktops / suspended UWP)
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        if (cloaked != 0) return false;
    }

    // Exclude shell system windows
    wchar_t clsName[256] = {};
    GetClassNameW(hWnd, clsName, 256);
    if (wcscmp(clsName, L"Progman") == 0 ||
        wcscmp(clsName, L"WorkerW") == 0 ||
        wcscmp(clsName, L"Shell_TrayWnd") == 0 ||
        wcscmp(clsName, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(clsName, L"Windows.UI.Core.CoreWindow") == 0) {
        return false;
    }

    return true;
}

struct EnumParam {
    HWND hDock;
    HWND hFg;
    std::vector<RunningWindowInfo>* pList;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    auto pParam = reinterpret_cast<EnumParam*>(lParam);
    if (TaskManager::IsTaskbarWindow(hwnd, pParam->hDock)) {
        std::wstring fullPath, exeFilename;
        if (TaskManager::GetWindowProcessInfo(hwnd, fullPath, exeFilename)) {
            wchar_t titleBuf[256] = {};
            GetWindowTextW(hwnd, titleBuf, 256);

            RunningWindowInfo win;
            win.hWnd = hwnd;
            win.title = titleBuf;
            win.fullExePath = fullPath;
            win.exeFilename = exeFilename;
            win.isForeground = (hwnd == pParam->hFg);
            pParam->pList->push_back(win);
        }
    }
    return TRUE;
}

std::vector<RunningWindowInfo> TaskManager::GetOpenWindows(HWND hDockWnd) {
    std::vector<RunningWindowInfo> windows;
    EnumParam param = { hDockWnd, GetForegroundWindow(), &windows };
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&param));
    return windows;
}

void TaskManager::ToggleWindowState(HWND hWnd, bool wasForegroundHint) {
    if (!IsWindow(hWnd)) return;

    HWND fg = GetForegroundWindow();
    bool isFg = (hWnd == fg || GetAncestor(fg, GA_ROOT) == hWnd || wasForegroundHint);

    // If it's already the foreground window and not minimized, minimize it!
    if (isFg && !IsIconic(hWnd)) {
        ShowWindow(hWnd, SW_MINIMIZE);
    } else {
        // Bring to front and restore if minimized
        if (IsIconic(hWnd)) {
            ShowWindow(hWnd, SW_RESTORE);
        } else {
            ShowWindow(hWnd, SW_SHOW);
        }

        // Attach thread input to bypass foreground lock restrictions
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
    }
}

std::wstring TaskManager::ResolveShortcutTarget(const std::wstring& lnkPath) {
    if (lnkPath.length() < 4 || lnkPath.substr(lnkPath.length() - 4) != L".lnk") {
        return lnkPath;
    }
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (SUCCEEDED(hr)) {
        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf)))) {
            if (SUCCEEDED(ppf->Load(lnkPath.c_str(), STGM_READ))) {
                wchar_t targetBuf[MAX_PATH] = {};
                if (SUCCEEDED(psl->GetPath(targetBuf, MAX_PATH, NULL, SLGP_UNCPRIORITY))) {
                    if (wcslen(targetBuf) > 0) {
                        ppf->Release();
                        psl->Release();
                        return targetBuf;
                    }
                }
            }
            ppf->Release();
        }
        psl->Release();
    }
    return lnkPath;
}

static std::wstring CleanShortcutName(const std::wstring& name) {
    std::wstring s = name;
    for (auto& ch : s) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'\"' || ch == L'<' || ch == L'>' || ch == L'|') {
            ch = L'_';
        }
    }
    return s;
}

bool TaskManager::PinApp(const std::wstring& targetPath, const std::wstring& displayName) {
    if (targetPath.empty()) return false;

    wchar_t appData[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    std::wstring folder = std::wstring(appData) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

    CreateDirectoryW(folder.c_str(), NULL);

    std::wstring cleanName = CleanShortcutName(displayName);
    if (cleanName.empty()) cleanName = L"App";
    std::wstring lnkPath = folder + L"\\" + cleanName + L".lnk";

    // If source is already a .lnk, copy it directly
    if (targetPath.length() > 4 && targetPath.substr(targetPath.length() - 4) == L".lnk") {
        if (CopyFileW(targetPath.c_str(), lnkPath.c_str(), FALSE)) {
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, folder.c_str(), NULL);
            return true;
        }
    }

    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (SUCCEEDED(hr)) {
        psl->SetPath(targetPath.c_str());
        size_t lastSlash = targetPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            psl->SetWorkingDirectory(targetPath.substr(0, lastSlash).c_str());
        }
        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_PPV_ARGS(&ppf)))) {
            hr = ppf->Save(lnkPath.c_str(), TRUE);
            ppf->Release();
            psl->Release();
            if (SUCCEEDED(hr)) {
                SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, folder.c_str(), NULL);
                return true;
            }
            return false;
        }
        psl->Release();
    }
    return false;
}

bool TaskManager::UnpinApp(const std::wstring& lnkOrExePath, const std::wstring& exeFilename) {
    wchar_t appData[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    std::wstring folder = std::wstring(appData) + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar";

    bool deleted = false;

    // 1. If lnkOrExePath is directly in the TaskBar folder, delete it
    if (lnkOrExePath.length() > 4 && lnkOrExePath.substr(lnkOrExePath.length() - 4) == L".lnk") {
        if (DeleteFileW(lnkOrExePath.c_str())) {
            deleted = true;
        }
    }

    // 2. Scan TaskBar folder for any .lnk whose target matches exeFilename or target path
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((folder + L"\\*.lnk").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring fullPath = folder + L"\\" + fd.cFileName;
            std::wstring target = ResolveShortcutTarget(fullPath);
            std::wstring targetExe = ToLower(GetFilenameFromPath(target));
            if (!exeFilename.empty() && targetExe == ToLower(exeFilename)) {
                if (DeleteFileW(fullPath.c_str())) {
                    deleted = true;
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    if (deleted) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH, folder.c_str(), NULL);
    }
    return deleted;
}

std::wstring TaskManager::GetAppFriendlyName(const std::wstring& fullExePath, const std::wstring& fallbackTitle) {
    if (!fullExePath.empty()) {
        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(fullExePath.c_str(), &handle);
        if (size > 0) {
            std::vector<BYTE> data(size);
            if (GetFileVersionInfoW(fullExePath.c_str(), handle, size, data.data())) {
                struct LANGANDCODEPAGE {
                    WORD wLanguage;
                    WORD wCodePage;
                } *lpTranslate = nullptr;
                UINT cbTranslate = 0;

                if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&lpTranslate, &cbTranslate) && cbTranslate >= sizeof(LANGANDCODEPAGE)) {
                    int numTranslations = (int)(cbTranslate / sizeof(LANGANDCODEPAGE));
                    for (int i = 0; i < numTranslations; ++i) {
                        wchar_t subBlock[128] = {};
                        swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\FileDescription", lpTranslate[i].wLanguage, lpTranslate[i].wCodePage);
                        wchar_t* lpBuffer = nullptr;
                        UINT dwBytes = 0;
                        if (VerQueryValueW(data.data(), subBlock, (LPVOID*)&lpBuffer, &dwBytes) && dwBytes > 0 && lpBuffer && wcslen(lpBuffer) > 0) {
                            std::wstring desc = lpBuffer;
                            while (!desc.empty() && (desc.back() == L' ' || desc.back() == L'\t')) desc.pop_back();
                            if (!desc.empty()) return desc;
                        }
                        swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\ProductName", lpTranslate[i].wLanguage, lpTranslate[i].wCodePage);
                        if (VerQueryValueW(data.data(), subBlock, (LPVOID*)&lpBuffer, &dwBytes) && dwBytes > 0 && lpBuffer && wcslen(lpBuffer) > 0) {
                            std::wstring desc = lpBuffer;
                            while (!desc.empty() && (desc.back() == L' ' || desc.back() == L'\t')) desc.pop_back();
                            if (!desc.empty()) return desc;
                        }
                    }
                }

                // Fallback translation blocks
                const wchar_t* commonBlocks[] = {
                    L"\\StringFileInfo\\040904b0\\FileDescription",
                    L"\\StringFileInfo\\040904b0\\ProductName",
                    L"\\StringFileInfo\\040904e4\\FileDescription",
                    L"\\StringFileInfo\\040904e4\\ProductName",
                    L"\\StringFileInfo\\000004b0\\FileDescription",
                    L"\\StringFileInfo\\000004b0\\ProductName"
                };
                for (const auto& block : commonBlocks) {
                    wchar_t* lpBuffer = nullptr;
                    UINT dwBytes = 0;
                    if (VerQueryValueW(data.data(), block, (LPVOID*)&lpBuffer, &dwBytes) && dwBytes > 0 && lpBuffer && wcslen(lpBuffer) > 0) {
                        std::wstring desc = lpBuffer;
                        while (!desc.empty() && (desc.back() == L' ' || desc.back() == L'\t')) desc.pop_back();
                        if (!desc.empty()) return desc;
                    }
                }
            }
        }
    }

    // Fallback: If title has a dash (e.g. "Doc - AppName"), extract the last part (the app name)
    if (!fallbackTitle.empty()) {
        size_t dash = fallbackTitle.find_last_of(L"-");
        if (dash != std::wstring::npos && dash + 1 < fallbackTitle.length()) {
            std::wstring afterDash = fallbackTitle.substr(dash + 1);
            size_t start = afterDash.find_first_not_of(L" \t");
            if (start != std::wstring::npos && afterDash.length() - start >= 2) {
                return afterDash.substr(start);
            }
        }
    }

    // Fallback to exe name without .exe capitalized
    std::wstring exeName = GetFilenameFromPath(fullExePath);
    if (exeName.length() > 4 && exeName.substr(exeName.length() - 4) == L".exe") {
        exeName = exeName.substr(0, exeName.length() - 4);
    }
    if (!exeName.empty()) {
        exeName[0] = towupper(exeName[0]);
        return exeName;
    }

    return fallbackTitle;
}


