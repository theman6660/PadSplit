#include "hooks_cursor.h"
#include "state.h"

#include <MinHook.h>
#include <cstdlib>

namespace padsplit::core {

static volatile LONG g_virtualShowCount = 0;
static POINT g_lastCursorPt = {0, 0};
static bool g_hasLastCursorPt = false;
static ULONGLONG g_lastMoveTick = 0;
static bool g_loggedSetCapture = false;

using FnSetCapture = HWND(WINAPI*)(HWND);
static FnSetCapture g_origSetCapture = nullptr;

static HWND WINAPI Hooked_SetCapture(HWND hWnd) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->unlock_cursor)) {
        shm_inc64(&g_shm->blocked_clip_cursor);
        if (!g_loggedSetCapture) {
            g_loggedSetCapture = true;
            CoreLog("Blocked SetCapture(0x%llX) to keep Monitor 2 clickable",
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hWnd)));
        }
        return g_gameHwnd;
    }
    return g_origSetCapture(hWnd);
}

static BOOL WINAPI Hooked_ClipCursor(const RECT* lpRect) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->unlock_cursor)) {
        if (lpRect != nullptr) {
            shm_inc64(&g_shm->blocked_clip_cursor);
            return TRUE;
        }
    }
    return g_origClipCursor(lpRect);
}

static BOOL WINAPI Hooked_SetCursorPos(int X, int Y) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled)) {
        if (shm_load32(&g_shm->unlock_cursor) || ShouldBlockKBM()) {
            shm_inc64(&g_shm->blocked_set_cursor_pos);
            return TRUE;
        }
    }
    return g_origSetCursorPos(X, Y);
}

static int WINAPI Hooked_ShowCursor(BOOL bShow) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->unlock_cursor)) {
        if (bShow) {
            return static_cast<int>(InterlockedIncrement(&g_virtualShowCount));
        } else {
            // Prevent game from hiding OS cursor when moving to secondary monitor
            return static_cast<int>(InterlockedDecrement(&g_virtualShowCount));
        }
    }
    return g_origShowCursor(bShow);
}

static HCURSOR WINAPI Hooked_SetCursor(HCURSOR hCursor) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->unlock_cursor)) {
        HWND realFg = g_origGetForegroundWindow ? g_origGetForegroundWindow() : GetForegroundWindow();
        if (IsCursorOutsideGameMonitor() || realFg != g_gameHwnd || ShouldBlockKBM()) {
            // Prevent game from overriding OS cursor with custom in-game cursor or hiding it
            return GetCursor();
        }
    }
    return g_origSetCursor(hCursor);
}

bool InstallCursorHooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;

    void* pClip = reinterpret_cast<void*>(GetProcAddress(user32, "ClipCursor"));
    void* pSetPos = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
    void* pShow = reinterpret_cast<void*>(GetProcAddress(user32, "ShowCursor"));
    void* pSetCur = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursor"));
    void* pSetCap = reinterpret_cast<void*>(GetProcAddress(user32, "SetCapture"));

    if (pClip) MH_CreateHook(pClip, reinterpret_cast<void*>(&Hooked_ClipCursor), reinterpret_cast<void**>(&g_origClipCursor));
    if (pSetPos) MH_CreateHook(pSetPos, reinterpret_cast<void*>(&Hooked_SetCursorPos), reinterpret_cast<void**>(&g_origSetCursorPos));
    if (pShow) MH_CreateHook(pShow, reinterpret_cast<void*>(&Hooked_ShowCursor), reinterpret_cast<void**>(&g_origShowCursor));
    if (pSetCur) MH_CreateHook(pSetCur, reinterpret_cast<void*>(&Hooked_SetCursor), reinterpret_cast<void**>(&g_origSetCursor));
    if (pSetCap) MH_CreateHook(pSetCap, reinterpret_cast<void*>(&Hooked_SetCapture), reinterpret_cast<void**>(&g_origSetCapture));

    g_lastMoveTick = GetTickCount64();
    CoreLog("Cursor & SetCapture hooks created");
    return true;
}

void ReleaseCursorClipNow() {
    if (g_origClipCursor) {
        g_origClipCursor(nullptr);
    } else {
        ClipCursor(nullptr);
    }
}

void UpdateIdleCursorTick() {
    if (!g_shm || g_detaching) return;

    POINT pt{};
    if (!GetCursorPos(&pt)) return;
    ULONGLONG now = GetTickCount64();

    if (!g_hasLastCursorPt) {
        g_lastCursorPt = pt;
        g_hasLastCursorPt = true;
        g_lastMoveTick = now;
        return;
    }

    int dx = std::abs(pt.x - g_lastCursorPt.x);
    int dy = std::abs(pt.y - g_lastCursorPt.y);
    if (dx >= 2 || dy >= 2) {
        g_lastCursorPt = pt;
        g_lastMoveTick = now;
        if (shm_load32(&g_shm->cursor_idle_hidden) != 0) {
            shm_store32(&g_shm->cursor_idle_hidden, 0);
            if (g_gameHwnd && IsWindow(g_gameHwnd)) {
                PostMessageW(g_gameHwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(g_gameHwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
        }
        return;
    }

    if (!PtInRect(&g_gameMonRect, pt) || !shm_load32(&g_shm->enabled) || !shm_load32(&g_shm->hide_idle_cursor)) {
        if (shm_load32(&g_shm->cursor_idle_hidden) != 0) {
            shm_store32(&g_shm->cursor_idle_hidden, 0);
            if (g_gameHwnd && IsWindow(g_gameHwnd)) {
                PostMessageW(g_gameHwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(g_gameHwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
        }
        return;
    }

    uint32_t idleMs = shm_load32(&g_shm->idle_cursor_ms);
    if (idleMs < 200) idleMs = 200;

    if (now - g_lastMoveTick >= idleMs && shm_load32(&g_shm->cursor_idle_hidden) == 0) {
        shm_store32(&g_shm->cursor_idle_hidden, 1);
        if (g_origSetCursorPos) {
            g_origSetCursorPos(pt.x, pt.y);
        }
    }
}

} // namespace padsplit::core
