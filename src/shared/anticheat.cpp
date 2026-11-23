#include "anticheat.h"
#include "win_util.h"

#include <filesystem>
#include <string>
#include <vector>

#include <tlhelp32.h>

namespace fs = std::filesystem;

namespace padsplit {

static bool ContainsIgnoreCase(const std::string& haystack, const char* needle) {
    return to_lower_ascii(haystack).find(needle) != std::string::npos;
}

static std::string FindActiveKernelAcDriver() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    HMODULE psapi = nullptr;
    using FnEnumDrivers = BOOL(WINAPI*)(LPVOID*, DWORD, LPDWORD);
    using FnGetDriverName = DWORD(WINAPI*)(LPVOID, LPWSTR, DWORD);

    auto pEnum = k32 ? reinterpret_cast<FnEnumDrivers>(GetProcAddress(k32, "K32EnumDeviceDrivers")) : nullptr;
    auto pName = k32 ? reinterpret_cast<FnGetDriverName>(GetProcAddress(k32, "K32GetDeviceDriverBaseNameW")) : nullptr;
    if (!pEnum || !pName) {
        psapi = LoadLibraryW(L"psapi.dll");
        if (psapi) {
            pEnum = reinterpret_cast<FnEnumDrivers>(GetProcAddress(psapi, "EnumDeviceDrivers"));
            pName = reinterpret_cast<FnGetDriverName>(GetProcAddress(psapi, "GetDeviceDriverBaseNameW"));
        }
    }

    std::string found;
    if (pEnum && pName) {
        std::vector<LPVOID> addrs(1024);
        DWORD needed = 0;
        if (pEnum(addrs.data(), static_cast<DWORD>(addrs.size() * sizeof(LPVOID)), &needed)) {
            if (needed > addrs.size() * sizeof(LPVOID)) {
                addrs.resize(needed / sizeof(LPVOID));
                pEnum(addrs.data(), needed, &needed);
            }
            size_t count = needed / sizeof(LPVOID);
            static const char* kAcDrivers[] = {
                "randgrid.sys",
                "vgk.sys",
                "bedaisy.sys",
                "easyanticheat.sys",
                "easyanticheat_eos.sys",
                "xhunter1.sys",
                "mhyprot2.sys",
                "hoyokprotect.sys",
                "ace-base.sys"
            };
            wchar_t buf[260]{};
            for (size_t i = 0; i < count; ++i) {
                if (pName(addrs[i], buf, 260) > 0) {
                    std::string drv = to_lower_ascii(wide_to_utf8(buf));
                    for (const char* acd : kAcDrivers) {
                        if (drv == acd) {
                            found = drv;
                            break;
                        }
                    }
                    if (!found.empty()) break;
                }
            }
        }
    }
    if (psapi) FreeLibrary(psapi);
    return found;
}

AntiCheatResult CheckAntiCheat(uint32_t pid, const std::wstring& exePath) {
    AntiCheatResult res;
    res.level = "none";

    std::wstring resolvedPath = exePath;
    if (resolvedPath.empty() && pid != 0) {
        resolvedPath = GetProcessImagePath(pid);
    }

    std::string exeBase = to_lower_ascii(GetExeBaseNameUtf8(resolvedPath));

    // 1. Known protected online/anti-cheat game executables
    static const char* kProtectedExes[] = {
        "cod.exe",
        "sp26-cod.exe",
        "valorant-win64-shipping.exe",
        "r5apex.exe",
        "fortniteclient-win64-shipping.exe",
        "cs2.exe",
        "yuanshen.exe",
        "genshinimpact.exe",
        "pubg.exe",
        "tslgame.exe",
        "rainbowsix.exe",
        "rainbowsix_vulkan.exe",
        "destiny2.exe",
        "deltaforceclient-win64-shipping.exe"
    };
    for (const char* p : kProtectedExes) {
        if (exeBase == p) {
            res.blocked = true;
            res.level = "block";
            res.signature = p;
            res.reason = "Target executable is a known anti-cheat protected game: " + exeBase;
            return res;
        }
    }

    // 2. Scan loaded modules in target process (active AC inside target)
    if (pid != 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me)) {
                do {
                    std::string modLower = to_lower_ascii(wide_to_utf8(me.szModule));
                    static const char* kAcModules[] = {
                        "easyanticheat",
                        "beclient",
                        "anticheat",
                        "ace-",
                        "sguard",
                        "gameguard",
                        "xigncode",
                        "vgk"
                    };
                    for (const char* m : kAcModules) {
                        if (ContainsIgnoreCase(modLower, m)) {
                            CloseHandle(snap);
                            res.blocked = true;
                            res.level = "block";
                            res.signature = modLower;
                            res.reason = "Anti-cheat module loaded in target process: " + modLower;
                            return res;
                        }
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
    }

    // 3. Scan active anti-cheat processes (split into transient game-bound vs always-on background services)
    std::string activeBgService;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                static const char* kTransientAcProcs[] = {
                    "easyanticheat.exe",
                    "easyanticheat_eos.exe",
                    "beservice.exe",
                    "gamemon.des",
                    "gamemon64.des"
                };
                static const char* kBackgroundAcServices[] = {
                    "ace-tray.exe",
                    "sguard64.exe",
                    "sguardsvc64.exe",
                    "faceitservice.exe"
                };
                do {
                    std::string procLower = to_lower_ascii(wide_to_utf8(pe.szExeFile));
                    for (const char* ap : kTransientAcProcs) {
                        if (procLower == ap) {
                            CloseHandle(snap);
                            res.blocked = true;
                            res.level = "block";
                            res.signature = procLower;
                            res.reason = "Active anti-cheat process detected on system: " + procLower;
                            return res;
                        }
                    }
                    if (activeBgService.empty()) {
                        for (const char* bg : kBackgroundAcServices) {
                            if (procLower == bg) {
                                activeBgService = procLower;
                                break;
                            }
                        }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    // 4. Scan target & parent directory for AC markers + correlate with active kernel AC drivers / bg services
    std::string dirMarkerFound;
    if (!resolvedPath.empty()) {
        std::error_code ec;
        fs::path p(resolvedPath);
        std::vector<fs::path> dirsToCheck;
        if (p.has_parent_path()) {
            dirsToCheck.push_back(p.parent_path());
            if (p.parent_path().has_parent_path()) {
                dirsToCheck.push_back(p.parent_path().parent_path());
            }
        }

        static const char* kAcDirMarkers[] = {
            "easyanticheat",
            "battleye",
            "gameguard",
            "xigncode",
            "denuvoanticheat",
            "randgrid.sys",
            "easyanticheat_eos.exe",
            "easyanticheat.exe",
            "beservice.exe",
            "start_protected_game.exe",
            "ace-tray.exe",
            "sguard64.exe"
        };

        for (const auto& dir : dirsToCheck) {
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
                std::string nameLower = to_lower_ascii(wide_to_utf8(entry.path().filename().wstring()));
                for (const char* marker : kAcDirMarkers) {
                    if (nameLower == marker) {
                        dirMarkerFound = nameLower;
                        break;
                    }
                }
                if (!dirMarkerFound.empty()) break;
            }
            if (!dirMarkerFound.empty()) break;
        }
    }

    std::string activeDriver = FindActiveKernelAcDriver();
    if (!activeDriver.empty()) {
        // Hard block if an active kernel AC driver matches directory marker or is a game-scoped kernel driver
        if (!dirMarkerFound.empty() || activeDriver == "randgrid.sys" ||
            activeDriver == "bedaisy.sys" || activeDriver.rfind("easyanticheat", 0) == 0) {
            res.blocked = true;
            res.level = "block";
            res.signature = activeDriver;
            res.reason = "Active kernel anti-cheat driver running: " + activeDriver;
            return res;
        }
    }

    if (!activeBgService.empty() && !dirMarkerFound.empty()) {
        res.blocked = true;
        res.level = "block";
        res.signature = activeBgService;
        res.reason = "Active anti-cheat service (" + activeBgService + ") matches game directory marker: " + dirMarkerFound;
        return res;
    }

    if (!dirMarkerFound.empty()) {
        // Static leftover files in directory without active driver/service/module -> soft warning only
        res.blocked = false;
        res.level = "warn";
        res.signature = dirMarkerFound;
        res.reason = "Anti-cheat marker '" + dirMarkerFound + "' found in game directory (no active AC service/driver detected)";
        return res;
    }

    return res;
}

} // namespace padsplit
