#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace padsplit {

constexpr uint32_t PADSPLIT_SHM_MAGIC = 0x5053504Cu; // "PSPL"
constexpr uint32_t PADSPLIT_SHM_VERSION = 1u;
constexpr uint32_t PADSPLIT_LOG_SLOTS = 128u;
constexpr uint32_t PADSPLIT_LOG_LINE_LEN = 192u;

constexpr const wchar_t* PADSPLIT_CONTROL_PIPE = L"\\\\.\\pipe\\PadSplit_Control";
constexpr const wchar_t* PADSPLIT_SINGLE_INSTANCE_MUTEX = L"Local\\PadSplit_SingleInstance";

enum IsolationMode : uint32_t {
    ISO_ALWAYS_BLOCK_KBM = 0,   // Gamepad-only isolation: always block KBM to game
    ISO_BLOCK_OUTSIDE_GAME = 1  // Border-crossing: block KBM only when cursor is outside game monitor
};

enum BorderlessMode : uint32_t {
    BORDERLESS_OFF = 0,
    BORDERLESS_WINDOW_STRIP = 1,
    BORDERLESS_FORCE_DX = 2
};

#pragma pack(push, 8)
struct SharedState {
    // Header (16 bytes)
    uint32_t magic;
    uint32_t version;
    uint32_t target_pid;
    uint32_t reserved0;

    // Controller -> Core config (48 bytes)
    volatile uint32_t enabled;
    volatile uint32_t isolation_mode;
    volatile uint32_t borderless_mode;
    volatile uint32_t hide_idle_cursor;
    volatile uint32_t idle_cursor_ms;
    volatile uint32_t spoof_focus;
    volatile uint32_t unlock_cursor;
    volatile uint32_t dsound_global_focus;
    volatile uint32_t request_detach;
    volatile uint32_t apply_borderless_seq;
    uint32_t reserved_ctrl[2];

    // Core -> Controller state (48 bytes)
    volatile uint32_t core_ready;           // 0=init, 1=ready, 2=detaching
    uint32_t core_arch;                     // 64 or 32
    uint64_t game_hwnd;
    int32_t game_mon_left;
    int32_t game_mon_top;
    int32_t game_mon_right;
    int32_t game_mon_bottom;
    volatile uint32_t real_is_foreground;
    volatile uint32_t kbm_currently_blocked;
    volatile uint32_t cursor_idle_hidden;
    uint32_t reserved_state;

    // Detection flags (40 bytes)
    volatile uint32_t detected_xinput;
    volatile uint32_t detected_dinput_pad;
    volatile uint32_t detected_dinput_kbm;
    volatile uint32_t detected_rawinput_pad;
    volatile uint32_t detected_rawinput_kbm;
    volatile uint32_t detected_steam_overlay;
    volatile uint32_t detected_dxgi;
    volatile uint32_t detected_d3d9;
    volatile uint32_t detected_dsound;
    volatile uint32_t detected_gameinput;

    // Telemetry counters (112 bytes)
    volatile uint64_t focus_api_spoofs;
    volatile uint64_t blocked_focus_msgs;
    volatile uint64_t blocked_clip_cursor;
    volatile uint64_t blocked_set_cursor_pos;
    volatile uint64_t blocked_kbm_polls;
    volatile uint64_t blocked_raw_kbm;
    volatile uint64_t blocked_dinput_kbm;
    volatile uint64_t xinput_polls;
    volatile uint64_t xinput_enable_blocked;
    volatile uint64_t dinput_coop_patched;
    volatile uint64_t rawinput_pad_inputsink_added;
    volatile uint64_t borderless_applied_count;
    volatile uint64_t gameinput_focus_patched;
    volatile uint64_t blocked_gameinput_kbm;

    // Lock-free log ring buffer (8 + 128*192 = 24584 bytes)
    volatile uint32_t log_write_seq;
    uint32_t reserved_log;
    char log_ring[PADSPLIT_LOG_SLOTS][PADSPLIT_LOG_LINE_LEN];
};
#pragma pack(pop)

static_assert(offsetof(SharedState, enabled) == 16, "SharedState offset mismatch: enabled");
static_assert(offsetof(SharedState, core_ready) == 64, "SharedState offset mismatch: core_ready");
static_assert(offsetof(SharedState, detected_xinput) == 112, "SharedState offset mismatch: detected_xinput");
static_assert(offsetof(SharedState, focus_api_spoofs) == 152, "SharedState offset mismatch: focus_api_spoofs");
static_assert(offsetof(SharedState, log_write_seq) == 264, "SharedState offset mismatch: log_write_seq");
static_assert(offsetof(SharedState, log_ring) == 272, "SharedState offset mismatch: log_ring");
static_assert(sizeof(SharedState) == 272 + PADSPLIT_LOG_SLOTS * PADSPLIT_LOG_LINE_LEN, "SharedState size mismatch");

inline void MakeShmName(uint32_t pid, wchar_t* out, size_t outChars) {
    _snwprintf(out, outChars, L"Local\\PadSplit_SHM_%u", pid);
    out[outChars - 1] = L'\0';
}

inline uint32_t shm_load32(const volatile uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void shm_store32(volatile uint32_t* p, uint32_t v) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(p), static_cast<LONG>(v));
}

inline uint64_t shm_load64(const volatile uint64_t* p) {
#if defined(_WIN64) || defined(__x86_64__)
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#else
    return static_cast<uint64_t>(InterlockedCompareExchange64(const_cast<volatile LONG64*>(reinterpret_cast<const volatile LONG64*>(p)), 0, 0));
#endif
}

inline void shm_inc64(volatile uint64_t* p) {
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(p));
}

inline void shm_append_log(SharedState* shm, const char* msg) {
    if (!shm || !msg) return;
    LONG seq = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->log_write_seq));
    uint32_t idx = static_cast<uint32_t>(seq - 1) % PADSPLIT_LOG_SLOTS;
    char* slot = shm->log_ring[idx];
    std::memset(slot, 0, PADSPLIT_LOG_LINE_LEN);
    std::strncpy(slot, msg, PADSPLIT_LOG_LINE_LEN - 1);
}

inline void InitSharedStateDefaults(SharedState* shm, uint32_t pid) {
    if (!shm) return;
    std::memset(shm, 0, sizeof(SharedState));
    shm->magic = PADSPLIT_SHM_MAGIC;
    shm->version = PADSPLIT_SHM_VERSION;
    shm->target_pid = pid;
    shm->enabled = 1;
    shm->isolation_mode = ISO_ALWAYS_BLOCK_KBM;
    shm->borderless_mode = BORDERLESS_WINDOW_STRIP;
    shm->hide_idle_cursor = 1;
    shm->idle_cursor_ms = 1500;
    shm->spoof_focus = 1;
    shm->unlock_cursor = 1;
    shm->dsound_global_focus = 1;
}

} // namespace padsplit
