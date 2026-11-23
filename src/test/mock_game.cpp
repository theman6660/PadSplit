#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>

static int g_killFocusCount = 0;
static int g_activateAppFalseCount = 0;

static LRESULT CALLBACK MockWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KILLFOCUS:
        g_killFocusCount++;
        return 0;
    case WM_ACTIVATEAPP:
        if (wParam == FALSE) g_activateAppFalseCount++;
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MockWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PadSplitMockGameClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"PadSplit Mock Game",
        WS_OVERLAPPEDWINDOW, 100, 100, 640, 480,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);

    // Register Raw Input: mouse (with RIDEV_NOLEGACY|RIDEV_CAPTUREMOUSE like DOOM!), keyboard, and gamepad
    RAWINPUTDEVICE rids[3]{};
    rids[0].usUsagePage = 0x01;
    rids[0].usUsage = 0x02; // Mouse
    rids[0].dwFlags = RIDEV_NOLEGACY | RIDEV_CAPTUREMOUSE;
    rids[0].hwndTarget = hwnd;
    rids[1].usUsagePage = 0x01;
    rids[1].usUsage = 0x06; // Keyboard
    rids[1].dwFlags = RIDEV_NOLEGACY | RIDEV_NOHOTKEYS;
    rids[1].hwndTarget = hwnd;
    rids[2].usUsagePage = 0x01;
    rids[2].usUsage = 0x05; // Gamepad
    rids[2].hwndTarget = hwnd;
    RegisterRawInputDevices(rids, 3, sizeof(RAWINPUTDEVICE));

    using FnXInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    FnXInputGetState pXInputGetState = nullptr;
    HMODULE xinputMod = LoadLibraryW(L"xinput1_4.dll");
    if (xinputMod) {
        pXInputGetState = reinterpret_cast<FnXInputGetState>(GetProcAddress(xinputMod, "XInputGetState"));
    }

    SetTimer(hwnd, 1, 150, nullptr);

    MSG msg{};
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            if (msg.message == WM_TIMER && msg.wParam == 1) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                ClipCursor(&rc);
                SetCursorPos((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
                SetCapture(hwnd);
                GetAsyncKeyState(VK_SPACE);

                if (pXInputGetState) {
                    XINPUT_STATE xs{};
                    pXInputGetState(0, &xs);
                }

                DWORD mouseRawFlags = 0;
                UINT ridCount = 0;
                if (GetRegisteredRawInputDevices(nullptr, &ridCount, sizeof(RAWINPUTDEVICE)) != static_cast<UINT>(-1) && ridCount > 0 && ridCount <= 16) {
                    RAWINPUTDEVICE cur[16]{};
                    if (GetRegisteredRawInputDevices(cur, &ridCount, sizeof(RAWINPUTDEVICE)) != static_cast<UINT>(-1)) {
                        for (UINT i = 0; i < ridCount; ++i) {
                            if (cur[i].usUsagePage == 0x01 && cur[i].usUsage == 0x02) {
                                mouseRawFlags = cur[i].dwFlags;
                            }
                        }
                    }
                }

                HWND fg = GetForegroundWindow();
                bool fgIsSelf = (fg == hwnd);
                bool hasCapture = (GetCapture() == hwnd);

                std::printf(
                    "{\"pid\":%lu,\"arch\":%u,\"fg_is_self\":%s,\"has_capture\":%s,\"mouse_raw_flags\":%lu,\"killfocus_count\":%d,\"activateapp_false_count\":%d,\"window_w\":%ld,\"window_h\":%ld}\n",
                    static_cast<unsigned long>(GetCurrentProcessId()),
                    static_cast<unsigned>(sizeof(void*) * 8),
                    fgIsSelf ? "true" : "false",
                    hasCapture ? "true" : "false",
                    static_cast<unsigned long>(mouseRawFlags),
                    g_killFocusCount,
                    g_activateAppFalseCount,
                    rc.right - rc.left,
                    rc.bottom - rc.top);
                std::fflush(stdout);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }

    ClipCursor(nullptr);
    if (xinputMod) FreeLibrary(xinputMod);
    return 0;
}
