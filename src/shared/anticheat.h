#pragma once

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace padsplit {

struct AntiCheatResult {
    bool blocked = false;
    std::string level;     // "none", "warn", "block"
    std::string reason;
    std::string signature;
};

AntiCheatResult CheckAntiCheat(uint32_t pid, const std::wstring& exePath);

} // namespace padsplit
