#include "hooks_focus.h"
#include "state.h"

#include <MinHook.h>

namespace padsplit::core {

static WNDPROC g_origWndProc = nullptr;
static HWND g_subclassedHwnd = nullptr;
static bool g_subclassedUnicode = true;
static bool g_cursorShowRestored = false;

static LRESULT CallOrigWndProc(WNDPROC orig, HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (orig) {
        return g_subclassedUnicode
            ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
            : CallWindowProcA(orig, hwnd, msg, wParam, lParam);
    }
    return g_subclassedUnicode
        ? DefWindowProcW(hwnd, msg, wParam, lParam)
        : DefWindowProcA(hwnd, msg, wParam, lParam);
}

using FnPeekMessageW = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using FnPeekMessageA = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
static FnPeekMessageW g_origPeekMessageW = nullptr;
static FnPeekMessageA g_origPeekMessageA = nullptr;

static HWND WINAPI Hooked_GetForegroundWindow() {
    if (ShouldSpoofFocus()) {
        shm_inc64(&g_shm->focus_api_spoofs);
        return g_gameHwnd;
    }
    return g_origGetForegroundWindow();
}

static HWND WINAPI Hooked_GetActiveWindow() {
    if (ShouldSpoofFocus() && GetCurrentThreadId() == g_gameThreadId) {
        shm_inc64(&g_shm->focus_api_spoofs);
        return g_gameHwnd;
    }
    return g_origGetActiveWindow();
}

static HWND WINAPI Hooked_GetFocus() {
    if (ShouldSpoofFocus() && GetCurrentThreadId() == g_gameThreadId) {
        HWND realFocus = g_origGetFocus();
        if (!realFocus) {
            shm_inc64(&g_shm->focus_api_spoofs);
            return g_gameHwnd;
        }
        return realFocus;
    }
    return g_origGetFocus();
}

static bool IsKbmWindowMessage(UINT msg) {
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_DEADCHAR:
        return true;
    default:
        return false;
    }
}

static void FilterPeekedMessage(LPMSG lpMsg, UINT wRemoveMsg) {
    if (!(wRemoveMsg & PM_REMOVE) || !lpMsg || !g_gameHwnd) return;
    if (lpMsg->hwnd != g_gameHwnd && !IsChild(g_gameHwnd, lpMsg->hwnd)) return;

    if (ShouldSpoofFocus()) {
        UINT m = lpMsg->message;
        if (m == WM_KILLFOCUS ||
            (m == WM_ACTIVATEAPP && lpMsg->wParam == FALSE) ||
            (m == WM_ACTIVATE && LOWORD(lpMsg->wParam) == WA_INACTIVE) ||
            (m == WM_NCACTIVATE && lpMsg->wParam == FALSE)) {
            lpMsg->message = WM_NULL;
            shm_inc64(&g_shm->blocked_focus_msgs);
            return;
        }
    }

    if (ShouldBlockKBM()) {
        if (IsKbmWindowMessage(lpMsg->message)) {
            lpMsg->message = WM_NULL;
            shm_inc64(&g_shm->blocked_kbm_polls);
            return;
        }
        if (lpMsg->message == WM_INPUT) {
            RAWINPUTHEADER hdr{};
            UINT sz = sizeof(hdr);
            auto fnGetRaw = g_origGetRawInputData ? g_origGetRawInputData : GetRawInputData;
            if (fnGetRaw(reinterpret_cast<HRAWINPUT>(lpMsg->lParam), RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                if (hdr.dwType == RIM_TYPEMOUSE || hdr.dwType == RIM_TYPEKEYBOARD) {
                    lpMsg->message = WM_NULL;
                    shm_inc64(&g_shm->blocked_raw_kbm);
                }
            }
        }
    }
}

static BOOL WINAPI Hooked_PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL ret = g_origPeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (ret) FilterPeekedMessage(lpMsg, wRemoveMsg);
    return ret;
}

static BOOL WINAPI Hooked_PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg) {
    BOOL ret = g_origPeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
    if (ret) FilterPeekedMessage(lpMsg, wRemoveMsg);
    return ret;
}

static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCDESTROY) {
        WNDPROC orig = g_origWndProc;
        bool isUni = g_subclassedUnicode;
        DetachWndProc();
        if (orig) {
            return isUni ? CallWindowProcW(orig, hwnd, msg, wParam, lParam)
                         : CallWindowProcA(orig, hwnd, msg, wParam, lParam);
        }
        return isUni ? DefWindowProcW(hwnd, msg, wParam, lParam)
                     : DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    if (g_shm && !g_detaching) {
        bool activeUnlock = (shm_load32(&g_shm->enabled) != 0 && shm_load32(&g_shm->unlock_cursor) != 0);
        if (activeUnlock) {
            if (GetCapture() == hwnd) {
                ReleaseCapture();
                CoreLog("Released pre-existing mouse capture on HWND=0x%llX",
                        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
            }
            if (!g_cursorShowRestored) {
                g_cursorShowRestored = true;
                auto fnShow = g_origShowCursor ? g_origShowCursor : ShowCursor;
                for (int i = 0; i < 16; ++i) {
                    if (fnShow(TRUE) >= 0) break;
                }
            }
        } else {
            g_cursorShowRestored = false;
        }
    }

    if (ShouldSpoofFocus()) {
        if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) {
            shm_inc64(&g_shm->blocked_focus_msgs);
            return 0;
        }
        if (msg == WM_ACTIVATEAPP && wParam == FALSE) {
            shm_inc64(&g_shm->blocked_focus_msgs);
            return 0;
        }
        if (msg == WM_NCACTIVATE && wParam == FALSE) {
            shm_inc64(&g_shm->blocked_focus_msgs);
            return TRUE;
        }
        if (msg == WM_KILLFOCUS) {
            shm_inc64(&g_shm->blocked_focus_msgs);
            return 0;
        }
    }

    if (ShouldBlockKBM()) {
        if (IsKbmWindowMessage(msg)) {
            shm_inc64(&g_shm->blocked_kbm_polls);
            return 0;
        }
        if (msg == WM_INPUT) {
            RAWINPUTHEADER hdr{};
            UINT sz = sizeof(hdr);
            auto fnGetRaw = g_origGetRawInputData ? g_origGetRawInputData : GetRawInputData;
            if (fnGetRaw(reinterpret_cast<HRAWINPUT>(lParam), RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                if (hdr.dwType == RIM_TYPEMOUSE || hdr.dwType == RIM_TYPEKEYBOARD) {
                    shm_inc64(&g_shm->blocked_raw_kbm);
                    return 0;
                }
            }
        }
    }

    if (msg == WM_SETCURSOR && g_shm && !g_detaching && shm_load32(&g_shm->enabled)) {
        auto fnSetCur = g_origSetCursor ? g_origSetCursor : SetCursor;
        POINT pt{};
        if (GetCursorPos(&pt)) {
            if (PtInRect(&g_gameMonRect, pt)) {
                if (shm_load32(&g_shm->hide_idle_cursor) && shm_load32(&g_shm->cursor_idle_hidden)) {
                    fnSetCur(nullptr);
                    return TRUE;
                }
                if (ShouldBlockKBM()) {
                    fnSetCur(LoadCursorW(nullptr, IDC_ARROW));
                    return TRUE;
                }
            } else if (shm_load32(&g_shm->unlock_cursor)) {
                fnSetCur(LoadCursorW(nullptr, IDC_ARROW));
                return TRUE;
            }
        }
    }

    return CallOrigWndProc(g_origWndProc, hwnd, msg, wParam, lParam);
}

bool InstallFocusHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;

    void* pFg = reinterpret_cast<void*>(GetProcAddress(user32, "GetForegroundWindow"));
    void* pAct = reinterpret_cast<void*>(GetProcAddress(user32, "GetActiveWindow"));
    void* pFoc = reinterpret_cast<void*>(GetProcAddress(user32, "GetFocus"));
    void* pPeekW = reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageW"));
    void* pPeekA = reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageA"));

    if (pFg) MH_CreateHook(pFg, reinterpret_cast<void*>(&Hooked_GetForegroundWindow), reinterpret_cast<void**>(&g_origGetForegroundWindow));
    if (pAct) MH_CreateHook(pAct, reinterpret_cast<void*>(&Hooked_GetActiveWindow), reinterpret_cast<void**>(&g_origGetActiveWindow));
    if (pFoc) MH_CreateHook(pFoc, reinterpret_cast<void*>(&Hooked_GetFocus), reinterpret_cast<void**>(&g_origGetFocus));
    if (pPeekW) MH_CreateHook(pPeekW, reinterpret_cast<void*>(&Hooked_PeekMessageW), reinterpret_cast<void**>(&g_origPeekMessageW));
    if (pPeekA) MH_CreateHook(pPeekA, reinterpret_cast<void*>(&Hooked_PeekMessageA), reinterpret_cast<void**>(&g_origPeekMessageA));

    CoreLog("Focus & PeekMessage hooks created");
    return true;
}

void AttachWndProc(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (g_subclassedHwnd == hwnd) return;
    if (g_subclassedHwnd && g_subclassedHwnd != hwnd) {
        DetachWndProc();
    }

    bool isUni = IsWindowUnicode(hwnd) != FALSE;
    WNDPROC prev = reinterpret_cast<WNDPROC>(
        isUni ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc))
              : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
    if (prev) {
        g_origWndProc = prev;
        g_subclassedHwnd = hwnd;
        g_subclassedUnicode = isUni;
        CoreLog("Subclassed game WndProc (%s) on HWND=0x%llX",
                isUni ? "Unicode" : "ANSI",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
    }
}

void EnsureWndProcAttached(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || g_detaching) return;
    bool isUni = IsWindowUnicode(hwnd) != FALSE;
    LONG_PTR cur = isUni ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (cur != reinterpret_cast<LONG_PTR>(HookedWndProc)) {
        WNDPROC prev = reinterpret_cast<WNDPROC>(
            isUni ? SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc))
                  : SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        if (prev) {
            g_origWndProc = prev;
            g_subclassedHwnd = hwnd;
            g_subclassedUnicode = isUni;
            CoreLog("Re-attached self-healed WndProc on HWND=0x%llX", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
        }
    }
}

void DetachWndProc() {
    if (g_subclassedHwnd && IsWindow(g_subclassedHwnd) && g_origWndProc) {
        LONG_PTR cur = g_subclassedUnicode
            ? GetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC)
            : GetWindowLongPtrA(g_subclassedHwnd, GWLP_WNDPROC);
        if (cur == reinterpret_cast<LONG_PTR>(HookedWndProc)) {
            if (g_subclassedUnicode) {
                SetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
            } else {
                SetWindowLongPtrA(g_subclassedHwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
            }
            CoreLog("Restored game WndProc");
        }
    }
    g_subclassedHwnd = nullptr;
    g_origWndProc = nullptr;
    g_subclassedUnicode = true;
    g_cursorShowRestored = false;
}

} // namespace padsplit::core
