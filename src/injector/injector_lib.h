#pragma once

#include "../shared/protocol.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace padsplit {

struct AttachOptions {
    IsolationMode isolation_mode = ISO_ALWAYS_BLOCK_KBM;
    BorderlessMode borderless_mode = BORDERLESS_WINDOW_STRIP;
    bool hide_idle_cursor = true;
    uint32_t idle_cursor_ms = 1500;
    bool dsound_global_focus = true;
    bool force_bypass_ac_warn = false;
};

bool InjectDllSameArch(uint32_t pid, const std::wstring& dllPath, std::string& err);
bool AttachProcess(uint32_t pid, const AttachOptions& opts, std::string& err);
bool DetachProcess(uint32_t pid, std::string& err);

bool ReadProcessShm(uint32_t pid, SharedState& outState, std::string& err);
bool MutateProcessShm(uint32_t pid, const std::function<void(SharedState*)>& fn, std::string& err);
std::vector<uint32_t> ListAttachedPids();

} // namespace padsplit
