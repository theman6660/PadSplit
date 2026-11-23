#include "hooks_display.h"
#include "state.h"

#include <d3d11.h>
#include <d3d9.h>
#include <dxgi.h>
#include <MinHook.h>

namespace padsplit::core {

static bool g_styleSaved = false;
static HWND g_styledHwnd = nullptr;
static LONG_PTR g_savedStyle = 0;
static LONG_PTR g_savedExStyle = 0;
static RECT g_savedRect = {};

static bool g_dxgiHooked = false;
static bool g_d3d9Hooked = false;

void ApplyWindowBorderless() {
    if (!g_shm || g_detaching || !g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    uint32_t mode = shm_load32(&g_shm->borderless_mode);
    if (mode == BORDERLESS_OFF) return;

    UpdateGameMonitorRect();
    RECT rc{};
    if (!GetWindowRect(g_gameHwnd, &rc)) return;

    LONG_PTR style = GetWindowLongPtrW(g_gameHwnd, GWL_STYLE);
    bool hasCaption = (style & (WS_CAPTION | WS_THICKFRAME | WS_BORDER)) != 0;
    bool coversMon = (rc.left <= g_gameMonRect.left && rc.top <= g_gameMonRect.top &&
                      rc.right >= g_gameMonRect.right && rc.bottom >= g_gameMonRect.bottom);

    if (!hasCaption && coversMon) {
        CoreLog("Window already borderless fullscreen; leaving untouched");
        return;
    }

    if (!g_styleSaved) {
        g_styledHwnd = g_gameHwnd;
        g_savedStyle = style;
        g_savedExStyle = GetWindowLongPtrW(g_gameHwnd, GWL_EXSTYLE);
        g_savedRect = rc;
        g_styleSaved = true;
    }

    LONG_PTR newStyle = style & ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER);
    LONG_PTR newExStyle = g_savedExStyle & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

    SetWindowLongPtrW(g_gameHwnd, GWL_STYLE, newStyle);
    SetWindowLongPtrW(g_gameHwnd, GWL_EXSTYLE, newExStyle);

    int monW = g_gameMonRect.right - g_gameMonRect.left;
    int monH = g_gameMonRect.bottom - g_gameMonRect.top;
    SetWindowPos(g_gameHwnd, HWND_TOP, g_gameMonRect.left, g_gameMonRect.top, monW, monH,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    shm_inc64(&g_shm->borderless_applied_count);
    CoreLog("Applied borderless window strip (%dx%d at %ld,%ld)", monW, monH, g_gameMonRect.left, g_gameMonRect.top);
}

void RestoreWindowBorderless() {
    if (!g_styleSaved || !g_styledHwnd || !IsWindow(g_styledHwnd)) {
        g_styleSaved = false;
        g_styledHwnd = nullptr;
        return;
    }
    SetWindowLongPtrW(g_styledHwnd, GWL_STYLE, g_savedStyle);
    SetWindowLongPtrW(g_styledHwnd, GWL_EXSTYLE, g_savedExStyle);
    SetWindowPos(g_styledHwnd, nullptr,
                 g_savedRect.left, g_savedRect.top,
                 g_savedRect.right - g_savedRect.left,
                 g_savedRect.bottom - g_savedRect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    g_styleSaved = false;
    g_styledHwnd = nullptr;
    CoreLog("Restored original window style");
}

// ---- DXGI Layer 2 Hook (only active when BORDERLESS_FORCE_DX) ----
using FnDXGISetFullscreenState = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
static FnDXGISetFullscreenState g_origDXGISetFullscreenState = nullptr;

static HRESULT STDMETHODCALLTYPE Hooked_DXGISetFullscreenState(IDXGISwapChain* self, BOOL Fullscreen, IDXGIOutput* pTarget) {
    if (g_shm) shm_store32(&g_shm->detected_dxgi, 1);
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) &&
        shm_load32(&g_shm->borderless_mode) == BORDERLESS_FORCE_DX && Fullscreen) {
        CoreLog("Blocked DXGI SetFullscreenState(TRUE) -> forcing borderless");
        return g_origDXGISetFullscreenState(self, FALSE, nullptr);
    }
    return g_origDXGISetFullscreenState(self, Fullscreen, pTarget);
}

static HWND CreateDummyHiddenWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_hModule;
    wc.lpszClassName = L"PadSplitDummyDX";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, g_hModule, nullptr);
}

static void DestroyDummyHiddenWindow(HWND hwnd) {
    if (hwnd) DestroyWindow(hwnd);
    UnregisterClassW(L"PadSplitDummyDX", g_hModule);
}

static void TryHookDXGI() {
    if (g_dxgiHooked) return;
    HMODULE dxgiMod = GetModuleHandleW(L"dxgi.dll");
    HMODULE d3d11Mod = GetModuleHandleW(L"d3d11.dll");
    if (!dxgiMod || !d3d11Mod) return;

    auto fnCreateDevAndSwap = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
        GetProcAddress(d3d11Mod, "D3D11CreateDeviceAndSwapChain"));
    if (!fnCreateDevAndSwap) return;

    HWND dummy = CreateDummyHiddenWindow();
    if (!dummy) return;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = fnCreateDevAndSwap(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr)) {
        hr = fnCreateDevAndSwap(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    }
    if (SUCCEEDED(hr) && sc) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        void* pSetFs = vtbl[10]; // IDXGISwapChain::SetFullscreenState
        if (MH_CreateHook(pSetFs, reinterpret_cast<void*>(&Hooked_DXGISetFullscreenState),
                          reinterpret_cast<void**>(&g_origDXGISetFullscreenState)) == MH_OK) {
            MH_EnableHook(pSetFs);
            g_dxgiHooked = true;
            if (g_shm) shm_store32(&g_shm->detected_dxgi, 1);
            CoreLog("Hooked DXGI IDXGISwapChain::SetFullscreenState");
        }
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc) sc->Release();
    DestroyDummyHiddenWindow(dummy);
}

// ---- D3D9 Layer 2 Hook (only active when BORDERLESS_FORCE_DX) ----
using FnD3D9CreateDevice = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using FnD3D9Reset = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using FnDirect3DCreate9 = IDirect3D9*(WINAPI*)(UINT);

static FnD3D9CreateDevice g_origD3D9CreateDevice = nullptr;
static FnD3D9Reset g_origD3D9Reset = nullptr;

static void ForceD3D9Windowed(D3DPRESENT_PARAMETERS* pp) {
    if (!pp) return;
    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        CoreLog("Forced D3D9 presentation parameters to Windowed");
    }
}

static HRESULT STDMETHODCALLTYPE Hooked_D3D9CreateDevice(IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                          HWND hFocusWindow, DWORD BehaviorFlags,
                                                          D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                          IDirect3DDevice9** ppReturnedDeviceInterface) {
    if (g_shm) shm_store32(&g_shm->detected_d3d9, 1);
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) &&
        shm_load32(&g_shm->borderless_mode) == BORDERLESS_FORCE_DX) {
        ForceD3D9Windowed(pPresentationParameters);
    }
    return g_origD3D9CreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
}

static HRESULT STDMETHODCALLTYPE Hooked_D3D9Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (g_shm) shm_store32(&g_shm->detected_d3d9, 1);
    if (g_shm && !g_detaching && shm_load32(&g_shm->enabled) &&
        shm_load32(&g_shm->borderless_mode) == BORDERLESS_FORCE_DX) {
        ForceD3D9Windowed(pPresentationParameters);
    }
    return g_origD3D9Reset(self, pPresentationParameters);
}

static void TryHookD3D9() {
    if (g_d3d9Hooked) return;
    HMODULE d3d9Mod = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9Mod) return;

    auto fnCreate9 = reinterpret_cast<FnDirect3DCreate9>(GetProcAddress(d3d9Mod, "Direct3DCreate9"));
    if (!fnCreate9) return;

    IDirect3D9* d3d9 = fnCreate9(D3D_SDK_VERSION);
    if (!d3d9) return;

    HWND dummy = CreateDummyHiddenWindow();
    if (!dummy) {
        d3d9->Release();
        return;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = dummy;

    IDirect3DDevice9* dev = nullptr;
    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev)) && dev) {
        void** vtblD3D9 = *reinterpret_cast<void***>(d3d9);
        void** vtblDev = *reinterpret_cast<void***>(dev);

        if (MH_CreateHook(vtblD3D9[16], reinterpret_cast<void*>(&Hooked_D3D9CreateDevice),
                          reinterpret_cast<void**>(&g_origD3D9CreateDevice)) == MH_OK) {
            MH_EnableHook(vtblD3D9[16]);
        }
        if (MH_CreateHook(vtblDev[16], reinterpret_cast<void*>(&Hooked_D3D9Reset),
                          reinterpret_cast<void**>(&g_origD3D9Reset)) == MH_OK) {
            MH_EnableHook(vtblDev[16]);
        }
        g_d3d9Hooked = true;
        if (g_shm) shm_store32(&g_shm->detected_d3d9, 1);
        CoreLog("Hooked D3D9 CreateDevice/Reset");
        dev->Release();
    }

    d3d9->Release();
    DestroyDummyHiddenWindow(dummy);
}

void TryHookLazyDisplayModules() {
    if (!g_shm || g_detaching) return;
    if (shm_load32(&g_shm->borderless_mode) != BORDERLESS_FORCE_DX) return;
    TryHookDXGI();
    TryHookD3D9();
}

} // namespace padsplit::core
