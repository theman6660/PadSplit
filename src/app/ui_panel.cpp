#include "ui_panel.h"
#include "../shared/version.h"

#include <d3d11.h>
#include <shellapi.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace padsplit::app {

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 101;
constexpr UINT WM_TRAY_NOTIFY = WM_APP + 102;
constexpr UINT WM_TRAY_SHOW_PANEL = WM_APP + 103;
constexpr UINT ID_TRAY_OPEN = 1001;
constexpr UINT ID_TRAY_ATTACH_FG = 1002;
constexpr UINT ID_TRAY_TOGGLE_PAUSE = 1003;
constexpr UINT ID_TRAY_EXIT = 1004;
constexpr UINT IDI_APP_ICON = 101;
constexpr ULONGLONG kUiPollIntervalMs = 400;

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static UiApp* g_uiInstance = nullptr;

static HICON CreateProgrammaticTrayIcon() {
    const int w = 16;
    const int h = 16;
    uint32_t pixels[w * h]{};
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            bool border = (x == 0 || x == w - 1 || y == 1 || y == h - 2);
            if (y == 0 || y == h - 1) {
                pixels[y * w + x] = 0x00000000;
            } else if (x == 7 || x == 8) {
                pixels[y * w + x] = 0xFF1E293B;
            } else if (border) {
                pixels[y * w + x] = 0xFF38BDF8;
            } else if (x < 7) {
                pixels[y * w + x] = 0xFF0EA5E9;
            } else {
                pixels[y * w + x] = 0xFF10B981;
            }
        }
    }

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -h;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (hColor && bits) {
        std::memcpy(bits, pixels, sizeof(pixels));
    }
    HBITMAP hMask = CreateBitmap(w, h, 1, 1, nullptr);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hColor;
    ii.hbmMask = hMask;
    HICON icon = CreateIconIndirect(&ii);
    if (hColor) DeleteObject(hColor);
    if (hMask) DeleteObject(hMask);
    return icon;
}

UiApp::~UiApp() {
    ClosePanel();
    DestroyTrayIcon();
    m_controller.Shutdown();
}

void UiApp::AddTrayIcon() {
    if (!m_trayHwnd) return;
    UiLang lang = m_controller.GetUiLang();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_trayHwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = m_trayIcon ? m_trayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    const wchar_t* tip = TrW(lang, L"PadSplit - Dual-Monitor Gamepad Splitter", L"PadSplit - 双屏手柄分身工具");
    wcsncpy(nid.szTip, tip, _countof(nid.szTip) - 1);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void UiApp::UpdateTrayTooltip() {
    if (!m_trayHwnd) return;
    UiLang lang = m_controller.GetUiLang();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_trayHwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    const wchar_t* tip = TrW(lang, L"PadSplit - Dual-Monitor Gamepad Splitter", L"PadSplit - 双屏手柄分身工具");
    wcsncpy(nid.szTip, tip, _countof(nid.szTip) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool UiApp::CreateTrayWindow(HINSTANCE hInst) {
    m_hInst = hInst;
    m_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"PadSplitTrayMsgWindow";
    RegisterClassExW(&wc);

    m_trayHwnd = CreateWindowExW(0, wc.lpszClassName, L"PadSplitTray", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!m_trayHwnd) return false;

    int smallW = GetSystemMetrics(SM_CXSMICON);
    int smallH = GetSystemMetrics(SM_CYSMICON);
    int bigW = GetSystemMetrics(SM_CXICON);
    int bigH = GetSystemMetrics(SM_CYICON);

    m_trayIcon = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, smallW, smallH, LR_DEFAULTCOLOR));
    m_appIconBig = static_cast<HICON>(LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, bigW, bigH, LR_DEFAULTCOLOR));
    if (!m_trayIcon) {
        m_trayIcon = CreateProgrammaticTrayIcon();
    }

    AddTrayIcon();
    return true;
}

void UiApp::DestroyTrayIcon() {
    if (m_trayHwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_trayHwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        DestroyWindow(m_trayHwnd);
        m_trayHwnd = nullptr;
    }
    if (m_trayIcon) {
        DestroyIcon(m_trayIcon);
        m_trayIcon = nullptr;
    }
    if (m_appIconBig) {
        DestroyIcon(m_appIconBig);
        m_appIconBig = nullptr;
    }
    UnregisterClassW(L"PadSplitTrayMsgWindow", m_hInst);
}

void UiApp::ShowTrayMenu() {
    AppConfig cfg = m_controller.GetConfigCopy();
    UiLang lang = ResolveUiLang(cfg.language);

    std::wstring hkAttachW = utf8_to_wide(cfg.hotkey_attach);
    std::wstring hkPauseW = utf8_to_wide(cfg.hotkey_toggle_pause);
    std::wstring labelAttach = std::wstring(TrW(lang, L"Attach/Detach Foreground Game", L"接管/卸载当前前台游戏")) +
                               L" (" + hkAttachW + L")";
    std::wstring labelPause = std::wstring(TrW(lang, L"Pause/Resume KBM Isolation", L"暂停/恢复键鼠隔离")) +
                              L" (" + hkPauseW + L")";

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, TrW(lang, L"Open Control Panel", L"打开控制面板"));
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_ATTACH_FG, labelAttach.c_str());
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_PAUSE, labelPause.c_str());
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, TrW(lang, L"Exit PadSplit", L"退出 PadSplit"));

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_trayHwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_trayHwnd, nullptr);
    PostMessageW(m_trayHwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

void UiApp::ShowTrayBalloon(const std::wstring& title, const std::wstring& body) {
    if (!m_trayHwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_trayHwnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    wcsncpy(nid.szInfoTitle, title.c_str(), _countof(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, body.c_str(), _countof(nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void UiApp::PostTrayNotification(const std::string& title, const std::string& body) {
    if (!m_trayHwnd) return;
    {
        std::lock_guard<std::mutex> lk(m_notifyMutex);
        m_pendingNotifyTitle = utf8_to_wide(title);
        m_pendingNotifyBody = utf8_to_wide(body);
    }
    PostMessageW(m_trayHwnd, WM_TRAY_NOTIFY, 0, 0);
}

bool UiApp::InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext))) {
        if (FAILED(D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevelArray, 2,
                D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext))) {
            return false;
        }
    }

    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
    return g_mainRenderTargetView != nullptr;
}

void UiApp::CleanupD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void UiApp::ResizeD3D(UINT w, UINT h) {
    if (!g_pSwapChain || w == 0 || h == 0) return;
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    g_pSwapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void UiApp::OpenPanel() {
    if (m_panelOpen && m_panelHwnd && IsWindow(m_panelHwnd)) {
        ShowWindow(m_panelHwnd, SW_RESTORE);
        SetForegroundWindow(m_panelHwnd);
        m_forceRefresh = true;
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = m_hInst;
    wc.hIcon = m_appIconBig ? m_appIconBig : m_trayIcon;
    wc.hIconSm = m_trayIcon;
    wc.lpszClassName = L"PadSplitPanelWindow";
    RegisterClassExW(&wc);

    UiLang lang = m_controller.GetUiLang();
    m_lastAppliedTitleLang = lang;
    const wchar_t* winTitle = TrW(lang, L"PadSplit Control Panel", L"PadSplit 控制面板 (双屏手柄分身)");

    m_panelHwnd = CreateWindowExW(
        0, wc.lpszClassName, winTitle,
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 860, 600,
        nullptr, nullptr, m_hInst, nullptr);
    if (!m_panelHwnd || !InitD3D(m_panelHwnd)) {
        CleanupD3D();
        if (m_panelHwnd) { DestroyWindow(m_panelHwnd); m_panelHwnd = nullptr; }
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.ChildRounding = 5.0f;
    style.GrabRounding = 4.0f;

    // Load crisp Latin UI font (Segoe UI) present on all Windows locales, then merge CJK font if installed
    bool baseLoaded = false;
    m_hasCjkFont = false;
    const char* latinFont = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (std::filesystem::exists(latinFont)) {
        if (io.Fonts->AddFontFromFileTTF(latinFont, 17.5f, nullptr, io.Fonts->GetGlyphRangesDefault())) {
            baseLoaded = true;
        }
    }

    const char* cjkCandidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc"
    };
    for (const char* fp : cjkCandidates) {
        if (std::filesystem::exists(fp)) {
            ImFontConfig cfg;
            cfg.MergeMode = baseLoaded;
            if (io.Fonts->AddFontFromFileTTF(fp, 17.0f, &cfg, io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
                baseLoaded = true;
                m_hasCjkFont = true;
                break;
            }
        }
    }
    if (!baseLoaded) {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplWin32_Init(m_panelHwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ShowWindow(m_panelHwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_panelHwnd);
    m_editCfg = m_controller.GetConfigCopy();
    m_autoStartEnabled = IsAutoStartEnabled();
    std::strncpy(m_hkAttachBuf, m_editCfg.hotkey_attach.c_str(), sizeof(m_hkAttachBuf) - 1);
    m_hkAttachBuf[sizeof(m_hkAttachBuf) - 1] = '\0';
    std::strncpy(m_hkPauseBuf, m_editCfg.hotkey_toggle_pause.c_str(), sizeof(m_hkPauseBuf) - 1);
    m_hkPauseBuf[sizeof(m_hkPauseBuf) - 1] = '\0';
    m_pendingClosePanel = false;
    m_forceRefresh = true;
    m_panelOpen = true;
}

void UiApp::ClosePanel() {
    if (!m_panelOpen) return;
    m_panelOpen = false;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    if (m_panelHwnd) {
        DestroyWindow(m_panelHwnd);
        m_panelHwnd = nullptr;
    }
    UnregisterClassW(L"PadSplitPanelWindow", m_hInst);
}

void UiApp::RefreshCachedData(bool force) {
    ULONGLONG now = GetTickCount64();
    if (!force && (now - m_lastPollTick) < kUiPollIntervalMs) {
        return;
    }
    m_lastPollTick = now;
    m_forceRefresh = false;
    m_cachedSessions = m_controller.GetAttachedSessions();
    m_cachedCandidates = m_controller.GetWindowedCandidates(!m_showAllProcesses);
}

void UiApp::RenderPanelFrame() {
    if (!m_panelOpen || !g_pd3dDeviceContext || !g_mainRenderTargetView) return;

    RefreshCachedData(m_forceRefresh);

    AppConfig liveCfg = m_controller.GetConfigCopy();
    UiLang lang = ResolveUiLang(liveCfg.language);
    if (lang != m_lastAppliedTitleLang && m_panelHwnd) {
        m_lastAppliedTitleLang = lang;
        SetWindowTextW(m_panelHwnd, TrW(lang, L"PadSplit Control Panel", L"PadSplit 控制面板 (双屏手柄分身)"));
        UpdateTrayTooltip();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("PadSplitMain", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    const auto& sessions = m_cachedSessions;
    ImGui::Text("%s: %s   |   %s: %zu",
                Tr(lang, "Status", "状态"),
                m_controller.GetLastStatusMessage().c_str(),
                Tr(lang, "Attached", "已接管进程数"),
                sessions.size());
    ImGui::Separator();

    if (ImGui::BeginTabBar("PadSplitTabs")) {
        // Tab 1: Status & Processes
        if (ImGui::BeginTabItem(Tr(lang, "Status##tab1", "运行状态##tab1"))) {
            std::string pauseBtnLabel = std::string(Tr(lang, "Pause/Resume All Isolation", "暂停/恢复全部隔离")) +
                                        " (" + liveCfg.hotkey_toggle_pause + ")##pauseall";
            if (ImGui::Button(pauseBtnLabel.c_str())) {
                std::string msg;
                m_controller.TogglePauseAll(msg);
                m_forceRefresh = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(Tr(lang, "Refresh##ref", "刷新列表##ref"))) {
                m_forceRefresh = true;
            }
            ImGui::SameLine();
            if (ImGui::Checkbox(Tr(lang, "Show all desktop windows", "显示全部桌面窗口"), &m_showAllProcesses)) {
                m_forceRefresh = true;
            }

            ImGui::Spacing();
            ImGui::TextUnformatted(Tr(lang, "Attached Game Sessions:", "已接管的游戏会话:"));
            if (sessions.empty()) {
                ImGui::TextDisabled(
                    Tr(lang,
                       "  No active sessions. Focus a game and press %s, or click Attach below.",
                       "  暂无已接管游戏。可在游戏窗口按 %s 一键接管，或从下方列表手动接管。"),
                    liveCfg.hotkey_attach.c_str());
            } else {
                for (const auto& s : sessions) {
                    ImGui::PushID(static_cast<int>(s.pid));
                    float cardH = s.state.detected_steam_overlay ? 114.0f : 90.0f;
                    ImGui::BeginChild("SessCard", ImVec2(0, cardH), true);
                    ImGui::Text("%s  (PID=%u, %u-bit)  |  %s  |  %s: %s",
                                s.exe_name.c_str(), s.pid, s.state.core_arch,
                                s.state.enabled ? Tr(lang, "Active", "隔离运行中") : Tr(lang, "Paused", "已暂停隔离"),
                                Tr(lang, "Mode", "模式"),
                                s.state.isolation_mode == ISO_BLOCK_OUTSIDE_GAME
                                    ? Tr(lang, "Border-Crossing", "越界隔离")
                                    : Tr(lang, "Gamepad-Only", "手柄独占"));
                    ImGui::Text("%s: %s%s%s%s | %s=%llu | %s=%llu | %s=%llu",
                                Tr(lang, "Input", "通道"),
                                s.state.detected_xinput ? "XInput " : "",
                                s.state.detected_dinput_pad ? "DInput " : "",
                                s.state.detected_rawinput_pad ? "RawInput " : "",
                                s.state.detected_gameinput ? "GameInput " : "",
                                Tr(lang, "FocusSpoof", "前台欺骗"),
                                static_cast<unsigned long long>(s.state.focus_api_spoofs),
                                Tr(lang, "KbmBlocked", "屏蔽键鼠"),
                                static_cast<unsigned long long>(s.state.blocked_kbm_polls + s.state.blocked_raw_kbm + s.state.blocked_dinput_kbm + s.state.blocked_gameinput_kbm),
                                Tr(lang, "ClipReleased", "释放Clip"),
                                static_cast<unsigned long long>(s.state.blocked_clip_cursor));
                    if (s.state.detected_steam_overlay) {
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.78f, 0.25f, 1.0f),
                            "%s",
                            Tr(lang,
                               "Tip: Steam Overlay detected. If your controller switches to Desktop layout on Monitor 2, disable Steam Input for this game.",
                               "提示: 检测到 Steam 覆盖层。若鼠标去副屏后手柄变成桌面模式，请在 Steam 属性中关闭该游戏的 Steam Input。"));
                    }
                    if (ImGui::Button(s.state.enabled ? Tr(lang, "Pause", "暂停隔离") : Tr(lang, "Resume", "恢复隔离"))) {
                        std::string err;
                        uint32_t next = s.state.enabled ? 0u : 1u;
                        MutateProcessShm(s.pid, [next](SharedState* shm) { shm_store32(&shm->enabled, next); }, err);
                        m_forceRefresh = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(Tr(lang, "Toggle Mode", "切换越界/独占模式"))) {
                        std::string err;
                        uint32_t nextMode = (s.state.isolation_mode == ISO_BLOCK_OUTSIDE_GAME)
                                                ? static_cast<uint32_t>(ISO_ALWAYS_BLOCK_KBM)
                                                : static_cast<uint32_t>(ISO_BLOCK_OUTSIDE_GAME);
                        MutateProcessShm(s.pid, [nextMode](SharedState* shm) { shm_store32(&shm->isolation_mode, nextMode); }, err);
                        m_forceRefresh = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(Tr(lang, "Re-apply Borderless", "重新应用无边框"))) {
                        std::string err;
                        MutateProcessShm(s.pid, [](SharedState* shm) {
                            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->apply_borderless_seq));
                        }, err);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(Tr(lang, "Detach", "卸载释放"))) {
                        std::string msg;
                        m_controller.DetachPid(s.pid, msg);
                        m_forceRefresh = true;
                    }
                    ImGui::EndChild();
                    ImGui::PopID();
                }
            }

            ImGui::Spacing();
            ImGui::TextUnformatted(Tr(lang, "Available Windowed Processes:", "可接管的窗口进程:"));
            const auto& candidates = m_cachedCandidates;
            if (ImGui::BeginTable("WinTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 0))) {
                ImGui::TableSetupColumn(Tr(lang, "Executable", "程序"), ImGuiTableColumnFlags_WidthFixed, 170);
                ImGui::TableSetupColumn(Tr(lang, "PID / Arch", "PID/架构"), ImGuiTableColumnFlags_WidthFixed, 105);
                ImGui::TableSetupColumn(Tr(lang, "Display", "显示"), ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn(Tr(lang, "Window Title", "窗口标题"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(Tr(lang, "Action", "操作"), ImGuiTableColumnFlags_WidthFixed, 85);
                ImGui::TableHeadersRow();

                for (const auto& c : candidates) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(c.exe_name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u (%u-bit)", c.pid, c.arch);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("M%d %s", c.monitor_index, c.display_mode.c_str());
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextUnformatted(c.window_title.c_str());
                    ImGui::TableSetColumnIndex(4);
                    ImGui::PushID(static_cast<int>(c.pid));
                    if (c.attached) {
                        if (ImGui::SmallButton(Tr(lang, "Detach", "卸载"))) {
                            std::string msg;
                            m_controller.DetachPid(c.pid, msg);
                            m_forceRefresh = true;
                        }
                    } else {
                        if (ImGui::SmallButton(Tr(lang, "Attach", "接管"))) {
                            AttachOptions opts = m_controller.BuildAttachOptionsForExe(c.exe_name);
                            std::string msg;
                            m_controller.AttachPid(c.pid, opts, true, msg);
                            m_forceRefresh = true;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }

        // Tab 2: Whitelist Profiles
        if (ImGui::BeginTabItem(Tr(lang, "Whitelist##tab2", "白名单配置##tab2"))) {
            AppConfig cfg = m_controller.GetConfigCopy();
            bool changed = false;
            int removeIdx = -1;

            if (cfg.whitelist.empty()) {
                ImGui::TextDisabled(
                    Tr(lang,
                       "Whitelist is empty. Attaching a game once (via %s or the Status tab) adds it here automatically.",
                       "白名单为空。用 %s 接管一次游戏后会自动加入白名单。"),
                    cfg.hotkey_attach.c_str());
            }
            for (size_t i = 0; i < cfg.whitelist.size(); ++i) {
                auto& gp = cfg.whitelist[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::BeginChild("WlItem", ImVec2(0, 84), true);
                ImGui::Text("%s: %s", Tr(lang, "Exe", "程序"), gp.exe_name.c_str());
                ImGui::SameLine();
                if (ImGui::Checkbox(Tr(lang, "Auto-Attach", "自动接管"), &gp.auto_attach)) changed = true;
                ImGui::SameLine();
                if (ImGui::Checkbox(Tr(lang, "Hide Idle Cursor", "静止隐藏光标"), &gp.hide_idle_cursor)) changed = true;
                ImGui::SameLine();
                if (ImGui::Checkbox(Tr(lang, "DSound Global", "DSound保活"), &gp.dsound_global_focus)) changed = true;
                ImGui::SameLine();
                if (ImGui::SmallButton(Tr(lang, "Remove", "删除"))) removeIdx = static_cast<int>(i);

                int iso = static_cast<int>(gp.isolation_mode);
                const char* isoItemsEn[] = {"Gamepad-Only (FPS / 3D)", "Border-Crossing (2D / Strategy)"};
                const char* isoItemsZh[] = {"手柄独占隔离 (推荐FPS/3D)", "越界隔离 (2D/策略)"};
                ImGui::SetNextItemWidth(240);
                if (ImGui::Combo(Tr(lang, "Isolation", "隔离模式"), &iso, lang == UiLang::Zh ? isoItemsZh : isoItemsEn, 2)) {
                    gp.isolation_mode = static_cast<IsolationMode>(iso);
                    changed = true;
                }
                ImGui::SameLine();
                int bdr = static_cast<int>(gp.borderless_mode);
                const char* bdrItemsEn[] = {"Off", "Window Strip (Default)", "Force DX Borderless"};
                const char* bdrItemsZh[] = {"关闭", "窗口去边框 (默认)", "强制DX无边框"};
                ImGui::SetNextItemWidth(190);
                if (ImGui::Combo(Tr(lang, "Fullscreen", "全屏策略"), &bdr, lang == UiLang::Zh ? bdrItemsZh : bdrItemsEn, 3)) {
                    gp.borderless_mode = static_cast<BorderlessMode>(bdr);
                    changed = true;
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
            if (removeIdx >= 0) {
                cfg.whitelist.erase(cfg.whitelist.begin() + removeIdx);
                changed = true;
            }
            if (changed) {
                m_controller.SetConfig(cfg);
            }
            ImGui::EndTabItem();
        }

        // Tab 3: Live Diagnostics & Logs
        if (ImGui::BeginTabItem(Tr(lang, "Diagnostics##tab3", "实时诊断日志##tab3"))) {
            if (sessions.empty()) {
                ImGui::TextDisabled("%s", Tr(lang, "No attached game processes.", "当前没有已接管的游戏进程。"));
            } else {
                if (m_selectedLogIdx < 0 || m_selectedLogIdx >= static_cast<int>(sessions.size())) {
                    m_selectedLogIdx = 0;
                }

                // Session selector combo
                std::string curLabel = sessions[static_cast<size_t>(m_selectedLogIdx)].exe_name +
                                       " (PID=" + std::to_string(sessions[static_cast<size_t>(m_selectedLogIdx)].pid) + ")";
                ImGui::SetNextItemWidth(280);
                if (ImGui::BeginCombo(Tr(lang, "Session", "目标会话"), curLabel.c_str())) {
                    for (size_t i = 0; i < sessions.size(); ++i) {
                        std::string itemLabel = sessions[i].exe_name + " (PID=" + std::to_string(sessions[i].pid) + ")";
                        bool isSel = (static_cast<int>(i) == m_selectedLogIdx);
                        if (ImGui::Selectable(itemLabel.c_str(), isSel)) {
                            m_selectedLogIdx = static_cast<int>(i);
                        }
                        if (isSel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                const auto& sel = sessions[static_cast<size_t>(m_selectedLogIdx)];

                ImGui::SameLine();
                if (ImGui::Button(Tr(lang, "Copy Logs", "复制日志"))) {
                    std::string allLogs;
                    uint32_t total = sel.state.log_write_seq;
                    uint32_t start = (total > PADSPLIT_LOG_SLOTS) ? (total - PADSPLIT_LOG_SLOTS) : 0;
                    for (uint32_t seq = start; seq < total; ++seq) {
                        const char* line = sel.state.log_ring[seq % PADSPLIT_LOG_SLOTS];
                        if (line[0] != '\0') {
                            allLogs += line;
                            allLogs += "\n";
                        }
                    }
                    ImGui::SetClipboardText(allLogs.c_str());
                }
                ImGui::SameLine();
                ImGui::Checkbox(Tr(lang, "Auto-scroll", "自动滚底"), &m_logAutoScroll);

                ImGui::Text("PID=%u (%u-bit)  Steam=%s  GameInput=%s(bg=%llu/blk=%llu)  DXGI=%s  D3D9=%s  DSound=%s",
                            sel.pid, sel.state.core_arch,
                            sel.state.detected_steam_overlay ? "Yes" : "No",
                            sel.state.detected_gameinput ? "Yes" : "No",
                            static_cast<unsigned long long>(sel.state.gameinput_focus_patched),
                            static_cast<unsigned long long>(sel.state.blocked_gameinput_kbm),
                            sel.state.detected_dxgi ? "Yes" : "No",
                            sel.state.detected_d3d9 ? "Yes" : "No",
                            sel.state.detected_dsound ? "Yes" : "No");

                ImGui::BeginChild("LogScroll", ImVec2(0, 0), true);
                uint32_t total = sel.state.log_write_seq;
                uint32_t start = (total > PADSPLIT_LOG_SLOTS) ? (total - PADSPLIT_LOG_SLOTS) : 0;
                for (uint32_t seq = start; seq < total; ++seq) {
                    const char* line = sel.state.log_ring[seq % PADSPLIT_LOG_SLOTS];
                    if (line[0] != '\0') ImGui::TextUnformatted(line);
                }
                if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
                    ImGui::SetScrollHereY(1.0f);
                }
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }

        // Tab 4: Global Settings
        if (ImGui::BeginTabItem(Tr(lang, "Settings##tab4", "全局设置##tab4"))) {
            // Language switcher (applies immediately)
            int langIdx = (m_editCfg.language == "en") ? 1 : ((m_editCfg.language == "zh") ? 2 : 0);
            const char* langItems[] = {"Auto (System)", "English", "简体中文"};
            ImGui::SetNextItemWidth(200);
            if (ImGui::Combo(Tr(lang, "UI Language", "界面语言"), &langIdx, langItems, 3)) {
                m_editCfg.language = (langIdx == 1) ? "en" : ((langIdx == 2) ? "zh" : "auto");
                AppConfig latest = m_controller.GetConfigCopy();
                latest.language = m_editCfg.language;
                m_controller.SetConfig(latest);
            }
            if (!m_hasCjkFont && ResolveUiLang(m_editCfg.language) == UiLang::Zh) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), "(CJK system font not installed)");
            }

            ImGui::Separator();

            if (ImGui::Checkbox(Tr(lang,
                                   "Launch minimized to system tray on Windows startup",
                                   "开机自动静默启动到系统托盘（推荐，无弹窗不闪黑框）"),
                                &m_autoStartEnabled)) {
                SetAutoStartEnabled(m_autoStartEnabled);
            }

            ImGui::Separator();
            ImGui::SetNextItemWidth(220);
            ImGui::InputText(Tr(lang, "Attach / Detach Hotkey", "接管/卸载当前窗口热键"), m_hkAttachBuf, sizeof(m_hkAttachBuf));
            ImGui::SameLine();
            if (m_controller.IsAttachHotkeyOk()) {
                ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.50f, 1.0f), "%s", Tr(lang, "[Registered]", "[已注册]"));
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", Tr(lang, "[Conflict / Invalid]", "[冲突或无效]"));
            }

            ImGui::SetNextItemWidth(220);
            ImGui::InputText(Tr(lang, "Pause / Resume Hotkey", "暂停/恢复键鼠隔离热键"), m_hkPauseBuf, sizeof(m_hkPauseBuf));
            ImGui::SameLine();
            if (m_controller.IsPauseHotkeyOk()) {
                ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.50f, 1.0f), "%s", Tr(lang, "[Registered]", "[已注册]"));
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", Tr(lang, "[Conflict / Invalid]", "[冲突或无效]"));
            }

            ImGui::Checkbox(Tr(lang, "Start minimized to tray on launch", "启动时静默缩在系统托盘（不主动弹主界面）"), &m_editCfg.start_minimized);
            ImGui::Checkbox(Tr(lang, "Hide idle cursor on game monitor by default", "默认启用主屏静止隐藏光标"), &m_editCfg.default_hide_idle_cursor);
            int idleMs = static_cast<int>(m_editCfg.default_idle_cursor_ms);
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderInt(Tr(lang, "Idle cursor delay (ms)", "静止隐藏延迟 (毫秒)"), &idleMs, 300, 5000)) {
                m_editCfg.default_idle_cursor_ms = static_cast<uint32_t>(idleMs);
            }

            ImGui::Spacing();
            if (ImGui::Button(Tr(lang, "Save Settings", "保存全局设置"))) {
                AppConfig latest = m_controller.GetConfigCopy();
                latest.language = m_editCfg.language;
                latest.hotkey_attach = m_hkAttachBuf;
                latest.hotkey_toggle_pause = m_hkPauseBuf;
                latest.start_minimized = m_editCfg.start_minimized;
                latest.default_hide_idle_cursor = m_editCfg.default_hide_idle_cursor;
                latest.default_idle_cursor_ms = m_editCfg.default_idle_cursor_ms;
                m_controller.SetConfig(latest);
                m_editCfg = latest;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("PadSplit v%s  |  Single-player offline games only (never bypasses kernel anti-cheat)", PADSPLIT_VERSION_STR);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::Render();

    const float clearColor[4] = {0.09f, 0.10f, 0.12f, 1.0f};
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
}

LRESULT CALLBACK UiApp::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_uiInstance) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (g_uiInstance->m_wmTaskbarCreated != 0 && msg == g_uiInstance->m_wmTaskbarCreated) {
        g_uiInstance->AddTrayIcon();
        return 0;
    }

    if (msg == WM_TRAY_CALLBACK) {
        if (lParam == WM_LBUTTONDBLCLK) {
            g_uiInstance->OpenPanel();
        } else if (lParam == WM_RBUTTONUP) {
            g_uiInstance->ShowTrayMenu();
        }
        return 0;
    }
    if (msg == WM_TRAY_NOTIFY) {
        std::wstring title, body;
        {
            std::lock_guard<std::mutex> lk(g_uiInstance->m_notifyMutex);
            title = g_uiInstance->m_pendingNotifyTitle;
            body = g_uiInstance->m_pendingNotifyBody;
        }
        if (!title.empty() || !body.empty()) {
            g_uiInstance->ShowTrayBalloon(title, body);
        }
        return 0;
    }
    if (msg == WM_TRAY_SHOW_PANEL) {
        g_uiInstance->OpenPanel();
        return 0;
    }
    if (msg == WM_TRAY_RELOAD_HOTKEYS) {
        g_uiInstance->m_controller.RegisterGlobalHotkeys();
        return 0;
    }
    if (msg == WM_HOTKEY) {
        g_uiInstance->m_controller.HandleHotkey(static_cast<int>(wParam));
        g_uiInstance->m_forceRefresh = true;
        return 0;
    }
    if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case ID_TRAY_OPEN:
            g_uiInstance->OpenPanel();
            break;
        case ID_TRAY_ATTACH_FG:
            g_uiInstance->m_controller.HandleHotkey(HK_ATTACH_FOREGROUND);
            g_uiInstance->m_forceRefresh = true;
            break;
        case ID_TRAY_TOGGLE_PAUSE:
            g_uiInstance->m_controller.HandleHotkey(HK_TOGGLE_PAUSE);
            g_uiInstance->m_forceRefresh = true;
            break;
        case ID_TRAY_EXIT:
            g_uiInstance->m_exiting = true;
            g_uiInstance->ClosePanel();
            PostQuitMessage(0);
            break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK UiApp::PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
        return true;
    }
    if (!g_uiInstance) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_uiInstance->ResizeD3D(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_CLOSE:
        g_uiInstance->m_pendingClosePanel = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int UiApp::Run(HINSTANCE hInst, bool startMinimized, bool forceShow) {
    g_uiInstance = this;
    if (!CreateTrayWindow(hInst)) return 1;
    m_controller.SetNotifyCallback([this](const std::string& title, const std::string& body) {
        PostTrayNotification(title, body);
    });
    m_controller.Init(m_trayHwnd);
    UpdateTrayTooltip();

    AppConfig cfg = m_controller.GetConfigCopy();
    if (forceShow || (!startMinimized && !cfg.start_minimized)) {
        OpenPanel();
    }

    MSG msg{};
    while (!m_exiting) {
        if (m_panelOpen) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    m_exiting = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (m_pendingClosePanel) {
                m_pendingClosePanel = false;
                ClosePanel();
            } else if (!m_exiting && m_panelOpen) {
                RenderPanelFrame();
            } else {
                Sleep(16);
            }
        } else {
            BOOL r = GetMessageW(&msg, nullptr, 0, 0);
            if (r <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    m_controller.Shutdown();
    g_uiInstance = nullptr;
    return 0;
}

} // namespace padsplit::app
