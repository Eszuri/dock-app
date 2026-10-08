@echo off
echo ===================================================
echo Compiling LiteDock (Taskbar-Synchronized macOS Dock)
echo ===================================================

g++ -std=c++20 -O3 -mwindows -municode ^
    src/main.cpp ^
    src/DockWindow.cpp ^
    src/IconHelper.cpp ^
    src/TaskManager.cpp ^
    -o LiteDock.exe ^
    -ld2d1 -ldwrite -lwindowscodecs -ldwmapi -lole32 -lshell32 -lgdi32 -luxtheme -luuid

if %ERRORLEVEL% EQU 0 (
    echo [SUCCESS] LiteDock.exe compiled successfully! Size:
    dir LiteDock.exe | findstr LiteDock.exe
) else (
    echo [ERROR] Compilation failed.
)
pause
