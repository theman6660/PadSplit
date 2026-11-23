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
    return page == 0x01 && (usage == 0x04 || usage == 0x05);
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

void TryHookLazyInputModules() {
    TryHookXInput();
    TryHookDirectInput8();
    TryHookGameInput();
}

void ShutdownInputHooks() {
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
