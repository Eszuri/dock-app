#include <windows.h>
#include "DockWindow.h"
#include "IconHelper.h"

int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    PWSTR pCmdLine,
    int nCmdShow
) {
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
        return 1;
    }

    IconHelper::Initialize();

    {
        DockWindow dock;
        if (dock.Initialize(hInstance)) {
            dock.Run();
        }
    }

    IconHelper::Shutdown();
    CoUninitialize();

    return 0;
}
