#include "../injector/injector_lib.h"
#include "../shared/anticheat.h"
#include "../shared/config.h"
#include "../shared/protocol.h"
#include "../shared/version.h"
#include "../shared/win_util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace padsplit;

static void PrintUsage() {
    std::cout <<
R"(PadSplit CLI - Dual-Monitor Gamepad + Free Mouse/KB Controller

Usage:
  padsplit_cli list [--games-only] [--json]
  padsplit_cli inspect <pid|name> [--json]
  padsplit_cli attach <pid|name> [--mode gamepad-only|border-crossing] [--borderless off|window|force-dx] [--idle-hide 0|1] [--idle-ms N] [--force]
  padsplit_cli detach <pid|name>
  padsplit_cli status [pid|name] [--json]
  padsplit_cli set <pid|name> [--enabled 0|1] [--mode ...] [--borderless ...] [--idle-hide 0|1] [--idle-ms N] [--apply-borderless]
  padsplit_cli pause <pid|name>
  padsplit_cli resume <pid|name>
  padsplit_cli logs <pid|name> [--follow] [--tail N]
  padsplit_cli whitelist list [--json]
  padsplit_cli whitelist add <exe> [--mode ...] [--borderless ...]
  padsplit_cli whitelist remove <exe>
  padsplit_cli ac-check <pid|path> [--json]
  padsplit_cli monitors [--json]
  padsplit_cli autostart on | off | status
  padsplit_cli daemon ping | reload | stop
  padsplit_cli --version
)";
}

static bool HasFlag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

static std::string GetFlagValue(const std::vector<std::string>& args, const std::string& flag, const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == flag) return args[i + 1];
    }
    return def;
}

static IsolationMode ParseIsolationMode(const std::string& s) {
    if (s == "border-crossing" || s == "1") return ISO_BLOCK_OUTSIDE_GAME;
    return ISO_ALWAYS_BLOCK_KBM;
}

static BorderlessMode ParseBorderlessMode(const std::string& s) {
    if (s == "off" || s == "0") return BORDERLESS_OFF;
    if (s == "force-dx" || s == "2") return BORDERLESS_FORCE_DX;
    return BORDERLESS_WINDOW_STRIP;
}

static json SharedStateToJson(uint32_t pid, const SharedState& st) {
    std::string exeName = GetExeBaseNameUtf8(GetProcessImagePath(pid));
    return json{
        {"pid", pid},
        {"exe_name", exeName},
        {"core_ready", st.core_ready},
        {"core_arch", st.core_arch},
        {"game_hwnd", st.game_hwnd},
        {"enabled", st.enabled != 0},
        {"isolation_mode", st.isolation_mode == ISO_BLOCK_OUTSIDE_GAME ? "border-crossing" : "gamepad-only"},
        {"borderless_mode", st.borderless_mode == BORDERLESS_OFF ? "off" : (st.borderless_mode == BORDERLESS_FORCE_DX ? "force-dx" : "window")},
        {"hide_idle_cursor", st.hide_idle_cursor != 0},
        {"idle_cursor_ms", st.idle_cursor_ms},
        {"real_is_foreground", st.real_is_foreground != 0},
        {"kbm_currently_blocked", st.kbm_currently_blocked != 0},
        {"cursor_idle_hidden", st.cursor_idle_hidden != 0},
        {"game_monitor", {
            {"left", st.game_mon_left},
            {"top", st.game_mon_top},
            {"right", st.game_mon_right},
            {"bottom", st.game_mon_bottom}
        }},
        {"detected", {
            {"xinput", st.detected_xinput != 0},
            {"dinput_pad", st.detected_dinput_pad != 0},
            {"dinput_kbm", st.detected_dinput_kbm != 0},
            {"rawinput_pad", st.detected_rawinput_pad != 0},
            {"rawinput_kbm", st.detected_rawinput_kbm != 0},
            {"steam_overlay", st.detected_steam_overlay != 0},
            {"dxgi", st.detected_dxgi != 0},
            {"d3d9", st.detected_d3d9 != 0},
            {"dsound", st.detected_dsound != 0},
            {"gameinput", st.detected_gameinput != 0}
        }},
        {"counters", {
            {"focus_api_spoofs", st.focus_api_spoofs},
            {"blocked_focus_msgs", st.blocked_focus_msgs},
            {"blocked_clip_cursor", st.blocked_clip_cursor},
            {"blocked_set_cursor_pos", st.blocked_set_cursor_pos},
            {"blocked_kbm_polls", st.blocked_kbm_polls},
            {"blocked_raw_kbm", st.blocked_raw_kbm},
            {"blocked_dinput_kbm", st.blocked_dinput_kbm},
            {"xinput_polls", st.xinput_polls},
            {"xinput_enable_blocked", st.xinput_enable_blocked},
            {"dinput_coop_patched", st.dinput_coop_patched},
            {"rawinput_pad_inputsink_added", st.rawinput_pad_inputsink_added},
            {"borderless_applied_count", st.borderless_applied_count},
            {"gameinput_focus_patched", st.gameinput_focus_patched},
            {"blocked_gameinput_kbm", st.blocked_gameinput_kbm}
        }},
        {"log_write_seq", st.log_write_seq}
    };
}

static int CmdMonitors(const std::vector<std::string>& args) {
    bool asJson = HasFlag(args, "--json");
    auto mons = GetMonitors();
    if (asJson) {
        json arr = json::array();
        for (const auto& m : mons) {
            arr.push_back({
                {"index", m.index},
                {"primary", m.primary},
                {"left", m.left},
                {"top", m.top},
                {"right", m.right},
                {"bottom", m.bottom},
                {"width", m.width},
                {"height", m.height},
                {"device_name", m.device_name}
            });
        }
        std::cout << arr.dump(2) << "\n";
    } else {
        for (const auto& m : mons) {
            std::cout << "[" << m.index << "] " << m.device_name
                      << (m.primary ? " (Primary)" : "")
                      << " Bounds=(" << m.left << "," << m.top << ")-(" << m.right << "," << m.bottom << ") "
                      << m.width << "x" << m.height << "\n";
        }
    }
    return 0;
}

static int CmdList(const std::vector<std::string>& args) {
    bool gamesOnly = HasFlag(args, "--games-only");
    bool asJson = HasFlag(args, "--json");
    auto procs = EnumWindowedProcesses(gamesOnly);

    if (asJson) {
        json arr = json::array();
        for (const auto& p : procs) {
            arr.push_back({
                {"pid", p.pid},
                {"arch", p.arch},
                {"hwnd", p.hwnd},
                {"exe_name", p.exe_name},
                {"exe_path", wide_to_utf8(p.exe_path)},
                {"window_title", p.window_title},
                {"monitor_index", p.monitor_index},
                {"display_mode", p.display_mode},
                {"attached", p.attached},
                {"is_foreground", p.is_foreground}
            });
        }
        std::cout << arr.dump(2) << "\n";
    } else {
        for (const auto& p : procs) {
            std::cout << "PID=" << p.pid
                      << " [" << p.arch << "-bit]"
                      << (p.attached ? " [ATTACHED]" : "")
                      << (p.is_foreground ? " [FG]" : "")
                      << " Mon=" << p.monitor_index
                      << " (" << p.display_mode << ") "
                      << p.exe_name << " - \"" << p.window_title << "\"\n";
        }
    }
    return 0;
}

static int CmdInspect(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli inspect <pid|name> [--json]\n";
        return 1;
    }
    uint32_t pid = 0;
    std::string err;
    if (!ResolveTargetPid(args[0], pid, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    bool asJson = HasFlag(args, "--json");
    std::wstring exePath = GetProcessImagePath(pid);
    uint32_t arch = GetProcessArch(pid);
    AntiCheatResult ac = CheckAntiCheat(pid, exePath);

    WindowProcessInfo winInfo{};
    bool foundWin = false;
    for (const auto& w : EnumWindowedProcesses(false)) {
        if (w.pid == pid) {
            winInfo = w;
            foundWin = true;
            break;
        }
    }

    if (asJson) {
        json j = {
            {"pid", pid},
            {"arch", arch},
            {"exe_name", GetExeBaseNameUtf8(exePath)},
            {"exe_path", wide_to_utf8(exePath)},
            {"has_window", foundWin},
            {"hwnd", foundWin ? winInfo.hwnd : 0},
            {"window_title", foundWin ? winInfo.window_title : ""},
            {"monitor_index", foundWin ? winInfo.monitor_index : -1},
            {"display_mode", foundWin ? winInfo.display_mode : "none"},
            {"attached", foundWin ? winInfo.attached : false},
            {"anticheat", {
                {"blocked", ac.blocked},
                {"level", ac.level},
                {"signature", ac.signature},
                {"reason", ac.reason}
            }}
        };
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << "PID: " << pid << " (" << arch << "-bit)\n";
        std::cout << "Exe: " << wide_to_utf8(exePath) << "\n";
        if (foundWin) {
            std::cout << "Window: \"" << winInfo.window_title << "\" HWND=0x" << std::hex << winInfo.hwnd << std::dec
                      << " Mon=" << winInfo.monitor_index << " Mode=" << winInfo.display_mode << "\n";
        } else {
            std::cout << "Window: (none)\n";
        }
        std::cout << "AntiCheat: " << (ac.blocked ? ("BLOCKED (" + ac.signature + "): " + ac.reason) : "Clear") << "\n";
    }
    return 0;
}

static int CmdAttach(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli attach <pid|name> [options]\n";
        return 1;
    }
    uint32_t pid = 0;
    std::string err;
    if (!ResolveTargetPid(args[0], pid, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    AttachOptions opts;
    if (HasFlag(args, "--mode")) opts.isolation_mode = ParseIsolationMode(GetFlagValue(args, "--mode"));
    if (HasFlag(args, "--borderless")) opts.borderless_mode = ParseBorderlessMode(GetFlagValue(args, "--borderless"));
    if (HasFlag(args, "--idle-hide")) opts.hide_idle_cursor = (GetFlagValue(args, "--idle-hide") != "0");
    if (HasFlag(args, "--idle-ms")) {
        int ms = std::atoi(GetFlagValue(args, "--idle-ms", "1500").c_str());
        if (ms >= 200) opts.idle_cursor_ms = static_cast<uint32_t>(ms);
    }
    if (HasFlag(args, "--force")) opts.force_bypass_ac_warn = true;

    if (!AttachProcess(pid, opts, err)) {
        std::cerr << "Attach failed: " << err << "\n";
        return 1;
    }
    std::cout << "Attached to PID " << pid << "\n";
    return 0;
}

static int CmdDetach(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli detach <pid|name>\n";
        return 1;
    }
    uint32_t pid = 0;
    std::string err;
    if (!ResolveTargetPid(args[0], pid, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (!DetachProcess(pid, err)) {
        std::cerr << "Detach failed: " << err << "\n";
        return 1;
    }
    std::cout << "Detached from PID " << pid << "\n";
    return 0;
}

static int CmdStatus(const std::vector<std::string>& args) {
    bool asJson = HasFlag(args, "--json");
    std::vector<uint32_t> pids;

    std::string targetArg;
    for (const auto& a : args) {
        if (a != "--json") {
            targetArg = a;
            break;
        }
    }

    if (!targetArg.empty()) {
        uint32_t pid = 0;
        std::string err;
        if (!ResolveTargetPid(targetArg, pid, err)) {
            std::cerr << "Error: " << err << "\n";
            return 1;
        }
        pids.push_back(pid);
    } else {
        pids = ListAttachedPids();
    }

    if (asJson) {
        if (!targetArg.empty()) {
            SharedState st{};
            std::string err;
            if (!ReadProcessShm(pids[0], st, err)) {
                std::cerr << "Error: " << err << "\n";
                return 1;
            }
            std::cout << SharedStateToJson(pids[0], st).dump(2) << "\n";
        } else {
            json arr = json::array();
            for (uint32_t pid : pids) {
                SharedState st{};
                std::string err;
                if (ReadProcessShm(pid, st, err)) {
                    arr.push_back(SharedStateToJson(pid, st));
                }
            }
            std::cout << arr.dump(2) << "\n";
        }
    } else {
        if (pids.empty()) {
            std::cout << "No attached processes.\n";
            return 0;
        }
        for (uint32_t pid : pids) {
            SharedState st{};
            std::string err;
            if (!ReadProcessShm(pid, st, err)) {
                std::cerr << "PID " << pid << ": " << err << "\n";
                continue;
            }
            std::cout << "PID=" << pid << " (" << st.core_arch << "-bit) "
                      << "enabled=" << st.enabled << " kbm_blocked=" << st.kbm_currently_blocked
                      << " real_fg=" << st.real_is_foreground
                      << " spoofs=" << st.focus_api_spoofs
                      << " clip_blk=" << st.blocked_clip_cursor
                      << " setpos_blk=" << st.blocked_set_cursor_pos
                      << " kbm_blk=" << (st.blocked_kbm_polls + st.blocked_raw_kbm + st.blocked_dinput_kbm + st.blocked_gameinput_kbm)
                      << " xinput=" << st.xinput_polls
                      << " gameinput=" << st.detected_gameinput << "\n";
        }
    }
    return 0;
}

static int CmdSet(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli set <pid|name> [options]\n";
        return 1;
    }
    uint32_t pid = 0;
    std::string err;
    if (!ResolveTargetPid(args[0], pid, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    bool ok = MutateProcessShm(pid, [&](SharedState* shm) {
        if (HasFlag(args, "--enabled")) {
            shm_store32(&shm->enabled, GetFlagValue(args, "--enabled") != "0" ? 1u : 0u);
        }
        if (HasFlag(args, "--mode")) {
            shm_store32(&shm->isolation_mode, static_cast<uint32_t>(ParseIsolationMode(GetFlagValue(args, "--mode"))));
        }
        if (HasFlag(args, "--borderless")) {
            shm_store32(&shm->borderless_mode, static_cast<uint32_t>(ParseBorderlessMode(GetFlagValue(args, "--borderless"))));
        }
        if (HasFlag(args, "--idle-hide")) {
            shm_store32(&shm->hide_idle_cursor, GetFlagValue(args, "--idle-hide") != "0" ? 1u : 0u);
        }
        if (HasFlag(args, "--idle-ms")) {
            int ms = std::atoi(GetFlagValue(args, "--idle-ms", "1500").c_str());
            if (ms >= 200) shm_store32(&shm->idle_cursor_ms, static_cast<uint32_t>(ms));
        }
        if (HasFlag(args, "--apply-borderless")) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->apply_borderless_seq));
        }
    }, err);

    if (!ok) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    std::cout << "Updated PID " << pid << "\n";
    return 0;
}

static int CmdPauseResume(const std::vector<std::string>& args, bool pause) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli " << (pause ? "pause" : "resume") << " <pid|name>\n";
        return 1;
    }
    uint32_t pid = 0;
    std::string err;
    if (!ResolveTargetPid(args[0], pid, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    if (!MutateProcessShm(pid, [&](SharedState* shm) {
            shm_store32(&shm->enabled, pause ? 0u : 1u);
        }, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }
    std::cout << (pause ? "Paused" : "Resumed") << " PID " << pid << "\n";
    return 0;
}

static int CmdLogs(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli logs <pid|name> [--follow] [--tail N]\n";
        return 1;
    }
    uint32_t pid = 0;
    std::string err;
    if (!ResolveTargetPid(args[0], pid, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    bool follow = HasFlag(args, "--follow");
    uint32_t tail = static_cast<uint32_t>(std::max(1, std::atoi(GetFlagValue(args, "--tail", "50").c_str())));

    SharedState st{};
    if (!ReadProcessShm(pid, st, err)) {
        std::cerr << "Error: " << err << "\n";
        return 1;
    }

    uint32_t total = st.log_write_seq;
    uint32_t start = (total > PADSPLIT_LOG_SLOTS) ? (total - PADSPLIT_LOG_SLOTS) : 0;
    if (total - start > tail) start = total - tail;

    for (uint32_t seq = start; seq < total; ++seq) {
        uint32_t idx = seq % PADSPLIT_LOG_SLOTS;
        if (st.log_ring[idx][0] != '\0') {
            std::cout << st.log_ring[idx] << "\n";
        }
    }

    if (!follow) return 0;

    uint32_t lastSeq = total;
    while (true) {
        Sleep(150);
        if (!ReadProcessShm(pid, st, err)) break;
        if (st.log_write_seq > lastSeq) {
            uint32_t beginSeq = (st.log_write_seq - lastSeq > PADSPLIT_LOG_SLOTS)
                              ? (st.log_write_seq - PADSPLIT_LOG_SLOTS)
                              : lastSeq;
            for (uint32_t seq = beginSeq; seq < st.log_write_seq; ++seq) {
                uint32_t idx = seq % PADSPLIT_LOG_SLOTS;
                if (st.log_ring[idx][0] != '\0') {
                    std::cout << st.log_ring[idx] << "\n";
                }
            }
            lastSeq = st.log_write_seq;
        }
        if (st.core_ready == 2) break;
    }
    return 0;
}

static int CmdWhitelist(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli whitelist <list|add|remove> ...\n";
        return 1;
    }

    AppConfig cfg;
    LoadConfig(cfg);
    const std::string& sub = args[0];

    if (sub == "list") {
        bool asJson = HasFlag(args, "--json");
        if (asJson) {
            json arr = json::array();
            for (const auto& gp : cfg.whitelist) {
                arr.push_back({
                    {"exe_name", gp.exe_name},
                    {"auto_attach", gp.auto_attach},
                    {"isolation_mode", gp.isolation_mode == ISO_BLOCK_OUTSIDE_GAME ? "border-crossing" : "gamepad-only"},
                    {"borderless_mode", gp.borderless_mode == BORDERLESS_OFF ? "off" : (gp.borderless_mode == BORDERLESS_FORCE_DX ? "force-dx" : "window")},
                    {"hide_idle_cursor", gp.hide_idle_cursor},
                    {"idle_cursor_ms", gp.idle_cursor_ms}
                });
            }
            std::cout << arr.dump(2) << "\n";
        } else {
            for (const auto& gp : cfg.whitelist) {
                std::cout << gp.exe_name << " auto=" << gp.auto_attach
                          << " mode=" << (gp.isolation_mode == ISO_BLOCK_OUTSIDE_GAME ? "border-crossing" : "gamepad-only")
                          << " borderless=" << (gp.borderless_mode == BORDERLESS_OFF ? "off" : (gp.borderless_mode == BORDERLESS_FORCE_DX ? "force-dx" : "window"))
                          << "\n";
            }
        }
        return 0;
    }

    if (sub == "add" && args.size() >= 2) {
        GameProfile gp;
        gp.exe_name = args[1];
        if (HasFlag(args, "--mode")) gp.isolation_mode = ParseIsolationMode(GetFlagValue(args, "--mode"));
        if (HasFlag(args, "--borderless")) gp.borderless_mode = ParseBorderlessMode(GetFlagValue(args, "--borderless"));
        UpsertWhitelistProfile(cfg, gp);
        SaveConfig(cfg);
        std::cout << "Added/updated whitelist entry: " << gp.exe_name << "\n";
        return 0;
    }

    if (sub == "remove" && args.size() >= 2) {
        if (RemoveWhitelistProfile(cfg, args[1])) {
            SaveConfig(cfg);
            std::cout << "Removed whitelist entry: " << args[1] << "\n";
            return 0;
        }
        std::cerr << "Not found in whitelist: " << args[1] << "\n";
        return 1;
    }

    std::cerr << "Invalid whitelist command.\n";
    return 1;
}

static int CmdAcCheck(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli ac-check <pid|path> [--json]\n";
        return 1;
    }
    bool asJson = HasFlag(args, "--json");
    uint32_t pid = 0;
    std::wstring path;
    std::string err;

    bool allDigits = std::all_of(args[0].begin(), args[0].end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (allDigits && ResolveTargetPid(args[0], pid, err)) {
        path = GetProcessImagePath(pid);
    } else {
        path = utf8_to_wide(args[0]);
    }

    AntiCheatResult ac = CheckAntiCheat(pid, path);
    if (asJson) {
        json j = {
            {"blocked", ac.blocked},
            {"level", ac.level},
            {"signature", ac.signature},
            {"reason", ac.reason}
        };
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << (ac.blocked ? ("BLOCKED (" + ac.signature + "): " + ac.reason) : "Clear") << "\n";
    }
    return 0;
}

static int CmdAutostart(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "status") {
        std::cout << (IsAutoStartEnabled() ? "enabled" : "disabled") << "\n";
        return 0;
    }
    if (args[0] == "on" || args[0] == "enable") {
        if (!SetAutoStartEnabled(true)) {
            std::cerr << "Failed to enable autostart\n";
            return 1;
        }
        std::cout << "Autostart enabled (minimized to tray on login)\n";
        return 0;
    }
    if (args[0] == "off" || args[0] == "disable") {
        if (!SetAutoStartEnabled(false)) {
            std::cerr << "Failed to disable autostart\n";
            return 1;
        }
        std::cout << "Autostart disabled\n";
        return 0;
    }
    std::cerr << "Usage: padsplit_cli autostart <on|off|status>\n";
    return 1;
}

static int CmdDaemon(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: padsplit_cli daemon <ping|reload|stop>\n";
        return 1;
    }
    const std::string& cmd = args[0];
    if (cmd != "ping" && cmd != "reload" && cmd != "stop") {
        std::cerr << "Unknown daemon command: " << cmd << "\n";
        return 1;
    }

    HANDLE hPipe = CreateFileW(PADSPLIT_CONTROL_PIPE, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        std::cerr << "Daemon not running or pipe unavailable (win32=" << GetLastError() << ")\n";
        return 1;
    }

    DWORD written = 0;
    std::string msg = cmd + "\n";
    WriteFile(hPipe, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);

    char buf[256]{};
    DWORD readBytes = 0;
    if (ReadFile(hPipe, buf, sizeof(buf) - 1, &readBytes, nullptr) && readBytes > 0) {
        std::cout << std::string(buf, readBytes);
        if (buf[readBytes - 1] != '\n') std::cout << "\n";
    }
    CloseHandle(hPipe);
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    EnablePerMonitorDpiAwareness();

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string cmd = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (cmd == "list") return CmdList(args);
    if (cmd == "inspect") return CmdInspect(args);
    if (cmd == "attach") return CmdAttach(args);
    if (cmd == "detach") return CmdDetach(args);
    if (cmd == "status") return CmdStatus(args);
    if (cmd == "set") return CmdSet(args);
    if (cmd == "pause") return CmdPauseResume(args, true);
    if (cmd == "resume") return CmdPauseResume(args, false);
    if (cmd == "logs") return CmdLogs(args);
    if (cmd == "whitelist") return CmdWhitelist(args);
    if (cmd == "ac-check") return CmdAcCheck(args);
    if (cmd == "monitors") return CmdMonitors(args);
    if (cmd == "autostart") return CmdAutostart(args);
    if (cmd == "daemon") return CmdDaemon(args);
    if (cmd == "--version" || cmd == "-v" || cmd == "version") {
        std::cout << "PadSplit CLI v" << PADSPLIT_VERSION_STR << "\n";
        return 0;
    }

    PrintUsage();
    return 1;
}
