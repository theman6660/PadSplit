#include "win_util.h"
#include "protocol.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <unordered_map>

#include <tlhelp32.h>

namespace padsplit {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring ws(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), ws.data(), n);
    return ws;
}

std::string wide_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string to_lower_ascii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

void EnablePerMonitorDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return;

    using FnSetDpiCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto fn = reinterpret_cast<FnSetDpiCtx>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (fn) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }
}

static BOOL CALLBACK EnumMonProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<MonitorInfoEx>*>(lParam);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorInfoEx info;
        info.index = static_cast<int32_t>(list->size());
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        info.left = mi.rcMonitor.left;
        info.top = mi.rcMonitor.top;
        info.right = mi.rcMonitor.right;
        info.bottom = mi.rcMonitor.bottom;
        info.width = mi.rcMonitor.right - mi.rcMonitor.left;
        info.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
        info.device_name = wide_to_utf8(mi.szDevice);
        list->push_back(std::move(info));
    }
    return TRUE;
}

std::vector<MonitorInfoEx> GetMonitors() {
    std::vector<MonitorInfoEx> list;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&list));
    return list;
}

uint32_t GetProcessArch(DWORD pid) {
    if (pid == 0) return 0;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return 0;

    uint32_t arch = 0;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    using FnIsWow64Process2 = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto fn2 = k32 ? reinterpret_cast<FnIsWow64Process2>(GetProcAddress(k32, "IsWow64Process2")) : nullptr;
    if (fn2) {
        USHORT procMachine = 0;
        USHORT nativeMachine = 0;
        if (fn2(hProc, &procMachine, &nativeMachine)) {
            if (procMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
                arch = (nativeMachine == IMAGE_FILE_MACHINE_AMD64 || nativeMachine == IMAGE_FILE_MACHINE_ARM64) ? 64 : 32;
            } else if (procMachine == IMAGE_FILE_MACHINE_I386) {
                arch = 32;
            } else {
                arch = 64;
            }
        }
    } else {
        BOOL isWow64 = FALSE;
        if (IsWow64Process(hProc, &isWow64)) {
            SYSTEM_INFO si{};
            GetNativeSystemInfo(&si);
            bool osIs64 = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64);
            arch = (osIs64 && !isWow64) ? 64 : 32;
        }
    }

    CloseHandle(hProc);
    return arch;
}

std::wstring GetProcessImagePath(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    wchar_t buf[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(buf));
    std::wstring res;
    if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
        res.assign(buf, size);
    }
    CloseHandle(hProc);
    return res;
}

std::string GetExeBaseNameUtf8(const std::wstring& path) {
    if (path.empty()) return "";
    size_t pos = path.find_last_of(L"\\/");
    std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
    return wide_to_utf8(name);
}

std::wstring GetExeDirectory() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0) return L".";
    std::wstring p(buf, len);
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return p.substr(0, pos);
}

bool IsLikelySystemOrUtilityExe(const std::string& exeLower) {
    static const char* kIgnored[] = {
        "explorer.exe", "shellexperiencehost.exe", "searchhost.exe", "startmenuexperiencehost.exe",
        "textinputhost.exe", "applicationframehost.exe", "systemsettings.exe", "taskmgr.exe",
        "cmd.exe", "powershell.exe", "pwsh.exe", "windowsterminal.exe", "conhost.exe",
        "code.exe", "antigravity.exe", "chrome.exe", "msedge.exe", "firefox.exe",
        "padsplit.exe", "padsplit_cli.exe", "padsplit_helper_x86.exe",
        "dwm.exe", "ctfmon.exe", "nvidia overlay.exe", "steamwebhelper.exe"
    };
    for (const char* item : kIgnored) {
        if (exeLower == item) return true;
    }
    return false;
}

static bool IsPidAttached(uint32_t pid) {
    wchar_t shmName[64]{};
    MakeShmName(pid, shmName, std::size(shmName));
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName);
    if (!hMap) return false;
    bool ready = false;
    auto* shm = static_cast<const SharedState*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedState)));
    if (shm) {
        if (shm->magic == PADSPLIT_SHM_MAGIC && shm_load32(&shm->core_ready) == 1) {
            ready = true;
        }
        UnmapViewOfFile(shm);
    }
    CloseHandle(hMap);
    return ready;
}

struct EnumWinCtx {
    std::vector<MonitorInfoEx> monitors;
    HWND fgHwnd;
    std::unordered_map<uint32_t, WindowProcessInfo> bestByPid;
    std::unordered_map<uint32_t, int64_t> bestAreaByPid;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    int32_t w = rc.right - rc.left;
    int32_t h = rc.bottom - rc.top;
    if (w < 120 || h < 80) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

    wchar_t titleBuf[256]{};
    GetWindowTextW(hwnd, titleBuf, static_cast<int>(std::size(titleBuf)));
    if (titleBuf[0] == L'\0') return TRUE;

    auto* ctx = reinterpret_cast<EnumWinCtx*>(lParam);
    int64_t area = static_cast<int64_t>(w) * static_cast<int64_t>(h);
    auto itArea = ctx->bestAreaByPid.find(pid);
    if (itArea != ctx->bestAreaByPid.end() && itArea->second >= area) {
        return TRUE;
    }

    WindowProcessInfo info;
    info.pid = pid;
    info.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
    info.window_title = wide_to_utf8(titleBuf);
    info.exe_path = GetProcessImagePath(pid);
    info.exe_name = GetExeBaseNameUtf8(info.exe_path);
    if (info.exe_name.empty()) return TRUE;

    info.arch = GetProcessArch(pid);
    info.is_foreground = (hwnd == ctx->fgHwnd);
    info.attached = IsPidAttached(pid);

    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        for (const auto& m : ctx->monitors) {
            if (m.left == mi.rcMonitor.left && m.top == mi.rcMonitor.top &&
                m.right == mi.rcMonitor.right && m.bottom == mi.rcMonitor.bottom) {
                info.monitor_index = m.index;
                break;
            }
        }
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (IsIconic(hwnd)) {
            info.display_mode = "minimized";
        } else if ((style & WS_CAPTION) == 0 &&
                   rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top &&
                   rc.right >= mi.rcMonitor.right && rc.bottom >= mi.rcMonitor.bottom) {
            info.display_mode = "borderless";
        } else if (rc.left == mi.rcMonitor.left && rc.top == mi.rcMonitor.top &&
                   rc.right == mi.rcMonitor.right && rc.bottom == mi.rcMonitor.bottom) {
            info.display_mode = "fullscreen-like";
        } else {
            info.display_mode = "windowed";
        }
    }

    ctx->bestAreaByPid[pid] = area;
    ctx->bestByPid[pid] = std::move(info);
    return TRUE;
}

std::vector<WindowProcessInfo> EnumWindowedProcesses(bool gamesOnly) {
    EnumWinCtx ctx;
    ctx.monitors = GetMonitors();
    ctx.fgHwnd = GetForegroundWindow();
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));

    std::vector<WindowProcessInfo> out;
    out.reserve(ctx.bestByPid.size());
    for (auto& kv : ctx.bestByPid) {
        if (gamesOnly && IsLikelySystemOrUtilityExe(to_lower_ascii(kv.second.exe_name))) {
            continue;
        }
        out.push_back(std::move(kv.second));
    }
    std::sort(out.begin(), out.end(), [](const WindowProcessInfo& a, const WindowProcessInfo& b) {
        if (a.attached != b.attached) return a.attached > b.attached;
        if (a.is_foreground != b.is_foreground) return a.is_foreground > b.is_foreground;
        return a.exe_name < b.exe_name;
    });
    return out;
}

bool ResolveTargetPid(const std::string& pidOrName, uint32_t& outPid, std::string& err) {
    if (pidOrName.empty()) {
        err = "Empty target PID/name";
        return false;
    }

    bool allDigits = std::all_of(pidOrName.begin(), pidOrName.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
    if (allDigits) {
        unsigned long val = std::strtoul(pidOrName.c_str(), nullptr, 10);
        if (val == 0) {
            err = "Invalid PID 0";
            return false;
        }
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(val));
        if (!h) {
            err = "Process PID " + pidOrName + " not found or access denied";
            return false;
        }
        CloseHandle(h);
        outPid = static_cast<uint32_t>(val);
        return true;
    }

    std::string needle = to_lower_ascii(pidOrName);
    auto procs = EnumWindowedProcesses(false);
    std::vector<WindowProcessInfo> exact;
    std::vector<WindowProcessInfo> partial;
    for (const auto& p : procs) {
        std::string exeLower = to_lower_ascii(p.exe_name);
        if (exeLower == needle || exeLower == (needle + ".exe")) {
            exact.push_back(p);
        } else if (exeLower.find(needle) != std::string::npos ||
                   to_lower_ascii(p.window_title).find(needle) != std::string::npos) {
            partial.push_back(p);
        }
    }
    if (exact.size() == 1) {
        outPid = exact[0].pid;
        return true;
    }
    if (exact.size() > 1) {
        err = "Multiple processes match '" + pidOrName + "'; specify PID";
        return false;
    }
    if (partial.size() == 1) {
        outPid = partial[0].pid;
        return true;
    }
    if (partial.size() > 1) {
        err = "Ambiguous process match '" + pidOrName + "'; specify PID";
        return false;
    }

    // Fallback: scan all processes via CreateToolhelp32Snapshot
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        std::vector<uint32_t> matches;
        if (Process32FirstW(snap, &pe)) {
            do {
                std::string exeLower = to_lower_ascii(wide_to_utf8(pe.szExeFile));
                if (exeLower == needle || exeLower == (needle + ".exe")) {
                    matches.push_back(pe.th32ProcessID);
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (matches.size() == 1) {
            outPid = matches[0];
            return true;
        }
        if (matches.size() > 1) {
            err = "Multiple processes named '" + pidOrName + "'; specify PID";
            return false;
        }
    }

    err = "No process matching '" + pidOrName + "'";
    return false;
}

static constexpr const wchar_t* kRunRegPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* kRunValueName = L"PadSplit";

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD cbData = 0;
    LONG r = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &cbData);
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && cbData > sizeof(wchar_t));
}

bool SetAutoStartEnabled(bool enable, const std::wstring& exePath) {
    HKEY hKey = nullptr;
    LONG openRes = enable
        ? RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr)
        : RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegPath, 0, KEY_SET_VALUE, &hKey);
    if (openRes != ERROR_SUCCESS) {
        return false;
    }
    LONG r = ERROR_SUCCESS;
    if (enable) {
        std::wstring target = exePath.empty() ? (GetExeDirectory() + L"\\PadSplit.exe") : exePath;
        std::wstring cmd = L"\"" + target + L"\" --minimized";
        DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        r = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), bytes);
    } else {
        r = RegDeleteValueW(hKey, kRunValueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

} // namespace padsplit
