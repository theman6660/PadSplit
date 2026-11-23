#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <string>

static int InjectX86(DWORD pid, const std::wstring& dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) return 2;

    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteBuf = VirtualAllocEx(hProc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteBuf) {
        CloseHandle(hProc);
        return 3;
    }

    if (!WriteProcessMemory(hProc, remoteBuf, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 4;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto pLoadLib = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    if (!pLoadLib) {
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 5;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pLoadLib, remoteBuf, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 6;
    }

    DWORD w = WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(hProc);

    if (w != WAIT_OBJECT_0) return 7;
    if (exitCode == 0) return 8;
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 4) {
        if (argv) LocalFree(argv);
        return 1;
    }

    int rc = 1;
    if (_wcsicmp(argv[1], L"inject") == 0) {
        DWORD pid = static_cast<DWORD>(_wtoi(argv[2]));
        std::wstring dllPath(argv[3]);
        if (pid != 0 && !dllPath.empty()) {
            rc = InjectX86(pid, dllPath);
        }
    }

    LocalFree(argv);
    return rc;
}
