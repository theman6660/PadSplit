#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace padsplit {

struct MonitorInfoEx {
    int32_t index = 0;
    bool primary = false;
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
    int32_t width = 0;
    int32_t height = 0;
    std::string device_name;
};

struct WindowProcessInfo {
    uint32_t pid = 0;
    uint32_t arch = 0; // 64 or 32
    uint64_t hwnd = 0;
    std::string exe_name;
    std::wstring exe_path;
    std::string window_title;
    int32_t monitor_index = -1;
    std::string display_mode; // "borderless", "windowed", "fullscreen-like", "minimized"
    bool attached = false;
    bool is_foreground = false;
};

std::wstring utf8_to_wide(const std::string& s);
std::string wide_to_utf8(const std::wstring& ws);
std::string to_lower_ascii(std::string s);

void EnablePerMonitorDpiAwareness();
std::vector<MonitorInfoEx> GetMonitors();
uint32_t GetProcessArch(DWORD pid);
std::wstring GetProcessImagePath(DWORD pid);
std::string GetExeBaseNameUtf8(const std::wstring& path);
std::wstring GetExeDirectory();

std::vector<WindowProcessInfo> EnumWindowedProcesses(bool gamesOnly = false);
bool ResolveTargetPid(const std::string& pidOrName, uint32_t& outPid, std::string& err);
bool IsLikelySystemOrUtilityExe(const std::string& exeLower);

bool IsAutoStartEnabled();
bool SetAutoStartEnabled(bool enable, const std::wstring& exePath = L"");

} // namespace padsplit
