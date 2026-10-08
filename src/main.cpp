#include <windows.h>
#include "DockWindow.h"
#include "IconHelper.h"
#include "Logger.h"

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    PWSTR pCmdLine,
    int nCmdShow
) {
    Log("=== LiteDock Starting ===");

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Log("CoInitializeEx FAILED: " + std::to_string(hr));
        return 1;
    }
    Log("CoInitializeEx OK");

    IconHelper::Initialize();
    Log("IconHelper::Initialize OK");

    {
        DockWindow dock;
        if (dock.Initialize(hInstance)) {
            Log("dock.Initialize SUCCESS -> entering Run()");
            dock.Run();
            Log("dock.Run() finished (GetMessage returned 0 or -1)");
        } else {
            Log("dock.Initialize FAILED!");
        }
    }

    IconHelper::Shutdown();
    CoUninitialize();

    Log("=== LiteDock Exiting Cleanly ===");
    return 0;
}
