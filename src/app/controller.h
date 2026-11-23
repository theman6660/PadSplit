#pragma once

#include "i18n.h"
#include "../injector/injector_lib.h"
#include "../shared/config.h"
#include "../shared/protocol.h"
#include "../shared/win_util.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace padsplit::app {

constexpr UINT WM_TRAY_RELOAD_HOTKEYS = WM_APP + 104;

enum HotkeyId : int {
    HK_ATTACH_FOREGROUND = 1,
    HK_TOGGLE_PAUSE = 2
};

struct AttachedSessionInfo {
    uint32_t pid = 0;
    std::string exe_name;
    SharedState state{};
};

class Controller {
public:
    Controller() = default;
    ~Controller();

    bool Init(HWND msgHwnd);
    void Shutdown();

    void ReloadConfig();
    bool SaveCurrentConfig();

    void RequestHotkeyRebind();
    void RegisterGlobalHotkeys();
    void UnregisterGlobalHotkeys();
    bool IsAttachHotkeyOk() const { return m_hotkeyAttachOk.load(); }
    bool IsPauseHotkeyOk() const { return m_hotkeyPauseOk.load(); }
    void HandleHotkey(int id);

    AttachOptions BuildAttachOptionsForExe(const std::string& exeName);
    bool AttachPid(uint32_t pid, const AttachOptions& opts, bool addToWhitelist, std::string& outMsg);
    bool DetachPid(uint32_t pid, std::string& outMsg);
    bool TogglePauseAll(std::string& outMsg);

    std::vector<AttachedSessionInfo> GetAttachedSessions();
    std::vector<WindowProcessInfo> GetWindowedCandidates(bool gamesOnly);
    AppConfig GetConfigCopy();
    void SetConfig(const AppConfig& cfg);
    UiLang GetUiLang();
    std::string GetLastStatusMessage();
    void SetNotifyCallback(std::function<void(const std::string&, const std::string&)> cb);

private:
    void AutoAttachLoop();
    void PipeServerLoop();
    void SetStatusMessage(const std::string& msg);
    void EmitNotify(const std::string& title, const std::string& body);

    HWND m_msgHwnd = nullptr;
    DWORD m_uiThreadId = 0;
    std::mutex m_cfgMutex;
    AppConfig m_config;
    std::mutex m_statusMutex;
    std::string m_lastStatus;
    std::function<void(const std::string&, const std::string&)> m_notifyCb;

    std::mutex m_autoStateMutex;
    std::unordered_set<uint32_t> m_failedAutoPids;
    std::unordered_set<uint32_t> m_manuallyDetachedPids;

    std::atomic<bool> m_hotkeyAttachOk{false};
    std::atomic<bool> m_hotkeyPauseOk{false};
    std::atomic<bool> m_running{false};
    std::thread m_autoThread;
    std::thread m_pipeThread;
};

} // namespace padsplit::app
