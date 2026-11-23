#include "injector_lib.h"
#include "../shared/anticheat.h"
#include "../shared/win_util.h"

#include <tlhelp32.h>
#include <filesystem>

namespace fs = std::filesystem;

namespace padsplit {

bool InjectDllSameArch(uint32_t pid, const std::wstring& dllPath, std::string& err) {
    if (!fs::exists(fs::path(dllPath))) {
        err = "DLL not found: " + wide_to_utf8(dllPath);
        return false;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) {
        DWORD le = GetLastError();
        if (le == ERROR_ACCESS_DENIED) {
            err = "OpenProcess denied (ERROR_ACCESS_DENIED) for PID " + std::to_string(pid) +
                  ": target may be running as Administrator (run PadSplit as Admin) or protected by a kernel anti-cheat driver";
        } else {
            err = "OpenProcess failed for PID " + std::to_string(pid) + " (win32=" + std::to_string(le) + ")";
        }
        return false;
    }

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteBuf = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        err = "VirtualAllocEx failed (win32=" + std::to_string(GetLastError()) + ")";
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteBuf, dllPath.c_str(), bytes, nullptr)) {
        err = "WriteProcessMemory failed (win32=" + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto pLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    if (!pLoadLib) {
        err = "Failed to locate kernel32!LoadLibraryW";
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pLoadLib, remoteBuf, 0, nullptr);
    if (!hThread) {
        err = "CreateRemoteThread failed (win32=" + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    DWORD waitRes = WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (waitRes != WAIT_OBJECT_0) {
        err = "Remote LoadLibraryW timed out";
        return false;
    }
    if (exitCode == 0) {
        err = "Remote LoadLibraryW returned NULL";
        return false;
    }
    return true;
}

static bool SpawnHelperX86Inject(uint32_t pid, const std::wstring& helperPath, const std::wstring& dllPath, std::string& err) {
    if (!fs::exists(fs::path(helperPath))) {
        err = "32-bit helper not found: " + wide_to_utf8(helperPath);
        return false;
    }
    if (!fs::exists(fs::path(dllPath))) {
        err = "32-bit core DLL not found: " + wide_to_utf8(dllPath);
        return false;
    }

    std::wstring cmd = L"\"" + helperPath + L"\" inject " + std::to_wstring(pid) + L" \"" + dllPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(helperPath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = "Failed to launch padsplit_helper_x86.exe (win32=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD w = WaitForSingleObject(pi.hProcess, 6000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (w != WAIT_OBJECT_0) {
        err = "padsplit_helper_x86.exe timed out";
        return false;
    }
    if (exitCode != 0) {
        err = "padsplit_helper_x86.exe failed with exit code " + std::to_string(exitCode);
        return false;
    }
    return true;
}

static void ApplyOptionsToShm(SharedState* shm, const AttachOptions& opts) {
    shm_store32(&shm->enabled, 1);
    shm_store32(&shm->isolation_mode, static_cast<uint32_t>(opts.isolation_mode));
    shm_store32(&shm->borderless_mode, static_cast<uint32_t>(opts.borderless_mode));
    shm_store32(&shm->hide_idle_cursor, opts.hide_idle_cursor ? 1u : 0u);
    shm_store32(&shm->idle_cursor_ms, opts.idle_cursor_ms);
    shm_store32(&shm->dsound_global_focus, opts.dsound_global_focus ? 1u : 0u);
    shm_store32(&shm->spoof_focus, 1);
    shm_store32(&shm->unlock_cursor, 1);
    shm_store32(&shm->request_detach, 0);
}

bool AttachProcess(uint32_t pid, const AttachOptions& opts, std::string& err) {
    std::wstring exePath = GetProcessImagePath(pid);
    AntiCheatResult ac = CheckAntiCheat(pid, exePath);
    if (ac.blocked && !opts.force_bypass_ac_warn) {
        err = "Anti-cheat protection triggered (" + ac.signature + "): " + ac.reason;
        return false;
    }

    wchar_t shmName[64]{};
    MakeShmName(pid, shmName, std::size(shmName));

    // Check if already attached
    {
        HANDLE existing = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
        if (existing) {
            auto* shm = static_cast<SharedState*>(MapViewOfFile(existing, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
            if (shm && shm->magic == PADSPLIT_SHM_MAGIC && shm_load32(&shm->core_ready) == 1) {
                ApplyOptionsToShm(shm, opts);
                if (opts.borderless_mode != BORDERLESS_OFF) {
                    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm->apply_borderless_seq));
                }
                UnmapViewOfFile(shm);
                CloseHandle(existing);
                return true;
            }
            if (shm) UnmapViewOfFile(shm);
            CloseHandle(existing);
        }
    }

    uint32_t arch = GetProcessArch(pid);
    if (arch != 64 && arch != 32) {
        err = "Could not determine architecture for PID " + std::to_string(pid);
        return false;
    }

    HANDLE hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedState), shmName);
    if (!hMap) {
        err = "CreateFileMappingW failed (win32=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    auto* shm = static_cast<SharedState*>(MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shm) {
        err = "MapViewOfFile failed";
        CloseHandle(hMap);
        return false;
    }

    InitSharedStateDefaults(shm, pid);
    ApplyOptionsToShm(shm, opts);

    std::wstring dir = GetExeDirectory();
    bool injected = false;
    if (arch == 64) {
        injected = InjectDllSameArch(pid, dir + L"\\padsplit_core_x64.dll", err);
    } else {
        injected = SpawnHelperX86Inject(pid, dir + L"\\padsplit_helper_x86.exe", dir + L"\\padsplit_core_x86.dll", err);
    }

    if (!injected) {
        UnmapViewOfFile(shm);
        CloseHandle(hMap);
        return false;
    }

    // Wait until core DLL signals core_ready == 1 (so DLL holds an open SHM handle before we close ours)
    bool ready = false;
    for (int i = 0; i < 100; ++i) {
        if (shm_load32(&shm->core_ready) == 1) {
            ready = true;
            break;
        }
        Sleep(50);
    }

    UnmapViewOfFile(shm);
    CloseHandle(hMap);

    if (!ready) {
        err = "Timed out waiting for PadSplit Core DLL initialization";
        return false;
    }
    return true;
}

static bool IsCoreModuleStillLoaded(uint32_t pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            std::string mod = to_lower_ascii(wide_to_utf8(me.szModule));
            if (mod == "padsplit_core_x64.dll" || mod == "padsplit_core_x86.dll") {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool DetachProcess(uint32_t pid, std::string& err) {
    wchar_t shmName[64]{};
    MakeShmName(pid, shmName, std::size(shmName));
    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (!hMap) {
        if (!IsCoreModuleStillLoaded(pid)) {
            err = "Process PID " + std::to_string(pid) + " is not attached";
            return false;
        }
        err = "Shared memory missing for attached PID " + std::to_string(pid);
        return false;
    }

    auto* shm = static_cast<SharedState*>(MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shm) {
        CloseHandle(hMap);
        err = "MapViewOfFile failed during detach";
        return false;
    }

    shm_store32(&shm->request_detach, 1);

    bool detached = false;
    for (int i = 0; i < 80; ++i) {
        if (shm_load32(&shm->core_ready) == 2 && !IsCoreModuleStillLoaded(pid)) {
            detached = true;
            break;
        }
        Sleep(50);
    }

    UnmapViewOfFile(shm);
    CloseHandle(hMap);

    if (!detached && IsCoreModuleStillLoaded(pid)) {
        err = "Timed out waiting for PadSplit Core DLL to unload";
        return false;
    }
    return true;
}

bool ReadProcessShm(uint32_t pid, SharedState& outState, std::string& err) {
    wchar_t shmName[64]{};
    MakeShmName(pid, shmName, std::size(shmName));
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName);
    if (!hMap) {
        err = "Process PID " + std::to_string(pid) + " is not attached";
        return false;
    }

    auto* shm = static_cast<const SharedState*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedState)));
    if (!shm || shm->magic != PADSPLIT_SHM_MAGIC) {
        if (shm) UnmapViewOfFile(shm);
        CloseHandle(hMap);
        err = "Invalid PadSplit shared memory for PID " + std::to_string(pid);
        return false;
    }

    std::memcpy(&outState, shm, sizeof(SharedState));
    UnmapViewOfFile(shm);
    CloseHandle(hMap);
    return true;
}

bool MutateProcessShm(uint32_t pid, const std::function<void(SharedState*)>& fn, std::string& err) {
    wchar_t shmName[64]{};
    MakeShmName(pid, shmName, std::size(shmName));
    HANDLE hMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, shmName);
    if (!hMap) {
        err = "Process PID " + std::to_string(pid) + " is not attached";
        return false;
    }

    auto* shm = static_cast<SharedState*>(MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
    if (!shm || shm->magic != PADSPLIT_SHM_MAGIC) {
        if (shm) UnmapViewOfFile(shm);
        CloseHandle(hMap);
        err = "Invalid PadSplit shared memory for PID " + std::to_string(pid);
        return false;
    }

    fn(shm);
    UnmapViewOfFile(shm);
    CloseHandle(hMap);
    return true;
}

std::vector<uint32_t> ListAttachedPids() {
    std::vector<uint32_t> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            wchar_t shmName[64]{};
            MakeShmName(pe.th32ProcessID, shmName, std::size(shmName));
            HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName);
            if (hMap) {
                auto* shm = static_cast<const SharedState*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(SharedState)));
                if (shm && shm->magic == PADSPLIT_SHM_MAGIC && shm_load32(&shm->core_ready) == 1) {
                    pids.push_back(pe.th32ProcessID);
                }
                if (shm) UnmapViewOfFile(shm);
                CloseHandle(hMap);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

} // namespace padsplit
