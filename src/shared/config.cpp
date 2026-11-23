#include "config.h"
#include "win_util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace padsplit {

using json = nlohmann::json;

std::wstring GetDefaultConfigPath() {
    return GetExeDirectory() + L"\\config.json";
}

static std::string IsolationToString(IsolationMode m) {
    return (m == ISO_BLOCK_OUTSIDE_GAME) ? "border-crossing" : "gamepad-only";
}

static IsolationMode IsolationFromString(const std::string& s) {
    return (s == "border-crossing" || s == "1") ? ISO_BLOCK_OUTSIDE_GAME : ISO_ALWAYS_BLOCK_KBM;
}

static std::string BorderlessToString(BorderlessMode m) {
    if (m == BORDERLESS_OFF) return "off";
    if (m == BORDERLESS_FORCE_DX) return "force-dx";
    return "window";
}

static BorderlessMode BorderlessFromString(const std::string& s) {
    if (s == "off" || s == "0") return BORDERLESS_OFF;
    if (s == "force-dx" || s == "2") return BORDERLESS_FORCE_DX;
    return BORDERLESS_WINDOW_STRIP;
}

bool LoadConfig(AppConfig& outCfg, const std::wstring& path) {
    std::wstring p = path.empty() ? GetDefaultConfigPath() : path;
    std::ifstream ifs(std::filesystem::path(p), std::ios::binary);
    if (!ifs.is_open()) {
        return false;
    }

    try {
        json j;
        ifs >> j;
        if (j.contains("language") && j["language"].is_string()) {
            std::string lang = to_lower_ascii(j["language"].get<std::string>());
            if (lang == "auto" || lang == "en" || lang == "zh") {
                outCfg.language = lang;
            }
        }
        if (j.contains("hotkey_attach") && j["hotkey_attach"].is_string()) {
            outCfg.hotkey_attach = j["hotkey_attach"].get<std::string>();
        }
        if (j.contains("hotkey_toggle_pause") && j["hotkey_toggle_pause"].is_string()) {
            outCfg.hotkey_toggle_pause = j["hotkey_toggle_pause"].get<std::string>();
        }
        if (j.contains("start_minimized") && j["start_minimized"].is_boolean()) {
            outCfg.start_minimized = j["start_minimized"].get<bool>();
        }
        if (j.contains("default_hide_idle_cursor") && j["default_hide_idle_cursor"].is_boolean()) {
            outCfg.default_hide_idle_cursor = j["default_hide_idle_cursor"].get<bool>();
        }
        if (j.contains("default_idle_cursor_ms") && j["default_idle_cursor_ms"].is_number_unsigned()) {
            outCfg.default_idle_cursor_ms = j["default_idle_cursor_ms"].get<uint32_t>();
        }
        outCfg.whitelist.clear();
        if (j.contains("whitelist") && j["whitelist"].is_array()) {
            for (const auto& item : j["whitelist"]) {
                if (!item.is_object() || !item.contains("exe_name")) continue;
                GameProfile gp;
                gp.exe_name = item.value("exe_name", "");
                if (gp.exe_name.empty()) continue;
                gp.auto_attach = item.value("auto_attach", true);
                gp.isolation_mode = IsolationFromString(item.value("isolation_mode", "gamepad-only"));
                gp.borderless_mode = BorderlessFromString(item.value("borderless_mode", "window"));
                gp.hide_idle_cursor = item.value("hide_idle_cursor", outCfg.default_hide_idle_cursor);
                gp.idle_cursor_ms = item.value("idle_cursor_ms", outCfg.default_idle_cursor_ms);
                gp.dsound_global_focus = item.value("dsound_global_focus", true);
                outCfg.whitelist.push_back(std::move(gp));
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveConfig(const AppConfig& cfg, const std::wstring& path) {
    std::wstring p = path.empty() ? GetDefaultConfigPath() : path;
    std::wstring tmp = p + L".tmp";

    json j;
    j["language"] = cfg.language;
    j["hotkey_attach"] = cfg.hotkey_attach;
    j["hotkey_toggle_pause"] = cfg.hotkey_toggle_pause;
    j["start_minimized"] = cfg.start_minimized;
    j["default_hide_idle_cursor"] = cfg.default_hide_idle_cursor;
    j["default_idle_cursor_ms"] = cfg.default_idle_cursor_ms;

    json arr = json::array();
    for (const auto& gp : cfg.whitelist) {
        arr.push_back({
            {"exe_name", gp.exe_name},
            {"auto_attach", gp.auto_attach},
            {"isolation_mode", IsolationToString(gp.isolation_mode)},
            {"borderless_mode", BorderlessToString(gp.borderless_mode)},
            {"hide_idle_cursor", gp.hide_idle_cursor},
            {"idle_cursor_ms", gp.idle_cursor_ms},
            {"dsound_global_focus", gp.dsound_global_focus}
        });
    }
    j["whitelist"] = std::move(arr);

    {
        std::ofstream ofs(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        std::string dump = j.dump(2);
        ofs.write(dump.data(), static_cast<std::streamsize>(dump.size()));
        ofs.flush();
        if (!ofs.good()) return false;
    }

    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool ParseHotkeyString(const std::string& str, HotkeySpec& outSpec) {
    outSpec = HotkeySpec{};
    outSpec.text = str;

    std::stringstream ss(str);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        std::string t = to_lower_ascii(tok);
        // trim spaces
        t.erase(0, t.find_first_not_of(" \t"));
        if (!t.empty()) t.erase(t.find_last_not_of(" \t") + 1);
        if (t.empty()) continue;

        if (t == "ctrl" || t == "control") {
            outSpec.modifiers |= MOD_CONTROL;
        } else if (t == "alt") {
            outSpec.modifiers |= MOD_ALT;
        } else if (t == "shift") {
            outSpec.modifiers |= MOD_SHIFT;
        } else if (t == "win") {
            outSpec.modifiers |= MOD_WIN;
        } else if (t.size() == 1 && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= '0' && t[0] <= '9'))) {
            outSpec.vk = static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(t[0])));
        } else if (t.size() >= 2 && t[0] == 'f') {
            int fn = std::atoi(t.c_str() + 1);
            if (fn >= 1 && fn <= 24) {
                outSpec.vk = static_cast<uint32_t>(VK_F1 + (fn - 1));
            }
        } else if (t == "space") {
            outSpec.vk = VK_SPACE;
        } else if (t == "tab") {
            outSpec.vk = VK_TAB;
        } else if (t == "insert" || t == "ins") {
            outSpec.vk = VK_INSERT;
        } else if (t == "home") {
            outSpec.vk = VK_HOME;
        }
    }
    return outSpec.vk != 0;
}

GameProfile* FindWhitelistProfile(AppConfig& cfg, const std::string& exeName) {
    std::string needle = to_lower_ascii(exeName);
    for (auto& gp : cfg.whitelist) {
        if (to_lower_ascii(gp.exe_name) == needle) return &gp;
    }
    return nullptr;
}

const GameProfile* FindWhitelistProfile(const AppConfig& cfg, const std::string& exeName) {
    std::string needle = to_lower_ascii(exeName);
    for (const auto& gp : cfg.whitelist) {
        if (to_lower_ascii(gp.exe_name) == needle) return &gp;
    }
    return nullptr;
}

void UpsertWhitelistProfile(AppConfig& cfg, const GameProfile& prof) {
    if (auto* existing = FindWhitelistProfile(cfg, prof.exe_name)) {
        *existing = prof;
        return;
    }
    cfg.whitelist.push_back(prof);
}

bool RemoveWhitelistProfile(AppConfig& cfg, const std::string& exeName) {
    std::string needle = to_lower_ascii(exeName);
    auto it = std::remove_if(cfg.whitelist.begin(), cfg.whitelist.end(), [&](const GameProfile& gp) {
        return to_lower_ascii(gp.exe_name) == needle;
    });
    if (it == cfg.whitelist.end()) return false;
    cfg.whitelist.erase(it, cfg.whitelist.end());
    return true;
}

} // namespace padsplit
