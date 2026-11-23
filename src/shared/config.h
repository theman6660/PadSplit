#pragma once

#include "protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace padsplit {

struct HotkeySpec {
    uint32_t modifiers = 0; // MOD_CONTROL | MOD_ALT | MOD_SHIFT
    uint32_t vk = 0;
    std::string text;
};

struct GameProfile {
    std::string exe_name;
    bool auto_attach = true;
    IsolationMode isolation_mode = ISO_ALWAYS_BLOCK_KBM;
    BorderlessMode borderless_mode = BORDERLESS_WINDOW_STRIP;
    bool hide_idle_cursor = true;
    uint32_t idle_cursor_ms = 1500;
    bool dsound_global_focus = true;
};

struct AppConfig {
    std::string language = "auto"; // "auto", "en", "zh"
    std::string hotkey_attach = "Ctrl+Alt+P";
    std::string hotkey_toggle_pause = "Ctrl+Alt+G";
    bool start_minimized = true;
    bool default_hide_idle_cursor = true;
    uint32_t default_idle_cursor_ms = 1500;
    std::vector<GameProfile> whitelist;
};

std::wstring GetDefaultConfigPath();
bool LoadConfig(AppConfig& outCfg, const std::wstring& path = L"");
bool SaveConfig(const AppConfig& cfg, const std::wstring& path = L"");

bool ParseHotkeyString(const std::string& str, HotkeySpec& outSpec);
GameProfile* FindWhitelistProfile(AppConfig& cfg, const std::string& exeName);
const GameProfile* FindWhitelistProfile(const AppConfig& cfg, const std::string& exeName);
void UpsertWhitelistProfile(AppConfig& cfg, const GameProfile& prof);
bool RemoveWhitelistProfile(AppConfig& cfg, const std::string& exeName);

} // namespace padsplit
