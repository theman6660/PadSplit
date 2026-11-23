#include "controller.h"
#include "../shared/anticheat.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace padsplit::app {

Controller::~Controller() {
    Shutdown();
}

bool Controller::Init(HWND msgHwnd) {
    m_msgHwnd = msgHwnd;
    m_uiThreadId = GetCurrentThreadId();
    ReloadConfig();
    if (!std::filesystem::exists(std::filesystem::path(GetDefaultConfigPath()))) {
        SaveCurrentConfig();
    }
    SetStatusMessage(Tr(GetUiLang(), "Ready", "就绪"));
    RegisterGlobalHotkeys();
    m_running = true;
    m_autoThread = std::thread(&Controller::AutoAttachLoop, this);
    m_pipeThread = std::thread(&Controller::PipeServerLoop, this);
    return true;
}

void Controller::Shutdown() {
    if (!m_running.exchange(false)) return;

    UnregisterGlobalHotkeys();

    // Wake up CreateNamedPipe/ConnectNamedPipe if blocked by opening a brief client connection
    HANDLE hWake = CreateFileW(PADSPLIT_CONTROL_PIPE, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hWake != INVALID_HANDLE_VALUE) {
        CloseHandle(hWake);
    }

    if (m_autoThread.joinable()) m_autoThread.join();
    if (m_pipeThread.joinable()) m_pipeThread.join();

    // Cleanly detach all attached games on exit
    auto pids = ListAttachedPids();
    for (uint32_t pid : pids) {
        std::string err;
        DetachProcess(pid, err);
    }
}

void Controller::ReloadConfig() {
    AppConfig loaded;
    LoadConfig(loaded);
    {
        std::lock_guard<std::mutex> lk(m_cfgMutex);
        m_config = std::move(loaded);
    }
    {
        std::lock_guard<std::mutex> lk(m_autoStateMutex);
        m_failedAutoPids.clear();
    }
    RequestHotkeyRebind();
}

bool Controller::SaveCurrentConfig() {
    AppConfig copy = GetConfigCopy();
    return SaveConfig(copy);
}

void Controller::RequestHotkeyRebind() {
    if (!m_msgHwnd) return;
    if (GetCurrentThreadId() == m_uiThreadId) {
        RegisterGlobalHotkeys();
    } else {
        PostMessageW(m_msgHwnd, WM_TRAY_RELOAD_HOTKEYS, 0, 0);
    }
}

void Controller::RegisterGlobalHotkeys() {
    if (!m_msgHwnd) return;
    UnregisterGlobalHotkeys();
    AppConfig cfg = GetConfigCopy();

    HotkeySpec hk1, hk2;
    bool ok1 = false;
    bool ok2 = false;
    if (ParseHotkeyString(cfg.hotkey_attach, hk1)) {
        ok1 = (RegisterHotKey(m_msgHwnd, HK_ATTACH_FOREGROUND, hk1.modifiers | MOD_NOREPEAT, hk1.vk) != FALSE);
    }
    if (ParseHotkeyString(cfg.hotkey_toggle_pause, hk2)) {
        ok2 = (RegisterHotKey(m_msgHwnd, HK_TOGGLE_PAUSE, hk2.modifiers | MOD_NOREPEAT, hk2.vk) != FALSE);
    }
    m_hotkeyAttachOk.store(ok1);
    m_hotkeyPauseOk.store(ok2);

    if (!ok1 || !ok2) {
        UiLang lang = ResolveUiLang(cfg.language);
        SetStatusMessage(Tr(
            lang,
            "Warning: one or more global hotkeys failed to register (invalid or in use)",
            "警告：部分全局热键注册失败（格式无效或已被其他软件占用）"));
    }
}

void Controller::UnregisterGlobalHotkeys() {
    if (!m_msgHwnd) return;
    UnregisterHotKey(m_msgHwnd, HK_ATTACH_FOREGROUND);
    UnregisterHotKey(m_msgHwnd, HK_TOGGLE_PAUSE);
}

void Controller::SetStatusMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_statusMutex);
    m_lastStatus = msg;
}

void Controller::SetNotifyCallback(std::function<void(const std::string&, const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(m_statusMutex);
    m_notifyCb = std::move(cb);
}

void Controller::EmitNotify(const std::string& title, const std::string& body) {
    std::function<void(const std::string&, const std::string&)> cb;
    {
        std::lock_guard<std::mutex> lk(m_statusMutex);
        cb = m_notifyCb;
    }
    if (cb) cb(title, body);
}

std::string Controller::GetLastStatusMessage() {
    std::lock_guard<std::mutex> lk(m_statusMutex);
    return m_lastStatus;
}

UiLang Controller::GetUiLang() {
    AppConfig cfg = GetConfigCopy();
    return ResolveUiLang(cfg.language);
}

AttachOptions Controller::BuildAttachOptionsForExe(const std::string& exeName) {
    AppConfig cfg = GetConfigCopy();
    AttachOptions opts;
    opts.hide_idle_cursor = cfg.default_hide_idle_cursor;
    opts.idle_cursor_ms = cfg.default_idle_cursor_ms;

    if (!exeName.empty()) {
        if (const auto* prof = FindWhitelistProfile(cfg, exeName)) {
            opts.isolation_mode = prof->isolation_mode;
            opts.borderless_mode = prof->borderless_mode;
            opts.hide_idle_cursor = prof->hide_idle_cursor;
            opts.idle_cursor_ms = prof->idle_cursor_ms;
            opts.dsound_global_focus = prof->dsound_global_focus;
        }
    }
    return opts;
}

void Controller::HandleHotkey(int id) {
    if (id == HK_ATTACH_FOREGROUND) {
        HWND fg = GetForegroundWindow();
        if (!fg) return;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == 0 || pid == GetCurrentProcessId()) return;

        auto attached = ListAttachedPids();
        bool already = std::find(attached.begin(), attached.end(), pid) != attached.end();
        std::string msg;
        if (already) {
            DetachPid(pid, msg);
        } else {
            std::string exeName = GetExeBaseNameUtf8(GetProcessImagePath(pid));
            AttachOptions opts = BuildAttachOptionsForExe(exeName);
            AttachPid(pid, opts, true, msg);
        }
        SetStatusMessage(msg);
    } else if (id == HK_TOGGLE_PAUSE) {
        std::string msg;
        TogglePauseAll(msg);
        SetStatusMessage(msg);
    }
}

bool Controller::AttachPid(uint32_t pid, const AttachOptions& opts, bool addToWhitelist, std::string& outMsg) {
    UiLang lang = GetUiLang();
    {
        std::lock_guard<std::mutex> lk(m_autoStateMutex);
        m_manuallyDetachedPids.erase(pid);
    }

    std::string err;
    if (!AttachProcess(pid, opts, err)) {
        outMsg = std::string(Tr(lang, "Attach failed: ", "接管失败: ")) + err;
        SetStatusMessage(outMsg);
        EmitNotify(Tr(lang, "PadSplit Attach Failed", "PadSplit 接管失败"), err);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_autoStateMutex);
        m_failedAutoPids.erase(pid);
    }

    std::string exeName = GetExeBaseNameUtf8(GetProcessImagePath(pid));
    if (addToWhitelist && !exeName.empty()) {
        std::lock_guard<std::mutex> lk(m_cfgMutex);
        if (!FindWhitelistProfile(m_config, exeName)) {
            GameProfile gp;
            gp.exe_name = exeName;
            gp.auto_attach = true;
            gp.isolation_mode = opts.isolation_mode;
            gp.borderless_mode = opts.borderless_mode;
            gp.hide_idle_cursor = opts.hide_idle_cursor;
            gp.idle_cursor_ms = opts.idle_cursor_ms;
            gp.dsound_global_focus = opts.dsound_global_focus;
            UpsertWhitelistProfile(m_config, gp);
            SaveConfig(m_config);
        }
    }

    outMsg = std::string(Tr(lang, "Attached to ", "已接管 ")) + exeName + " (PID=" + std::to_string(pid) + ")";
    SetStatusMessage(outMsg);
    EmitNotify(
        Tr(lang, "PadSplit Attached", "PadSplit 已接管游戏"),
        exeName + Tr(lang, " — mouse is free for secondary monitor while gamepad stays active",
                     " — 鼠标可直接滑往副屏，手柄保持操控"));
    return true;
}

bool Controller::DetachPid(uint32_t pid, std::string& outMsg) {
    UiLang lang = GetUiLang();
    std::string err;
    std::string exeName = GetExeBaseNameUtf8(GetProcessImagePath(pid));
    if (!DetachProcess(pid, err)) {
        outMsg = std::string(Tr(lang, "Detach failed: ", "卸载失败: ")) + err;
        SetStatusMessage(outMsg);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_autoStateMutex);
        m_manuallyDetachedPids.insert(pid);
    }
    outMsg = std::string(Tr(lang, "Detached from ", "已卸载 ")) + exeName + " (PID=" + std::to_string(pid) + ")";
    SetStatusMessage(outMsg);
    EmitNotify(
        Tr(lang, "PadSplit Detached", "PadSplit 已卸载"),
        exeName + Tr(lang, " restored to original window & input state", " 已恢复原始窗口与输入状态"));
    return true;
}

bool Controller::TogglePauseAll(std::string& outMsg) {
    UiLang lang = GetUiLang();
    auto pids = ListAttachedPids();
    if (pids.empty()) {
        outMsg = Tr(lang, "No games currently attached", "当前没有已接管的游戏");
        return false;
    }

    bool anyPaused = false;
    for (uint32_t pid : pids) {
        SharedState st{};
        std::string err;
        if (ReadProcessShm(pid, st, err)) {
            uint32_t nextEnabled = (st.enabled != 0) ? 0u : 1u;
            if (nextEnabled == 0) anyPaused = true;
            MutateProcessShm(pid, [nextEnabled](SharedState* s) {
                shm_store32(&s->enabled, nextEnabled);
            }, err);
        }
    }
    outMsg = anyPaused
        ? Tr(lang, "KBM isolation paused (mouse returned to game)", "已暂停键鼠隔离（鼠标已归还游戏）")
        : Tr(lang, "KBM isolation resumed", "已恢复手柄独占隔离");
    SetStatusMessage(outMsg);
    EmitNotify(
        anyPaused ? Tr(lang, "PadSplit Paused", "PadSplit 已暂停隔离")
                  : Tr(lang, "PadSplit Resumed", "PadSplit 已恢复隔离"),
        outMsg);
    return true;
}

std::vector<AttachedSessionInfo> Controller::GetAttachedSessions() {
    std::vector<AttachedSessionInfo> out;
    auto pids = ListAttachedPids();
    for (uint32_t pid : pids) {
        AttachedSessionInfo info;
        info.pid = pid;
        info.exe_name = GetExeBaseNameUtf8(GetProcessImagePath(pid));
        std::string err;
        if (ReadProcessShm(pid, info.state, err)) {
            out.push_back(std::move(info));
        }
    }
    return out;
}

std::vector<WindowProcessInfo> Controller::GetWindowedCandidates(bool gamesOnly) {
    return EnumWindowedProcesses(gamesOnly);
}

AppConfig Controller::GetConfigCopy() {
    std::lock_guard<std::mutex> lk(m_cfgMutex);
    return m_config;
}

void Controller::SetConfig(const AppConfig& cfg) {
    {
        std::lock_guard<std::mutex> lk(m_cfgMutex);
        m_config = cfg;
    }
    SaveCurrentConfig();
    RequestHotkeyRebind();
}

void Controller::AutoAttachLoop() {
    while (m_running) {
        for (int i = 0; i < 15 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!m_running) break;

        AppConfig cfg = GetConfigCopy();
        if (cfg.whitelist.empty()) continue;

        auto wins = EnumWindowedProcesses(false);

        // Prune exited PIDs from failed / manually-detached tracking sets
        {
            std::unordered_set<uint32_t> alivePids;
            alivePids.reserve(wins.size());
            for (const auto& w : wins) alivePids.insert(w.pid);

            std::lock_guard<std::mutex> lk(m_autoStateMutex);
            for (auto it = m_failedAutoPids.begin(); it != m_failedAutoPids.end();) {
                if (alivePids.find(*it) == alivePids.end()) it = m_failedAutoPids.erase(it);
                else ++it;
            }
            for (auto it = m_manuallyDetachedPids.begin(); it != m_manuallyDetachedPids.end();) {
                if (alivePids.find(*it) == alivePids.end()) it = m_manuallyDetachedPids.erase(it);
                else ++it;
            }
        }

        for (const auto& w : wins) {
            if (w.attached) continue;
            const auto* prof = FindWhitelistProfile(cfg, w.exe_name);
            if (!prof || !prof->auto_attach) continue;

            {
                std::lock_guard<std::mutex> lk(m_autoStateMutex);
                if (m_failedAutoPids.count(w.pid) || m_manuallyDetachedPids.count(w.pid)) {
                    continue;
                }
            }

            AntiCheatResult ac = CheckAntiCheat(w.pid, w.exe_path);
            if (ac.blocked) {
                std::lock_guard<std::mutex> lk(m_autoStateMutex);
                m_failedAutoPids.insert(w.pid);
                continue;
            }

            AttachOptions opts;
            opts.isolation_mode = prof->isolation_mode;
            opts.borderless_mode = prof->borderless_mode;
            opts.hide_idle_cursor = prof->hide_idle_cursor;
            opts.idle_cursor_ms = prof->idle_cursor_ms;
            opts.dsound_global_focus = prof->dsound_global_focus;

            std::string msg;
            if (!AttachPid(w.pid, opts, false, msg)) {
                std::lock_guard<std::mutex> lk(m_autoStateMutex);
                m_failedAutoPids.insert(w.pid);
            }
        }
    }
}

void Controller::PipeServerLoop() {
    while (m_running) {
        HANDLE hPipe = CreateNamedPipeW(
            PADSPLIT_CONTROL_PIPE,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 512, 512, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (connected && m_running) {
            char buf[256]{};
            DWORD readBytes = 0;
            if (ReadFile(hPipe, buf, sizeof(buf) - 1, &readBytes, nullptr) && readBytes > 0) {
                std::string cmd(buf, readBytes);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r' || cmd.back() == ' ')) {
                    cmd.pop_back();
                }

                std::string reply = "unknown\n";
                if (cmd == "ping") {
                    reply = "pong\n";
                } else if (cmd == "reload") {
                    ReloadConfig();
                    reply = "reloaded\n";
                } else if (cmd == "stop") {
                    reply = "stopping\n";
                    if (m_msgHwnd) {
                        PostMessageW(m_msgHwnd, WM_COMMAND, 1004, 0);
                    }
                }
                DWORD written = 0;
                WriteFile(hPipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
                FlushFileBuffers(hPipe);
            }
        }
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

} // namespace padsplit::app
