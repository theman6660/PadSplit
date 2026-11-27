#include "hooks_input.h"
#include "state.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>

#include <cstring>
#include <vector>

namespace padsplit::core {

// ---- Win32 KBM Polling Hooks ----
using FnGetAsyncKeyState = SHORT(WINAPI*)(int);
using FnGetKeyState = SHORT(WINAPI*)(int);
using FnGetKeyboardState = BOOL(WINAPI*)(PBYTE);

static FnGetAsyncKeyState g_origGetAsyncKeyState = nullptr;
static FnGetKeyState g_origGetKeyState = nullptr;
static FnGetKeyboardState g_origGetKeyboardState = nullptr;

static SHORT WINAPI Hooked_GetAsyncKeyState(int vKey) {
    if (ShouldBlockKBM()) {
        shm_inc64(&g_shm->blocked_kbm_polls);
        return 0;
    }
    return g_origGetAsyncKeyState(vKey);
}

static SHORT WINAPI Hooked_GetKeyState(int nVirtKey) {
    if (ShouldBlockKBM()) {
        shm_inc64(&g_shm->blocked_kbm_polls);
        return 0;
    }
    return g_origGetKeyState(nVirtKey);
}

static BOOL WINAPI Hooked_GetKeyboardState(PBYTE lpKeyState) {
    if (ShouldBlockKBM()) {
        shm_inc64(&g_shm->blocked_kbm_polls);
        if (lpKeyState) std::memset(lpKeyState, 0, 256);
        return TRUE;
    }
    return g_origGetKeyboardState(lpKeyState);
}

// ---- Raw Input Hooks ----
constexpr DWORD kMouseBadRawFlags = RIDEV_CAPTUREMOUSE | RIDEV_NOLEGACY | RIDEV_INPUTSINK;
constexpr DWORD kKbdBadRawFlags = RIDEV_NOLEGACY | RIDEV_NOHOTKEYS | RIDEV_APPKEYS | RIDEV_INPUTSINK;

static RAWINPUTDEVICE g_savedOrigMouseRaw{};
static bool g_hasSavedMouseRaw = false;
static RAWINPUTDEVICE g_savedOrigKbdRaw{};
static bool g_hasSavedKbdRaw = false;
static bool g_loggedMouseRawStrip = false;
static bool g_loggedKbdRawStrip = false;
static CRITICAL_SECTION g_rawSaveCs;
static bool g_rawSaveCsInit = false;

static bool IsRawGamepadUsage(USHORT page, USHORT usage) {
    return page == 0x01 && (usage == 0x04 || usage == 0x05 || usage == 0x08);
}

static bool IsRawMouseUsage(USHORT page, USHORT usage) {
    return page == 0x01 && usage == 0x02;
}

static bool IsRawKbdUsage(USHORT page, USHORT usage) {
    return page == 0x01 && usage == 0x06;
}

static BOOL WINAPI Hooked_RegisterRawInputDevices(PCRAWINPUTDEVICE pRawInputDevices, UINT uiNumDevices, UINT cbSize) {
    if (!pRawInputDevices || uiNumDevices == 0 || cbSize != sizeof(RAWINPUTDEVICE) || !g_shm || g_detaching) {
        return g_origRegisterRawInputDevices(pRawInputDevices, uiNumDevices, cbSize);
    }

    bool activeUnlock = (shm_load32(&g_shm->enabled) != 0 && shm_load32(&g_shm->unlock_cursor) != 0);
    std::vector<RAWINPUTDEVICE> patched(pRawInputDevices, pRawInputDevices + uiNumDevices);
    for (auto& d : patched) {
        if (d.dwFlags & RIDEV_REMOVE) continue;
        if (IsRawGamepadUsage(d.usUsagePage, d.usUsage)) {
            shm_store32(&g_shm->detected_rawinput_pad, 1);
            if (!(d.dwFlags & RIDEV_INPUTSINK)) {
                d.dwFlags |= RIDEV_INPUTSINK;
                if (!d.hwndTarget && g_gameHwnd) {
                    d.hwndTarget = g_gameHwnd;
                }
                shm_inc64(&g_shm->rawinput_pad_inputsink_added);
            }
        } else if (IsRawMouseUsage(d.usUsagePage, d.usUsage)) {
            shm_store32(&g_shm->detected_rawinput_kbm, 1);
            if (g_rawSaveCsInit) {
                EnterCriticalSection(&g_rawSaveCs);
                g_savedOrigMouseRaw = d;
                g_hasSavedMouseRaw = true;
                LeaveCriticalSection(&g_rawSaveCs);
            }
            if (activeUnlock && (d.dwFlags & kMouseBadRawFlags) != 0) {
                DWORD oldFlags = d.dwFlags;
                d.dwFlags &= ~kMouseBadRawFlags;
                if (!g_loggedMouseRawStrip) {
                    g_loggedMouseRawStrip = true;
                    CoreLog("Stripped RawInput mouse flags 0x%lX -> 0x%lX (unblocked Monitor 2 clicks)", oldFlags, d.dwFlags);
                }
            }
        } else if (IsRawKbdUsage(d.usUsagePage, d.usUsage)) {
            shm_store32(&g_shm->detected_rawinput_kbm, 1);
            if (g_rawSaveCsInit) {
                EnterCriticalSection(&g_rawSaveCs);
                g_savedOrigKbdRaw = d;
                g_hasSavedKbdRaw = true;
                LeaveCriticalSection(&g_rawSaveCs);
            }
            if (activeUnlock && (d.dwFlags & kKbdBadRawFlags) != 0) {
                DWORD oldFlags = d.dwFlags;
                d.dwFlags &= ~kKbdBadRawFlags;
                if (!g_loggedKbdRawStrip) {
                    g_loggedKbdRawStrip = true;
                    CoreLog("Stripped RawInput keyboard flags 0x%lX -> 0x%lX", oldFlags, d.dwFlags);
                }
            }
        }
    }

    return g_origRegisterRawInputDevices(patched.data(), uiNumDevices, cbSize);
}

static UINT WINAPI Hooked_GetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    UINT ret = g_origGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    if (ret == static_cast<UINT>(-1) || !pData || uiCommand != RID_INPUT) {
        return ret;
    }

    auto* raw = static_cast<RAWINPUT*>(pData);
    if (raw->header.dwType == RIM_TYPEMOUSE || raw->header.dwType == RIM_TYPEKEYBOARD) {
        if (g_shm) shm_store32(&g_shm->detected_rawinput_kbm, 1);
        if (ShouldBlockKBM()) {
            shm_inc64(&g_shm->blocked_raw_kbm);
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                raw->data.mouse.usFlags = 0;
                raw->data.mouse.usButtonFlags = 0;
                raw->data.mouse.usButtonData = 0;
                raw->data.mouse.ulRawButtons = 0;
                raw->data.mouse.lLastX = 0;
                raw->data.mouse.lLastY = 0;
            } else {
                raw->data.keyboard.MakeCode = 0;
                raw->data.keyboard.Flags = 0;
                raw->data.keyboard.VKey = 0;
                raw->data.keyboard.Message = 0;
                raw->data.keyboard.ExtraInformation = 0;
            }
        }
    } else if (raw->header.dwType == RIM_TYPEHID) {
        if (g_shm) shm_store32(&g_shm->detected_rawinput_pad, 1);
    }

    return ret;
}

void PatchExistingRawInputDevices() {
    if (!g_shm || g_detaching || !g_origRegisterRawInputDevices) return;
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) || count == 0) {
        return;
    }

    std::vector<RAWINPUTDEVICE> devs(count);
    UINT got = GetRegisteredRawInputDevices(devs.data(), &count, sizeof(RAWINPUTDEVICE));
    if (got == static_cast<UINT>(-1) || got == 0) return;

    bool activeUnlock = (shm_load32(&g_shm->enabled) != 0 && shm_load32(&g_shm->unlock_cursor) != 0);
    std::vector<RAWINPUTDEVICE> toPatch;
    size_t padCount = 0;

    for (UINT i = 0; i < got; ++i) {
        if (IsRawGamepadUsage(devs[i].usUsagePage, devs[i].usUsage)) {
            shm_store32(&g_shm->detected_rawinput_pad, 1);
            if (!(devs[i].dwFlags & RIDEV_INPUTSINK)) {
                RAWINPUTDEVICE d = devs[i];
                d.dwFlags |= RIDEV_INPUTSINK;
                if (!d.hwndTarget && g_gameHwnd) d.hwndTarget = g_gameHwnd;
                toPatch.push_back(d);
                padCount++;
            }
        } else if (IsRawMouseUsage(devs[i].usUsagePage, devs[i].usUsage)) {
            shm_store32(&g_shm->detected_rawinput_kbm, 1);
            if (activeUnlock && (devs[i].dwFlags & kMouseBadRawFlags) != 0) {
                if (g_rawSaveCsInit) {
                    EnterCriticalSection(&g_rawSaveCs);
                    if (!g_hasSavedMouseRaw) {
                        g_savedOrigMouseRaw = devs[i];
                        g_hasSavedMouseRaw = true;
                    }
                    LeaveCriticalSection(&g_rawSaveCs);
                }
                RAWINPUTDEVICE d = devs[i];
                DWORD oldFlags = d.dwFlags;
                d.dwFlags &= ~kMouseBadRawFlags;
                toPatch.push_back(d);
                CoreLog("Patched existing RawInput mouse flags 0x%lX -> 0x%lX", oldFlags, d.dwFlags);
            }
        } else if (IsRawKbdUsage(devs[i].usUsagePage, devs[i].usUsage)) {
            shm_store32(&g_shm->detected_rawinput_kbm, 1);
            if (activeUnlock && (devs[i].dwFlags & kKbdBadRawFlags) != 0) {
                if (g_rawSaveCsInit) {
                    EnterCriticalSection(&g_rawSaveCs);
                    if (!g_hasSavedKbdRaw) {
                        g_savedOrigKbdRaw = devs[i];
                        g_hasSavedKbdRaw = true;
                    }
                    LeaveCriticalSection(&g_rawSaveCs);
                }
                RAWINPUTDEVICE d = devs[i];
                DWORD oldFlags = d.dwFlags;
                d.dwFlags &= ~kKbdBadRawFlags;
                toPatch.push_back(d);
                CoreLog("Patched existing RawInput keyboard flags 0x%lX -> 0x%lX", oldFlags, d.dwFlags);
            }
        }
    }

    if (!toPatch.empty()) {
        if (g_origRegisterRawInputDevices(toPatch.data(), static_cast<UINT>(toPatch.size()), sizeof(RAWINPUTDEVICE))) {
            for (size_t i = 0; i < padCount; ++i) {
                shm_inc64(&g_shm->rawinput_pad_inputsink_added);
            }
            if (padCount > 0) {
                CoreLog("Patched %zu existing RawInput gamepad device(s) with RIDEV_INPUTSINK", padCount);
            }
        } else {
            CoreLog("WARNING: PatchExistingRawInputDevices RegisterRawInputDevices failed (err=%lu)", GetLastError());
        }
    }
}

void RestoreOriginalRawInputDevices() {
    if (!g_origRegisterRawInputDevices) return;
    std::vector<RAWINPUTDEVICE> restore;
    if (g_rawSaveCsInit) {
        EnterCriticalSection(&g_rawSaveCs);
        if (g_hasSavedMouseRaw) restore.push_back(g_savedOrigMouseRaw);
        if (g_hasSavedKbdRaw) restore.push_back(g_savedOrigKbdRaw);
        LeaveCriticalSection(&g_rawSaveCs);
    }
    if (!restore.empty()) {
        if (g_origRegisterRawInputDevices(restore.data(), static_cast<UINT>(restore.size()), sizeof(RAWINPUTDEVICE))) {
            CoreLog("Restored %zu original RawInput KBM device registration(s)", restore.size());
        }
    }
}

// ---- XInput Lazy Hooks ----
struct XInputSlot {
    const wchar_t* dllName;
    bool hooked;
    void(WINAPI* origEnable)(BOOL);
    DWORD(WINAPI* origGetState)(DWORD, XINPUT_STATE*);
};

static XInputSlot g_xinputSlots[] = {
    {L"xinput1_4.dll", false, nullptr, nullptr},
    {L"xinput1_3.dll", false, nullptr, nullptr},
    {L"xinput9_1_0.dll", false, nullptr, nullptr},
};

template <int Idx>
static void WINAPI Hooked_XInputEnable(BOOL enable) {
    if (!enable && g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->spoof_focus)) {
        shm_inc64(&g_shm->xinput_enable_blocked);
        return;
    }
    if (g_xinputSlots[Idx].origEnable) {
        g_xinputSlots[Idx].origEnable(enable);
    }
}

template <int Idx>
static DWORD WINAPI Hooked_XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    if (g_shm && !g_detaching) {
        shm_store32(&g_shm->detected_xinput, 1);
        shm_inc64(&g_shm->xinput_polls);
    }
    return g_xinputSlots[Idx].origGetState(dwUserIndex, pState);
}

static void TryHookXInput() {
    for (int i = 0; i < 3; ++i) {
        if (g_xinputSlots[i].hooked) continue;
        HMODULE mod = GetModuleHandleW(g_xinputSlots[i].dllName);
        if (!mod) continue;

        void* pEnable = reinterpret_cast<void*>(GetProcAddress(mod, "XInputEnable"));
        void* pGetState = reinterpret_cast<void*>(GetProcAddress(mod, "XInputGetState"));

        void* hookEn = (i == 0) ? reinterpret_cast<void*>(&Hooked_XInputEnable<0>)
                     : (i == 1) ? reinterpret_cast<void*>(&Hooked_XInputEnable<1>)
                                : reinterpret_cast<void*>(&Hooked_XInputEnable<2>);
        void* hookGs = (i == 0) ? reinterpret_cast<void*>(&Hooked_XInputGetState<0>)
                     : (i == 1) ? reinterpret_cast<void*>(&Hooked_XInputGetState<1>)
                                : reinterpret_cast<void*>(&Hooked_XInputGetState<2>);

        bool anyHooked = false;
        if (pEnable) {
            if (MH_CreateHook(pEnable, hookEn, reinterpret_cast<void**>(&g_xinputSlots[i].origEnable)) == MH_OK) {
                MH_EnableHook(pEnable);
                anyHooked = true;
            }
        }
        if (pGetState) {
            if (MH_CreateHook(pGetState, hookGs, reinterpret_cast<void**>(&g_xinputSlots[i].origGetState)) == MH_OK) {
                MH_EnableHook(pGetState);
                anyHooked = true;
            }
        }
        if (anyHooked) {
            g_xinputSlots[i].hooked = true;
            CoreLog("Hooked XInput (%ls)", g_xinputSlots[i].dllName);
        }
    }
}

// ---- DirectInput8 Lazy Hooks ----
static const GUID kIID_IDirectInput8W = {0xBF798031, 0x483A, 0x4DA2, {0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00}};
static const GUID kGUID_SysKeyboard  = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

using FnDirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using FnDInputGetDeviceState = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPVOID);
using FnDInputGetDeviceData = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using FnDInputSetCooperativeLevel = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8W*, HWND, DWORD);

static FnDInputGetDeviceState g_origDIGetDeviceState = nullptr;
static FnDInputGetDeviceData g_origDIGetDeviceData = nullptr;
static FnDInputSetCooperativeLevel g_origDISetCoopLevel = nullptr;
static bool g_dinputHooked = false;

static CRITICAL_SECTION g_diCacheCs;
static bool g_diCacheInit = false;
static CRITICAL_SECTION g_wgiCacheCs;
static bool g_wgiCacheInit = false;

struct DICacheEntry {
    void* dev;
    bool isKbm;
};
static DICacheEntry g_diCache[64]{};
static int g_diCacheCount = 0;

static bool IsDirectInputDeviceKbm(IDirectInputDevice8W* dev) {
    if (!dev) return false;
    if (g_diCacheInit) {
        EnterCriticalSection(&g_diCacheCs);
        for (int i = 0; i < g_diCacheCount; ++i) {
            if (g_diCache[i].dev == dev) {
                bool r = g_diCache[i].isKbm;
                LeaveCriticalSection(&g_diCacheCs);
                return r;
            }
        }
        LeaveCriticalSection(&g_diCacheCs);
    }

    DIDEVICEINSTANCEW dii{};
    dii.dwSize = sizeof(dii);
    bool isKbm = false;
    if (SUCCEEDED(dev->GetDeviceInfo(&dii))) {
        DWORD devType = GET_DIDEVICE_TYPE(dii.dwDevType);
        isKbm = (devType == DI8DEVTYPE_MOUSE || devType == DI8DEVTYPE_KEYBOARD);
    } else {
        DIDEVICEINSTANCEA diiA{};
        diiA.dwSize = sizeof(diiA);
        if (SUCCEEDED(reinterpret_cast<IDirectInputDevice8A*>(dev)->GetDeviceInfo(&diiA))) {
            DWORD devType = GET_DIDEVICE_TYPE(diiA.dwDevType);
            isKbm = (devType == DI8DEVTYPE_MOUSE || devType == DI8DEVTYPE_KEYBOARD);
        }
    }

    if (g_diCacheInit) {
        EnterCriticalSection(&g_diCacheCs);
        if (g_diCacheCount < 64) {
            g_diCache[g_diCacheCount++] = {dev, isKbm};
        } else {
            static int s_nextEvict = 0;
            g_diCache[s_nextEvict] = {dev, isKbm};
            s_nextEvict = (s_nextEvict + 1) % 64;
        }
        LeaveCriticalSection(&g_diCacheCs);
    }
    return isKbm;
}

static HRESULT STDMETHODCALLTYPE Hooked_DISetCooperativeLevel(IDirectInputDevice8W* self, HWND hwnd, DWORD dwFlags) {
    bool isKbm = IsDirectInputDeviceKbm(self);
    if (!isKbm && g_shm && !g_detaching && shm_load32(&g_shm->enabled)) {
        shm_store32(&g_shm->detected_dinput_pad, 1);
        DWORD patched = dwFlags;
        if (patched & DISCL_FOREGROUND) {
            patched = (patched & ~static_cast<DWORD>(DISCL_FOREGROUND)) | DISCL_BACKGROUND;
        }
        if (patched & DISCL_EXCLUSIVE) {
            patched = (patched & ~static_cast<DWORD>(DISCL_EXCLUSIVE)) | DISCL_NONEXCLUSIVE;
        }
        if (patched != dwFlags) {
            shm_inc64(&g_shm->dinput_coop_patched);
            dwFlags = patched;
        }
    } else if (isKbm && g_shm) {
        shm_store32(&g_shm->detected_dinput_kbm, 1);
    }
    return g_origDISetCoopLevel(self, hwnd, dwFlags);
}

static HRESULT STDMETHODCALLTYPE Hooked_DIGetDeviceState(IDirectInputDevice8W* self, DWORD cbData, LPVOID lpvData) {
    bool isKbm = IsDirectInputDeviceKbm(self);
    if (isKbm) {
        if (g_shm) shm_store32(&g_shm->detected_dinput_kbm, 1);
        if (ShouldBlockKBM()) {
            shm_inc64(&g_shm->blocked_dinput_kbm);
            if (lpvData && cbData > 0) std::memset(lpvData, 0, cbData);
            return DI_OK;
        }
        return g_origDIGetDeviceState(self, cbData, lpvData);
    }

    if (g_shm) shm_store32(&g_shm->detected_dinput_pad, 1);
    HRESULT hr = g_origDIGetDeviceState(self, cbData, lpvData);
    if ((hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) && g_shm && shm_load32(&g_shm->enabled)) {
        if (SUCCEEDED(self->Acquire())) {
            hr = g_origDIGetDeviceState(self, cbData, lpvData);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_DIGetDeviceData(IDirectInputDevice8W* self, DWORD cbObjectData, LPDIDEVICEOBJECTDATA rgdod, LPDWORD pdwInOut, DWORD dwFlags) {
    bool isKbm = IsDirectInputDeviceKbm(self);
    if (isKbm) {
        if (g_shm) shm_store32(&g_shm->detected_dinput_kbm, 1);
        if (ShouldBlockKBM()) {
            shm_inc64(&g_shm->blocked_dinput_kbm);
            if (pdwInOut) *pdwInOut = 0;
            return DI_OK;
        }
        return g_origDIGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags);
    }

    if (g_shm) shm_store32(&g_shm->detected_dinput_pad, 1);
    HRESULT hr = g_origDIGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags);
    if ((hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) && g_shm && shm_load32(&g_shm->enabled)) {
        if (SUCCEEDED(self->Acquire())) {
            hr = g_origDIGetDeviceData(self, cbObjectData, rgdod, pdwInOut, dwFlags);
        }
    }
    return hr;
}

static void TryHookDirectInput8() {
    if (g_dinputHooked) return;
    HMODULE diMod = GetModuleHandleW(L"dinput8.dll");
    if (!diMod) return;

    auto fnCreate = reinterpret_cast<FnDirectInput8Create>(GetProcAddress(diMod, "DirectInput8Create"));
    if (!fnCreate) return;

    IDirectInput8W* di = nullptr;
    if (FAILED(fnCreate(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr)) || !di) {
        return;
    }

    IDirectInputDevice8W* dev = nullptr;
    if (SUCCEEDED(di->CreateDevice(kGUID_SysKeyboard, &dev, nullptr)) && dev) {
        void** vtbl = *reinterpret_cast<void***>(dev);
        void* pGetState = vtbl[9];
        void* pGetData = vtbl[10];
        void* pSetCoop = vtbl[13];

        if (MH_CreateHook(pGetState, reinterpret_cast<void*>(&Hooked_DIGetDeviceState), reinterpret_cast<void**>(&g_origDIGetDeviceState)) == MH_OK) {
            MH_EnableHook(pGetState);
        }
        if (MH_CreateHook(pGetData, reinterpret_cast<void*>(&Hooked_DIGetDeviceData), reinterpret_cast<void**>(&g_origDIGetDeviceData)) == MH_OK) {
            MH_EnableHook(pGetData);
        }
        if (MH_CreateHook(pSetCoop, reinterpret_cast<void*>(&Hooked_DISetCooperativeLevel), reinterpret_cast<void**>(&g_origDISetCoopLevel)) == MH_OK) {
            MH_EnableHook(pSetCoop);
        }

        g_dinputHooked = true;
        CoreLog("Hooked DirectInput8 device vtable");
        dev->Release();
    }
    di->Release();
}

// ---- Public Interface ----
bool InstallInputHooks() {
    if (!g_rawSaveCsInit) {
        InitializeCriticalSection(&g_rawSaveCs);
        g_rawSaveCsInit = true;
    }
    if (!g_diCacheInit) {
        InitializeCriticalSection(&g_diCacheCs);
        g_diCacheInit = true;
    }
    if (!g_wgiCacheInit) {
        InitializeCriticalSection(&g_wgiCacheCs);
        g_wgiCacheInit = true;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;

    void* pAsync = reinterpret_cast<void*>(GetProcAddress(user32, "GetAsyncKeyState"));
    void* pKey = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyState"));
    void* pKbd = reinterpret_cast<void*>(GetProcAddress(user32, "GetKeyboardState"));
    void* pRegRaw = reinterpret_cast<void*>(GetProcAddress(user32, "RegisterRawInputDevices"));
    void* pGetRaw = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"));

    if (pAsync) MH_CreateHook(pAsync, reinterpret_cast<void*>(&Hooked_GetAsyncKeyState), reinterpret_cast<void**>(&g_origGetAsyncKeyState));
    if (pKey) MH_CreateHook(pKey, reinterpret_cast<void*>(&Hooked_GetKeyState), reinterpret_cast<void**>(&g_origGetKeyState));
    if (pKbd) MH_CreateHook(pKbd, reinterpret_cast<void*>(&Hooked_GetKeyboardState), reinterpret_cast<void**>(&g_origGetKeyboardState));
    if (pRegRaw) MH_CreateHook(pRegRaw, reinterpret_cast<void*>(&Hooked_RegisterRawInputDevices), reinterpret_cast<void**>(&g_origRegisterRawInputDevices));
    if (pGetRaw) MH_CreateHook(pGetRaw, reinterpret_cast<void*>(&Hooked_GetRawInputData), reinterpret_cast<void**>(&g_origGetRawInputData));

    CoreLog("Win32 & RawInput hooks created");
    return true;
}

// ---- Microsoft GameInput (v1/v2/v3/v4) Lazy Hooks ----
static const GUID kIID_IGameInput_v4 = {0xf2daae4c, 0x4d21, 0x4250, {0x89, 0x53, 0x4f, 0x79, 0x1e, 0x62, 0x79, 0xab}};
static const GUID kIID_IGameInput_v3 = {0x20efc1c7, 0x5d9a, 0x43ba, {0xb2, 0x6f, 0xb8, 0x07, 0xfa, 0x48, 0x60, 0x9c}};
static const GUID kIID_IGameInput_v2 = {0xbbaa66d2, 0x837a, 0x40f7, {0xa3, 0x03, 0x91, 0x7d, 0x50, 0x09, 0x55, 0xf4}};
static const GUID kIID_IGameInput_v1 = {0x40ffb7e4, 0x6150, 0x407a, {0xb4, 0x39, 0x13, 0x2b, 0xad, 0xc0, 0x8d, 0x2d}};

constexpr uint32_t kGameInputKindKbmMask = 0x00000030u; // Keyboard(0x10) | Mouse(0x20)
constexpr uint32_t kGameInputEnableBackgroundInput = 0x00000040u;
constexpr uint32_t kGameInputExclusiveForegroundMask = 0x00000002u;
constexpr HRESULT kGameInputReadingNotFound = static_cast<HRESULT>(0x838A0003L);

using FnGameInputInitialize = HRESULT(WINAPI*)(REFIID, LPVOID*);
using FnGameInputCreate = HRESULT(WINAPI*)(LPVOID*);
using FnGI_GetCurrentReading = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void*, void**);
using FnGI_GetNextReading = HRESULT(STDMETHODCALLTYPE*)(void*, void*, uint32_t, void*, void**);
using FnGI_EnableBackgroundInput = void(STDMETHODCALLTYPE*)(void*, bool);
using FnGI_SetFocusPolicy = void(STDMETHODCALLTYPE*)(void*, uint32_t);

static FnGI_GetCurrentReading g_origGIGetCurrentReading = nullptr;
static FnGI_GetNextReading g_origGIGetNextReading = nullptr;
static FnGI_GetNextReading g_origGIGetPrevReading = nullptr;
static FnGI_EnableBackgroundInput g_origGIEnableBg = nullptr;
static FnGI_SetFocusPolicy g_origGISetFocusPolicy = nullptr;
static bool g_gameInputHooked = false;

static HRESULT STDMETHODCALLTYPE Hooked_GI_GetCurrentReading(void* self, uint32_t inputKind, void* device, void** reading) {
    if (g_shm) shm_store32(&g_shm->detected_gameinput, 1);
    if (ShouldBlockKBM() && (inputKind & kGameInputKindKbmMask) != 0) {
        uint32_t filtered = inputKind & ~kGameInputKindKbmMask;
        shm_inc64(&g_shm->blocked_gameinput_kbm);
        if (filtered == 0) {
            if (reading) *reading = nullptr;
            return kGameInputReadingNotFound;
        }
        inputKind = filtered;
    }
    return g_origGIGetCurrentReading(self, inputKind, device, reading);
}

static HRESULT STDMETHODCALLTYPE Hooked_GI_GetNextReading(void* self, void* refReading, uint32_t inputKind, void* device, void** reading) {
    if (g_shm) shm_store32(&g_shm->detected_gameinput, 1);
    if (ShouldBlockKBM() && (inputKind & kGameInputKindKbmMask) != 0) {
        uint32_t filtered = inputKind & ~kGameInputKindKbmMask;
        shm_inc64(&g_shm->blocked_gameinput_kbm);
        if (filtered == 0) {
            if (reading) *reading = nullptr;
            return kGameInputReadingNotFound;
        }
        inputKind = filtered;
    }
    return g_origGIGetNextReading(self, refReading, inputKind, device, reading);
}

static HRESULT STDMETHODCALLTYPE Hooked_GI_GetPreviousReading(void* self, void* refReading, uint32_t inputKind, void* device, void** reading) {
    if (g_shm) shm_store32(&g_shm->detected_gameinput, 1);
    if (ShouldBlockKBM() && (inputKind & kGameInputKindKbmMask) != 0) {
        uint32_t filtered = inputKind & ~kGameInputKindKbmMask;
        shm_inc64(&g_shm->blocked_gameinput_kbm);
        if (filtered == 0) {
            if (reading) *reading = nullptr;
            return kGameInputReadingNotFound;
        }
        inputKind = filtered;
    }
    return g_origGIGetPrevReading(self, refReading, inputKind, device, reading);
}

static void STDMETHODCALLTYPE Hooked_GI_EnableBackgroundInput(void* self, bool enable) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->spoof_focus)) {
        if (!enable) {
            enable = true;
            shm_inc64(&g_shm->gameinput_focus_patched);
        }
    }
    g_origGIEnableBg(self, enable);
}

static void STDMETHODCALLTYPE Hooked_GI_SetFocusPolicy(void* self, uint32_t policy) {
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->spoof_focus)) {
        uint32_t patched = (policy & ~kGameInputExclusiveForegroundMask) | kGameInputEnableBackgroundInput;
        if (patched != policy) {
            policy = patched;
            shm_inc64(&g_shm->gameinput_focus_patched);
        }
    }
    g_origGISetFocusPolicy(self, policy);
}

static void TryHookGameInput() {
    if (g_gameInputHooked) return;
    HMODULE giMod = GetModuleHandleW(L"GameInputRedist.dll");
    if (!giMod) giMod = GetModuleHandleW(L"GameInput.dll");
    if (!giMod) return;

    auto fnInit = reinterpret_cast<FnGameInputInitialize>(GetProcAddress(giMod, "GameInputInitialize"));
    auto fnCreate = reinterpret_cast<FnGameInputCreate>(GetProcAddress(giMod, "GameInputCreate"));
    if (!fnInit && !fnCreate) return;

    void* gi = nullptr;
    int giVer = 0;
    if (fnInit) {
        if (SUCCEEDED(fnInit(kIID_IGameInput_v4, &gi)) && gi) giVer = 4;
        else if (SUCCEEDED(fnInit(kIID_IGameInput_v3, &gi)) && gi) giVer = 3;
        else if (SUCCEEDED(fnInit(kIID_IGameInput_v2, &gi)) && gi) giVer = 2;
        else if (SUCCEEDED(fnInit(kIID_IGameInput_v1, &gi)) && gi) giVer = 1;
    }
    if (!gi && fnCreate) {
        if (SUCCEEDED(fnCreate(&gi)) && gi) giVer = 3;
    }
    if (!gi) return;

    void** vtbl = *reinterpret_cast<void***>(gi);
    if (MH_CreateHook(vtbl[4], reinterpret_cast<void*>(&Hooked_GI_GetCurrentReading),
                      reinterpret_cast<void**>(&g_origGIGetCurrentReading)) == MH_OK) {
        MH_EnableHook(vtbl[4]);
    }
    if (MH_CreateHook(vtbl[5], reinterpret_cast<void*>(&Hooked_GI_GetNextReading),
                      reinterpret_cast<void**>(&g_origGIGetNextReading)) == MH_OK) {
        MH_EnableHook(vtbl[5]);
    }
    if (MH_CreateHook(vtbl[6], reinterpret_cast<void*>(&Hooked_GI_GetPreviousReading),
                      reinterpret_cast<void**>(&g_origGIGetPrevReading)) == MH_OK) {
        MH_EnableHook(vtbl[6]);
    }

    if (giVer == 4) {
        if (MH_CreateHook(vtbl[13], reinterpret_cast<void*>(&Hooked_GI_EnableBackgroundInput),
                          reinterpret_cast<void**>(&g_origGIEnableBg)) == MH_OK) {
            MH_EnableHook(vtbl[13]);
            if (g_shm && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->spoof_focus)) {
                g_origGIEnableBg(gi, true);
                shm_inc64(&g_shm->gameinput_focus_patched);
            }
        }
    } else {
        if (MH_CreateHook(vtbl[16], reinterpret_cast<void*>(&Hooked_GI_SetFocusPolicy),
                          reinterpret_cast<void**>(&g_origGISetFocusPolicy)) == MH_OK) {
            MH_EnableHook(vtbl[16]);
            if (g_shm && shm_load32(&g_shm->enabled) && shm_load32(&g_shm->spoof_focus)) {
                g_origGISetFocusPolicy(gi, kGameInputEnableBackgroundInput);
                shm_inc64(&g_shm->gameinput_focus_patched);
            }
        }
    }

    static_cast<IUnknown*>(gi)->Release();
    g_gameInputHooked = true;
    if (g_shm) shm_store32(&g_shm->detected_gameinput, 1);
    CoreLog("Hooked Microsoft GameInput v%d (background gamepad enabled + KBM filtered)", giVer);
}

// ---- Windows.Gaming.Input (WinRT) Lazy Hooks ----
// Windows.Gaming.Input intentionally returns zeroed readings when the calling process is not
// the real OS foreground window. When PadSplit is spoofing focus in the background, bridge
// IGamepad::GetCurrentReading and IRawGameController::GetCurrentReading from live XInput state.
static const GUID kIID_IGamepadStatics = {0x8bbce529, 0xd49c, 0x39e9, {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};
static const GUID kIID_IRawGameControllerStatics = {0xeb8d0792, 0xe95a, 0x4b19, {0xaf, 0xc7, 0x0a, 0x59, 0xf8, 0xbf, 0x75, 0x9e}};

struct WgiHStringHeader {
    union {
        void* Reserved1;
        char Reserved2[sizeof(void*) == 8 ? 24 : 20];
    } Reserved;
};
using WgiHString = void*;

struct WgiGamepadReading {
    uint64_t Timestamp;
    uint32_t Buttons;
    uint32_t _pad;
    double LeftTrigger;
    double RightTrigger;
    double LeftThumbstickX;
    double LeftThumbstickY;
    double RightThumbstickX;
    double RightThumbstickY;
};
static_assert(sizeof(WgiGamepadReading) == 64, "WgiGamepadReading size mismatch");

using FnRoInitialize = HRESULT(WINAPI*)(int);
using FnRoGetActivationFactory = HRESULT(WINAPI*)(WgiHString, REFIID, void**);
using FnWindowsCreateStringReference = HRESULT(WINAPI*)(PCWSTR, UINT32, WgiHStringHeader*, WgiHString*);

using FnWgiStaticsGetList = HRESULT(STDMETHODCALLTYPE*)(void*, void**);
using FnWgiVecGetAt = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, void**);
using FnWgiVecGetSize = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t*);
using FnWgiGamepadGetReading = HRESULT(STDMETHODCALLTYPE*)(void*, WgiGamepadReading*);
using FnWgiRawGetButtonLabel = HRESULT(STDMETHODCALLTYPE*)(void*, int32_t, int32_t*);
using FnWgiRawGetReading = HRESULT(STDMETHODCALLTYPE*)(void*, uint32_t, uint8_t*, uint32_t, int32_t*, uint32_t, double*, uint64_t*);
using FnXInputGetStateRaw = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

static FnWgiGamepadGetReading g_origWgiGamepadGetReading = nullptr;
static FnWgiRawGetReading g_origWgiRawGetReading = nullptr;
static bool g_wgiRoInitDone = false;
static bool g_wgiGamepadHooked = false;
static bool g_wgiRawHooked = false;
static bool g_wgiGamepadCallLogged = false;
static bool g_wgiGamepadBridgeLogged = false;
static bool g_wgiRawCallLogged = false;
static bool g_wgiRawBridgeLogged = false;
static FnXInputGetStateRaw g_fallbackXInputGetState = nullptr;

struct WgiRawCacheEntry {
    void* dev;
    uint32_t btnCount;
    int32_t labels[32];
};
static WgiRawCacheEntry g_wgiRawCache[16]{};
static int g_wgiRawCacheCount = 0;

static uint64_t NowMicrosecondsQpc() {
    static LARGE_INTEGER s_freq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (s_freq.QuadPart <= 0) return GetTickCount64() * 1000ULL;
    return static_cast<uint64_t>((now.QuadPart * 1000000LL) / s_freq.QuadPart);
}

static bool ReadBackgroundXInput(DWORD preferredSlot, XINPUT_STATE* outState, DWORD* outUsedSlot) {
    if (!outState) return false;
    FnXInputGetStateRaw fn = nullptr;
    for (int i = 0; i < 3; ++i) {
        if (g_xinputSlots[i].origGetState) {
            fn = g_xinputSlots[i].origGetState;
            break;
        }
    }
    if (!fn) {
        if (!g_fallbackXInputGetState) {
            const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
            for (const wchar_t* name : dlls) {
                HMODULE m = GetModuleHandleW(name);
                if (!m) m = LoadLibraryW(name);
                if (m) {
                    auto p = reinterpret_cast<FnXInputGetStateRaw>(GetProcAddress(m, "XInputGetState"));
                    if (p) {
                        g_fallbackXInputGetState = p;
                        break;
                    }
                }
            }
        }
        fn = g_fallbackXInputGetState;
    }
    if (!fn) return false;

    if (preferredSlot < 4 && fn(preferredSlot, outState) == ERROR_SUCCESS) {
        if (outUsedSlot) *outUsedSlot = preferredSlot;
        return true;
    }
    for (DWORD s = 0; s < 4; ++s) {
        if (s == preferredSlot) continue;
        if (fn(s, outState) == ERROR_SUCCESS) {
            if (outUsedSlot) *outUsedSlot = s;
            return true;
        }
    }
    return false;
}

static bool IsGameCurrentlyBackground() {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return false;
    HWND realFg = g_origGetForegroundWindow ? g_origGetForegroundWindow() : GetForegroundWindow();
    return realFg != g_gameHwnd;
}

static double NormalizeThumbSigned(SHORT v) {
    if (v <= -32768) return -1.0;
    return static_cast<double>(v) / 32767.0;
}

static double NormalizeThumbUnit(SHORT v, bool invert) {
    double u = (static_cast<double>(v) + 32768.0) / 65535.0;
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    return invert ? (1.0 - u) : u;
}

static int32_t DpadToSwitchPos(WORD wButtons) {
    bool up = (wButtons & XINPUT_GAMEPAD_DPAD_UP) != 0;
    bool down = (wButtons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    bool left = (wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    bool right = (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
    if (up && right) return 2;
    if (down && right) return 4;
    if (down && left) return 6;
    if (up && left) return 8;
    if (up) return 1;
    if (right) return 3;
    if (down) return 5;
    if (left) return 7;
    return 0;
}

static HRESULT STDMETHODCALLTYPE Hooked_WGI_Gamepad_GetCurrentReading(void* self, WgiGamepadReading* value) {
    HRESULT hr = g_origWgiGamepadGetReading(self, value);
    if (!g_wgiGamepadCallLogged) {
        g_wgiGamepadCallLogged = true;
        CoreLog("WGI IGamepad::GetCurrentReading active");
    }
    if (SUCCEEDED(hr) && value && ShouldSpoofFocus() && IsGameCurrentlyBackground()) {
        XINPUT_STATE xs{};
        DWORD usedSlot = 0;
        if (ReadBackgroundXInput(0, &xs, &usedSlot)) {
            WORD b = xs.Gamepad.wButtons;
            uint32_t wb = 0;
            if (b & XINPUT_GAMEPAD_START)          wb |= 0x0001u; // Menu
            if (b & XINPUT_GAMEPAD_BACK)           wb |= 0x0002u; // View
            if (b & XINPUT_GAMEPAD_A)              wb |= 0x0004u; // A
            if (b & XINPUT_GAMEPAD_B)              wb |= 0x0008u; // B
            if (b & XINPUT_GAMEPAD_X)              wb |= 0x0010u; // X
            if (b & XINPUT_GAMEPAD_Y)              wb |= 0x0020u; // Y
            if (b & XINPUT_GAMEPAD_DPAD_UP)        wb |= 0x0040u; // DPadUp
            if (b & XINPUT_GAMEPAD_DPAD_DOWN)      wb |= 0x0080u; // DPadDown
            if (b & XINPUT_GAMEPAD_DPAD_LEFT)      wb |= 0x0100u; // DPadLeft
            if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     wb |= 0x0200u; // DPadRight
            if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  wb |= 0x0400u; // LeftShoulder
            if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) wb |= 0x0800u; // RightShoulder
            if (b & XINPUT_GAMEPAD_LEFT_THUMB)     wb |= 0x1000u; // LeftThumbstick
            if (b & XINPUT_GAMEPAD_RIGHT_THUMB)    wb |= 0x2000u; // RightThumbstick

            value->Timestamp = NowMicrosecondsQpc();
            value->Buttons = wb;
            value->LeftTrigger = static_cast<double>(xs.Gamepad.bLeftTrigger) / 255.0;
            value->RightTrigger = static_cast<double>(xs.Gamepad.bRightTrigger) / 255.0;
            value->LeftThumbstickX = NormalizeThumbSigned(xs.Gamepad.sThumbLX);
            value->LeftThumbstickY = NormalizeThumbSigned(xs.Gamepad.sThumbLY);
            value->RightThumbstickX = NormalizeThumbSigned(xs.Gamepad.sThumbRX);
            value->RightThumbstickY = NormalizeThumbSigned(xs.Gamepad.sThumbRY);

            if (!g_wgiGamepadBridgeLogged) {
                g_wgiGamepadBridgeLogged = true;
                CoreLog("WGI IGamepad background read bridged via XInput (slot=%lu)", static_cast<unsigned long>(usedSlot));
            }
        }
    }
    return hr;
}

static bool MapLabelToXInputButton(int32_t label, WORD wButtons, BYTE lt, BYTE rt, uint8_t& outPressed) {
    switch (label) {
    case 1:  // XboxBack
    case 4:  // XboxView
    case 24: // Select
    case 26: // View
    case 27: // Back
        outPressed = (wButtons & XINPUT_GAMEPAD_BACK) ? 1 : 0;
        return true;
    case 2:  // XboxStart
    case 3:  // XboxMenu
    case 25: // Menu
    case 28: // Start
    case 29: // Options
        outPressed = (wButtons & XINPUT_GAMEPAD_START) ? 1 : 0;
        return true;
    case 5:  // XboxUp
    case 31: // Up
        outPressed = (wButtons & XINPUT_GAMEPAD_DPAD_UP) ? 1 : 0;
        return true;
    case 6:  // XboxDown
    case 32: // Down
        outPressed = (wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1 : 0;
        return true;
    case 7:  // XboxLeft
    case 33: // Left
        outPressed = (wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1 : 0;
        return true;
    case 8:  // XboxRight
    case 34: // Right
        outPressed = (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1 : 0;
        return true;
    case 9:  // XboxA
    case 35: // LetterA
    case 43: // Cross
        outPressed = (wButtons & XINPUT_GAMEPAD_A) ? 1 : 0;
        return true;
    case 10: // XboxB
    case 36: // LetterB
    case 44: // Circle
        outPressed = (wButtons & XINPUT_GAMEPAD_B) ? 1 : 0;
        return true;
    case 11: // XboxX
    case 40: // LetterX
        outPressed = (wButtons & XINPUT_GAMEPAD_X) ? 1 : 0;
        return true;
    case 12: // XboxY
    case 41: // LetterY
        outPressed = (wButtons & XINPUT_GAMEPAD_Y) ? 1 : 0;
        return true;
    case 13: // XboxLeftBumper
        outPressed = (wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 1 : 0;
        return true;
    case 14: // XboxLeftTrigger
        outPressed = (lt > 30) ? 1 : 0;
        return true;
    case 15: // XboxLeftStickButton
        outPressed = (wButtons & XINPUT_GAMEPAD_LEFT_THUMB) ? 1 : 0;
        return true;
    case 16: // XboxRightBumper
        outPressed = (wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 1 : 0;
        return true;
    case 17: // XboxRightTrigger
        outPressed = (rt > 30) ? 1 : 0;
        return true;
    case 18: // XboxRightStickButton
        outPressed = (wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) ? 1 : 0;
        return true;
    default:
        return false;
    }
}

static const WgiRawCacheEntry* GetOrInitWgiRawLabels(void* self, uint32_t btnCount) {
    if (!self) return nullptr;
    if (btnCount > 32) btnCount = 32;
    if (g_wgiCacheInit) {
        EnterCriticalSection(&g_wgiCacheCs);
        for (int i = 0; i < g_wgiRawCacheCount; ++i) {
            if (g_wgiRawCache[i].dev == self) {
                const WgiRawCacheEntry* e = &g_wgiRawCache[i];
                LeaveCriticalSection(&g_wgiCacheCs);
                return e;
            }
        }
        LeaveCriticalSection(&g_wgiCacheCs);
    }

    WgiRawCacheEntry entry{};
    entry.dev = self;
    entry.btnCount = btnCount;
    void** vtbl = *reinterpret_cast<void***>(self);
    auto fnGetLabel = reinterpret_cast<FnWgiRawGetButtonLabel>(vtbl[12]);
    for (uint32_t i = 0; i < btnCount; ++i) {
        int32_t lbl = 0;
        if (fnGetLabel && SUCCEEDED(fnGetLabel(self, static_cast<int32_t>(i), &lbl))) {
            entry.labels[i] = lbl;
        }
    }

    if (g_wgiCacheInit) {
        EnterCriticalSection(&g_wgiCacheCs);
        if (g_wgiRawCacheCount < 16) {
            g_wgiRawCache[g_wgiRawCacheCount] = entry;
            const WgiRawCacheEntry* e = &g_wgiRawCache[g_wgiRawCacheCount++];
            LeaveCriticalSection(&g_wgiCacheCs);
            return e;
        }
        LeaveCriticalSection(&g_wgiCacheCs);
    }
    return nullptr;
}

static HRESULT STDMETHODCALLTYPE Hooked_WGI_RawGameController_GetCurrentReading(
    void* self,
    uint32_t buttonsSize, uint8_t* buttons,
    uint32_t switchesSize, int32_t* switches,
    uint32_t axesSize, double* axes,
    uint64_t* timestamp) {
    HRESULT hr = g_origWgiRawGetReading(self, buttonsSize, buttons, switchesSize, switches, axesSize, axes, timestamp);
    if (!g_wgiRawCallLogged) {
        g_wgiRawCallLogged = true;
        CoreLog("WGI IRawGameController::GetCurrentReading active (btns=%u sw=%u axes=%u)",
                buttonsSize, switchesSize, axesSize);
    }
    if (SUCCEEDED(hr) && ShouldSpoofFocus() && IsGameCurrentlyBackground()) {
        XINPUT_STATE xs{};
        DWORD usedSlot = 0;
        if (ReadBackgroundXInput(0, &xs, &usedSlot)) {
            WORD b = xs.Gamepad.wButtons;
            BYTE lt = xs.Gamepad.bLeftTrigger;
            BYTE rt = xs.Gamepad.bRightTrigger;

            if (buttons && buttonsSize > 0) {
                const WgiRawCacheEntry* meta = GetOrInitWgiRawLabels(self, buttonsSize);
                bool anyLabelMapped = false;
                if (meta) {
                    for (uint32_t i = 0; i < buttonsSize && i < meta->btnCount; ++i) {
                        uint8_t pressed = 0;
                        if (MapLabelToXInputButton(meta->labels[i], b, lt, rt, pressed)) {
                            buttons[i] = pressed;
                            anyLabelMapped = true;
                        }
                    }
                }
                if (!anyLabelMapped) {
                    static const WORD kStdOrder[10] = {
                        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
                        XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START,
                        XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB
                    };
                    for (uint32_t i = 0; i < buttonsSize && i < 10; ++i) {
                        buttons[i] = (b & kStdOrder[i]) ? 1 : 0;
                    }
                }
            }

            if (switches && switchesSize >= 1) {
                switches[0] = DpadToSwitchPos(b);
            }

            if (axes && axesSize >= 4) {
                axes[0] = NormalizeThumbUnit(xs.Gamepad.sThumbLX, false);
                axes[1] = NormalizeThumbUnit(xs.Gamepad.sThumbLY, true);
                axes[2] = NormalizeThumbUnit(xs.Gamepad.sThumbRX, false);
                axes[3] = NormalizeThumbUnit(xs.Gamepad.sThumbRY, true);
                if (axesSize >= 6) {
                    axes[4] = static_cast<double>(lt) / 255.0;
                    axes[5] = static_cast<double>(rt) / 255.0;
                } else if (axesSize == 5) {
                    axes[4] = 0.5 + (static_cast<double>(lt) - static_cast<double>(rt)) / 510.0;
                }
            }

            if (timestamp) {
                *timestamp = NowMicrosecondsQpc();
            }

            if (!g_wgiRawBridgeLogged) {
                g_wgiRawBridgeLogged = true;
                CoreLog("WGI IRawGameController background read bridged via XInput (slot=%lu, btns=%u sw=%u axes=%u)",
                        static_cast<unsigned long>(usedSlot), buttonsSize, switchesSize, axesSize);
            }
        }
    }
    return hr;
}

static void TryHookWindowsGamingInput() {
    if (g_wgiGamepadHooked && g_wgiRawHooked) return;
    HMODULE wgiMod = GetModuleHandleW(L"Windows.Gaming.Input.dll");
    if (!wgiMod) return;
    HMODULE combase = GetModuleHandleW(L"combase.dll");
    if (!combase) return;

    auto fnRoInit = reinterpret_cast<FnRoInitialize>(GetProcAddress(combase, "RoInitialize"));
    auto fnRoGetFactory = reinterpret_cast<FnRoGetActivationFactory>(GetProcAddress(combase, "RoGetActivationFactory"));
    auto fnCreateStrRef = reinterpret_cast<FnWindowsCreateStringReference>(GetProcAddress(combase, "WindowsCreateStringReference"));
    if (!fnRoGetFactory || !fnCreateStrRef) return;

    if (!g_wgiRoInitDone && fnRoInit) {
        fnRoInit(1); // RO_INIT_MULTITHREADED
        g_wgiRoInitDone = true;
    }

    if (!g_wgiGamepadHooked) {
        static const wchar_t kClassName[] = L"Windows.Gaming.Input.Gamepad";
        WgiHStringHeader hdr{};
        WgiHString hstr = nullptr;
        if (SUCCEEDED(fnCreateStrRef(kClassName, static_cast<UINT32>(wcslen(kClassName)), &hdr, &hstr))) {
            void* statics = nullptr;
            if (SUCCEEDED(fnRoGetFactory(hstr, kIID_IGamepadStatics, &statics)) && statics) {
                void** svtbl = *reinterpret_cast<void***>(statics);
                auto fnGetGamepads = reinterpret_cast<FnWgiStaticsGetList>(svtbl[10]);
                void* vec = nullptr;
                if (fnGetGamepads && SUCCEEDED(fnGetGamepads(statics, &vec)) && vec) {
                    void** vvtbl = *reinterpret_cast<void***>(vec);
                    auto fnGetAt = reinterpret_cast<FnWgiVecGetAt>(vvtbl[6]);
                    auto fnGetSize = reinterpret_cast<FnWgiVecGetSize>(vvtbl[7]);
                    uint32_t count = 0;
                    if (fnGetSize && fnGetAt && SUCCEEDED(fnGetSize(vec, &count)) && count > 0) {
                        void* gp = nullptr;
                        if (SUCCEEDED(fnGetAt(vec, 0, &gp)) && gp) {
                            void** gvtbl = *reinterpret_cast<void***>(gp);
                            void* pGetReading = gvtbl[8];
                            if (MH_CreateHook(pGetReading,
                                              reinterpret_cast<void*>(&Hooked_WGI_Gamepad_GetCurrentReading),
                                              reinterpret_cast<void**>(&g_origWgiGamepadGetReading)) == MH_OK) {
                                MH_EnableHook(pGetReading);
                                g_wgiGamepadHooked = true;
                                CoreLog("Hooked Windows.Gaming.Input IGamepad::GetCurrentReading (background XInput bridge)");
                            }
                            static_cast<IUnknown*>(gp)->Release();
                        }
                    }
                    static_cast<IUnknown*>(vec)->Release();
                }
                static_cast<IUnknown*>(statics)->Release();
            }
        }
    }

    if (!g_wgiRawHooked) {
        static const wchar_t kClassName[] = L"Windows.Gaming.Input.RawGameController";
        WgiHStringHeader hdr{};
        WgiHString hstr = nullptr;
        if (SUCCEEDED(fnCreateStrRef(kClassName, static_cast<UINT32>(wcslen(kClassName)), &hdr, &hstr))) {
            void* statics = nullptr;
            if (SUCCEEDED(fnRoGetFactory(hstr, kIID_IRawGameControllerStatics, &statics)) && statics) {
                void** svtbl = *reinterpret_cast<void***>(statics);
                auto fnGetList = reinterpret_cast<FnWgiStaticsGetList>(svtbl[10]);
                void* vec = nullptr;
                if (fnGetList && SUCCEEDED(fnGetList(statics, &vec)) && vec) {
                    void** vvtbl = *reinterpret_cast<void***>(vec);
                    auto fnGetAt = reinterpret_cast<FnWgiVecGetAt>(vvtbl[6]);
                    auto fnGetSize = reinterpret_cast<FnWgiVecGetSize>(vvtbl[7]);
                    uint32_t count = 0;
                    if (fnGetSize && fnGetAt && SUCCEEDED(fnGetSize(vec, &count)) && count > 0) {
                        void* rgc = nullptr;
                        if (SUCCEEDED(fnGetAt(vec, 0, &rgc)) && rgc) {
                            void** rvtbl = *reinterpret_cast<void***>(rgc);
                            void* pGetReading = rvtbl[13];
                            if (MH_CreateHook(pGetReading,
                                              reinterpret_cast<void*>(&Hooked_WGI_RawGameController_GetCurrentReading),
                                              reinterpret_cast<void**>(&g_origWgiRawGetReading)) == MH_OK) {
                                MH_EnableHook(pGetReading);
                                g_wgiRawHooked = true;
                                CoreLog("Hooked Windows.Gaming.Input IRawGameController::GetCurrentReading (background XInput bridge)");
                            }
                            static_cast<IUnknown*>(rgc)->Release();
                        }
                    }
                    static_cast<IUnknown*>(vec)->Release();
                }
                static_cast<IUnknown*>(statics)->Release();
            }
        }
    }
}

void TryHookLazyInputModules() {
    TryHookXInput();
    TryHookDirectInput8();
    TryHookGameInput();
    TryHookWindowsGamingInput();
}

void ShutdownInputHooks() {
    if (g_wgiCacheInit) {
        DeleteCriticalSection(&g_wgiCacheCs);
        g_wgiCacheInit = false;
    }
    if (g_diCacheInit) {
        DeleteCriticalSection(&g_diCacheCs);
        g_diCacheInit = false;
    }
    if (g_rawSaveCsInit) {
        DeleteCriticalSection(&g_rawSaveCs);
        g_rawSaveCsInit = false;
    }
}

} // namespace padsplit::core
