#include "hooks_audio.h"
#include "state.h"

#include <mmsystem.h>
#include <dsound.h>
#include <MinHook.h>

namespace padsplit::core {

using FnDirectSoundCreate8 = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
using FnDSCreateSoundBuffer = HRESULT(STDMETHODCALLTYPE*)(IDirectSound8*, LPCDSBUFFERDESC, LPLPDIRECTSOUNDBUFFER, LPUNKNOWN);

static FnDSCreateSoundBuffer g_origDSCreateSoundBuffer = nullptr;
static bool g_dsoundHooked = false;

static HRESULT STDMETHODCALLTYPE Hooked_DSCreateSoundBuffer(IDirectSound8* self,
                                                            LPCDSBUFFERDESC pcDSBufferDesc,
                                                            LPLPDIRECTSOUNDBUFFER ppDSBuffer,
                                                            LPUNKNOWN pUnkOuter) {
    if (g_shm) shm_store32(&g_shm->detected_dsound, 1);

    if (pcDSBufferDesc && g_shm && !g_detaching &&
        shm_load32(&g_shm->enabled) && shm_load32(&g_shm->dsound_global_focus) &&
        !(pcDSBufferDesc->dwFlags & DSBCAPS_PRIMARYBUFFER) &&
        !(pcDSBufferDesc->dwFlags & DSBCAPS_GLOBALFOCUS) &&
        pcDSBufferDesc->dwSize <= sizeof(DSBUFFERDESC)) {
        DSBUFFERDESC patched = *pcDSBufferDesc;
        patched.dwFlags |= DSBCAPS_GLOBALFOCUS;
        return g_origDSCreateSoundBuffer(self, &patched, ppDSBuffer, pUnkOuter);
    }

    return g_origDSCreateSoundBuffer(self, pcDSBufferDesc, ppDSBuffer, pUnkOuter);
}

void TryHookLazyAudioModules() {
    if (g_dsoundHooked) return;
    HMODULE dsMod = GetModuleHandleW(L"dsound.dll");
    if (!dsMod) return;

    auto fnCreate = reinterpret_cast<FnDirectSoundCreate8>(GetProcAddress(dsMod, "DirectSoundCreate8"));
    if (!fnCreate) return;

    IDirectSound8* ds = nullptr;
    if (SUCCEEDED(fnCreate(nullptr, &ds, nullptr)) && ds) {
        void** vtbl = *reinterpret_cast<void***>(ds);
        void* pCreateBuf = vtbl[3]; // IDirectSound8::CreateSoundBuffer
        if (MH_CreateHook(pCreateBuf, reinterpret_cast<void*>(&Hooked_DSCreateSoundBuffer),
                          reinterpret_cast<void**>(&g_origDSCreateSoundBuffer)) == MH_OK) {
            MH_EnableHook(pCreateBuf);
            g_dsoundHooked = true;
            if (g_shm) shm_store32(&g_shm->detected_dsound, 1);
            CoreLog("Hooked DirectSound8::CreateSoundBuffer (DSBCAPS_GLOBALFOCUS)");
        }
        ds->Release();
    }
}

} // namespace padsplit::core
