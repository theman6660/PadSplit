#pragma once

#include "controller.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

namespace padsplit::app {

class UiApp {
public:
    UiApp() = default;
    ~UiApp();

    int Run(HINSTANCE hInst, bool startMinimized, bool forceShow = false);

private:
    bool CreateTrayWindow(HINSTANCE hInst);
    void AddTrayIcon();
    void UpdateTrayTooltip();
    void DestroyTrayIcon();
    void ShowTrayMenu();
    void ShowTrayBalloon(const std::wstring& title, const std::wstring& body);
    void PostTrayNotification(const std::string& title, const std::string& body);

    void OpenPanel();
    void ClosePanel();
    void RenderPanelFrame();
    void RefreshCachedData(bool force);

    bool InitD3D(HWND hwnd);
    void CleanupD3D();
    void ResizeD3D(UINT w, UINT h);

    static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HINSTANCE m_hInst = nullptr;
    HWND m_trayHwnd = nullptr;
    HWND m_panelHwnd = nullptr;
    HICON m_trayIcon = nullptr;
    HICON m_appIconBig = nullptr;
    UINT m_wmTaskbarCreated = 0;

    bool m_panelOpen = false;
    bool m_pendingClosePanel = false;
    bool m_exiting = false;
    bool m_showAllProcesses = false;
    bool m_autoStartEnabled = false;
    bool m_logAutoScroll = true;
    bool m_hasCjkFont = false;
    int m_selectedLogIdx = 0;
    UiLang m_lastAppliedTitleLang = UiLang::En;

    ULONGLONG m_lastPollTick = 0;
    bool m_forceRefresh = true;
    std::vector<AttachedSessionInfo> m_cachedSessions;
    std::vector<WindowProcessInfo> m_cachedCandidates;

    AppConfig m_editCfg;
    char m_hkAttachBuf[64]{};
    char m_hkPauseBuf[64]{};
    std::mutex m_notifyMutex;
    std::wstring m_pendingNotifyTitle;
    std::wstring m_pendingNotifyBody;

    Controller m_controller;
};

} // namespace padsplit::app
