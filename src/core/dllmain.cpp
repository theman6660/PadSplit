#include "state.h"
#include "hooks_focus.h"
#include "hooks_cursor.h"
#include "hooks_input.h"
#include "hooks_display.h"
#include "hooks_audio.h"

#include <MinHook.h>

namespace padsplit::core {

static DWORD WINAPI CoreMainThread(LPVOID lpParam) {
    g_hModule = static_cast<HMODULE>(lpParam);

    if (!OpenSharedState()) {
        FreeLibraryAndExitThread(g_hModule, 1);
        return 1;
    }

    CoreLog("PadSplit Core (%u-bit) initializing in PID=%lu",
            static_cast<unsigned>(sizeof(void*) * 8),
            static_cast<unsigned long>(GetCurrentProcessId()));

    if (MH_Initialize() != MH_OK) {
        CoreLog("ERROR: MH_Initialize failed");
        CloseSharedState();
        FreeLibraryAndExitThread(g_hModule, 1);
        return 1;
    }

    InstallFocusHooks();
    InstallCursorHooks();
    InstallInputHooks();
    MH_EnableHook(MH_ALL_HOOKS);

    if (DiscoverGameWindow() && g_gameHwnd) {
        AttachWndProc(g_gameHwnd);
    }
    PatchExistingRawInputDevices();
    DetectSteamOverlay();
    TryHookLazyInputModules();
    TryHookLazyDisplayModules();
    TryHookLazyAudioModules();

    uint32_t prevEnabled = shm_load32(&g_shm->enabled);
    uint32_t lastBorderlessSeq = shm_load32(&g_shm->apply_borderless_seq);

    if (prevEnabled) {
        if (shm_load32(&g_shm->unlock_cursor)) {
            ReleaseCursorClipNow();
            if (g_gameHwnd) PostMessageW(g_gameHwnd, WM_NULL, 0, 0);
        }
        if (shm_load32(&g_shm->borderless_mode) != BORDERLESS_OFF && g_gameHwnd) {
            ApplyWindowBorderless();
        }
    }

    shm_store32(&g_shm->core_ready, 1);
    CoreLog("PadSplit Core ready");

    ULONGLONG lastLazyScan = GetTickCount64();

    while (shm_load32(&g_shm->request_detach) == 0) {
        if (DiscoverGameWindow() && g_gameHwnd) {
            AttachWndProc(g_gameHwnd);
            PatchExistingRawInputDevices();
            if (shm_load32(&g_shm->enabled) && shm_load32(&g_shm->borderless_mode) != BORDERLESS_OFF) {
                ApplyWindowBorderless();
            }
        } else if (g_gameHwnd) {
            EnsureWndProcAttached(g_gameHwnd);
        }

        HWND realFg = g_origGetForegroundWindow ? g_origGetForegroundWindow() : GetForegroundWindow();
        shm_store32(&g_shm->real_is_foreground, (g_gameHwnd && realFg == g_gameHwnd) ? 1u : 0u);
        shm_store32(&g_shm->kbm_currently_blocked, ShouldBlockKBM() ? 1u : 0u);

        uint32_t curEnabled = shm_load32(&g_shm->enabled);
        if (curEnabled && !prevEnabled) {
            if (shm_load32(&g_shm->unlock_cursor)) {
                ReleaseCursorClipNow();
                PatchExistingRawInputDevices();
                if (g_gameHwnd) PostMessageW(g_gameHwnd, WM_NULL, 0, 0);
            }
        } else if (!curEnabled && prevEnabled) {
            RestoreOriginalRawInputDevices();
        }
        prevEnabled = curEnabled;

        uint32_t curSeq = shm_load32(&g_shm->apply_borderless_seq);
        if (curSeq != lastBorderlessSeq) {
            lastBorderlessSeq = curSeq;
            ApplyWindowBorderless();
        }

        UpdateIdleCursorTick();

        ULONGLONG now = GetTickCount64();
        if (now - lastLazyScan >= 1000) {
            lastLazyScan = now;
            DetectSteamOverlay();
            TryHookLazyInputModules();
            TryHookLazyDisplayModules();
            TryHookLazyAudioModules();
        }

        Sleep(100);
    }

    CoreLog("Detach requested, cleaning up...");
    InterlockedExchange(&g_detaching, 1);
    shm_store32(&g_shm->core_ready, 2);
    shm_store32(&g_shm->cursor_idle_hidden, 0);
    shm_store32(&g_shm->kbm_currently_blocked, 0);

    DetachWndProc();
    RestoreWindowBorderless();
    RestoreOriginalRawInputDevices();
    ReleaseCursorClipNow();

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Sleep(80);
    ShutdownInputHooks();

    CloseSharedState();
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

} // namespace padsplit::core

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, padsplit::core::CoreMainThread, hModule, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
