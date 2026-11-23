#include "ui_panel.h"
#include "../shared/protocol.h"
#include "../shared/win_util.h"

#include <cstring>

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    padsplit::EnablePerMonitorDpiAwareness();

    bool startMinimized = (lpCmdLine && wcsstr(lpCmdLine, L"--minimized") != nullptr);
    bool forceShow = (lpCmdLine && wcsstr(lpCmdLine, L"--show") != nullptr);

    HANDLE hMutex = CreateMutexW(nullptr, FALSE, padsplit::PADSPLIT_SINGLE_INSTANCE_MUTEX);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startMinimized) {
            HWND existing = FindWindowExW(HWND_MESSAGE, nullptr, L"PadSplitTrayMsgWindow", L"PadSplitTray");
            if (existing) {
                PostMessageW(existing, WM_APP + 103, 0, 0);
            }
        }
        CloseHandle(hMutex);
        return 0;
    }

    int rc = 0;
    {
        padsplit::app::UiApp app;
        rc = app.Run(hInstance, startMinimized, forceShow);
    }

    if (hMutex) CloseHandle(hMutex);
    return rc;
}
