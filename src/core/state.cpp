#include "state.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace padsplit::core {

HMODULE g_hModule = nullptr;
SharedState* g_shm = nullptr;
HANDLE g_shmHandle = nullptr;
bool g_shmOwnedByCore = false;
HWND g_gameHwnd = nullptr;
DWORD g_gameThreadId = 0;
RECT g_gameMonRect = {0, 0, 1920, 1080};
volatile LONG g_detaching = 0;

FnGetForegroundWindow g_origGetForegroundWindow = nullptr;
FnGetActiveWindow g_origGetActiveWindow = nullptr;
FnGetFocus g_origGetFocus = nullptr;
FnClipCursor g_origClipCursor = nullptr;
FnSetCursorPos g_origSetCursorPos = nullptr;
FnShowCursor g_origShowCursor = nullptr;
FnSetCursor g_origSetCursor = nullptr;
FnGetRawInputData g_origGetRawInputData = nullptr;
FnRegisterRawInputDevices g_origRegisterRawInputDevices = nullptr;

static bool g_steamLogged = false;

bool OpenSharedState() {
    wchar_t shmName[64]{};
    DWORD pid = GetCurrentProcessId();
    MakeShmName(pid, shmName, sizeof(shmName) / sizeof(shmName[0]));

    g_shmHandle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (!g_shmHandle) {
        g_shmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedState), shmName);
        if (!g_shmHandle) return false;
        g_shmOwnedByCore = true;
    }

    g_shm = static_cast<SharedState*>(MapViewOfFile(g_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!g_shm) {
        CloseHandle(g_shmHandle);
        g_shmHandle = nullptr;
        return false;
    }

    if (g_shmOwnedByCore || g_shm->magic != PADSPLIT_SHM_MAGIC) {
        InitSharedStateDefaults(g_shm, pid);
    }
    g_shm->core_arch = static_cast<uint32_t>(sizeof(void*) * 8);
    return true;
}

void CloseSharedState() {
    if (g_shm) {
        UnmapViewOfFile(g_shm);
        g_shm = nullptr;
    }
    if (g_shmHandle) {
        CloseHandle(g_shmHandle);
        g_shmHandle = nullptr;
    }
}

void CoreLog(const char* fmt, ...) {
    if (!g_shm || !fmt) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);

    char msg[PADSPLIT_LOG_LINE_LEN]{};
    int prefixLen = _snprintf(msg, sizeof(msg), "[%02u:%02u:%02u.%03u] ",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (prefixLen < 0) prefixLen = 0;

    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(msg + prefixLen, sizeof(msg) - static_cast<size_t>(prefixLen) - 1, fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    shm_append_log(g_shm, msg);
}

struct FindWinCtx {
    DWORD pid;
    HWND bestHwnd;
    int64_t bestArea;
};

static BOOL CALLBACK FindGameWinProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    auto* ctx = reinterpret_cast<FindWinCtx*>(lParam);
    if (pid != ctx->pid) return TRUE;

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    int64_t w = rc.right - rc.left;
    int64_t h = rc.bottom - rc.top;
    if (w < 100 || h < 60) return TRUE;

    int64_t area = w * h;
    if (area > ctx->bestArea) {
        ctx->bestArea = area;
        ctx->bestHwnd = hwnd;
    }
    return TRUE;
}

void UpdateGameMonitorRect() {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    HMONITOR hMon = MonitorFromWindow(g_gameHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        g_gameMonRect = mi.rcMonitor;
        if (g_shm) {
            g_shm->game_mon_left = mi.rcMonitor.left;
            g_shm->game_mon_top = mi.rcMonitor.top;
            g_shm->game_mon_right = mi.rcMonitor.right;
            g_shm->game_mon_bottom = mi.rcMonitor.bottom;
        }
    }
}

bool DiscoverGameWindow() {
    if (g_gameHwnd && IsWindow(g_gameHwnd)) {
        UpdateGameMonitorRect();
        return false;
    }

    FindWinCtx ctx{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(FindGameWinProc, reinterpret_cast<LPARAM>(&ctx));
    if (!ctx.bestHwnd) return false;

    g_gameHwnd = ctx.bestHwnd;
    g_gameThreadId = GetWindowThreadProcessId(g_gameHwnd, nullptr);
    if (g_shm) {
        g_shm->game_hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_gameHwnd));
    }
    UpdateGameMonitorRect();
    CoreLog("Discovered game HWND=0x%llX tid=%lu mon=(%ld,%ld,%ld,%ld)",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_gameHwnd)),
            static_cast<unsigned long>(g_gameThreadId),
            g_gameMonRect.left, g_gameMonRect.top, g_gameMonRect.right, g_gameMonRect.bottom);
    return true;
}

void DetectSteamOverlay() {
    if (g_steamLogged || !g_shm) return;
    if (GetModuleHandleW(L"gameoverlayrenderer64.dll") ||
        GetModuleHandleW(L"gameoverlayrenderer.dll") ||
        GetModuleHandleW(L"SteamOverlayVulkanLayer64.dll")) {
        shm_store32(&g_shm->detected_steam_overlay, 1);
        g_steamLogged = true;
        CoreLog("Steam Overlay detected. Tip: If gamepad switches to desktop mode on alt-monitor, disable Steam Input for this game or clear Steam Desktop Config.");
    }
}

bool IsCursorOutsideGameMonitor() {
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    return PtInRect(&g_gameMonRect, pt) == FALSE;
}

bool ShouldSpoofFocus() {
    if (!g_shm || g_detaching) return false;
    if (!shm_load32(&g_shm->enabled) || !shm_load32(&g_shm->spoof_focus)) return false;
    if (!g_gameHwnd || !IsWindow(g_gameHwnd) || IsIconic(g_gameHwnd)) return false;
    return true;
}

bool ShouldBlockKBM() {
    if (!g_shm || g_detaching) return false;
    if (!shm_load32(&g_shm->enabled)) return false;

    uint32_t mode = shm_load32(&g_shm->isolation_mode);
    if (mode == ISO_ALWAYS_BLOCK_KBM) {
        return true;
    }

    // ISO_BLOCK_OUTSIDE_GAME: block KBM if cursor is outside game monitor OR game is not real foreground
    HWND fg = g_origGetForegroundWindow ? g_origGetForegroundWindow() : GetForegroundWindow();
    if (fg != g_gameHwnd) return true;
    return IsCursorOutsideGameMonitor();
}

} // namespace padsplit::core
