#include "inject.h"

#include <tlhelp32.h>

#include <algorithm>

namespace loader {

namespace {

std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::towlower);
    return r;
}

std::wstring FormatWin32Error(const wchar_t* what, DWORD code) {
    wchar_t buffer[512];
    _snwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE, L"%s failed (%lu)", what,
                 code);
    return buffer;
}

}  // namespace

std::vector<DWORD> FindProcesses(const std::wstring& imageName) {
    std::vector<DWORD> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return result;
    }

    const std::wstring wanted = ToLower(imageName);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (ToLower(entry.szExeFile) == wanted) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool IsDllLoaded(DWORD pid, const std::wstring& dllBaseName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::wstring wanted = ToLower(dllBaseName);
    bool found = false;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (ToLower(entry.szModule) == wanted) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool InjectDll(DWORD pid,
               const std::wstring& dllPath,
               std::wstring* error,
               bool* alreadyLoaded) {
    const size_t slash = dllPath.find_last_of(L'\\');
    const std::wstring baseName =
        slash == std::wstring::npos ? dllPath : dllPath.substr(slash + 1);

    if (alreadyLoaded) {
        *alreadyLoaded = false;
    }

    if (IsDllLoaded(pid, baseName)) {
        if (alreadyLoaded) {
            *alreadyLoaded = true;
        }
        return true;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ,
                                 FALSE, pid);
    if (!process) {
        *error = FormatWin32Error(L"OpenProcess", GetLastError());
        return false;
    }

    bool ok = false;
    void* remotePath = nullptr;
    HANDLE thread = nullptr;

    do {
        const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
        if (!remotePath) {
            *error = FormatWin32Error(L"VirtualAllocEx", GetLastError());
            break;
        }

        SIZE_T written = 0;
        if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes,
                                &written) ||
            written != bytes) {
            *error = FormatWin32Error(L"WriteProcessMemory", GetLastError());
            break;
        }

        // kernel32 is mapped at the same address in every process of a given
        // architecture in a session, so our own LoadLibraryW address is valid in
        // the target. This is the assumption every same-arch injector makes.
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(kernel32, "LoadLibraryW"));
        if (!loadLibrary) {
            *error = L"cannot find LoadLibraryW";
            break;
        }

        thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath,
                                    0, nullptr);
        if (!thread) {
            *error = FormatWin32Error(L"CreateRemoteThread", GetLastError());
            break;
        }

        // A mod's DllMain only starts a thread and returns, so this should be
        // near-instant. Bound it anyway rather than risk hanging the loader on a
        // wedged process.
        if (WaitForSingleObject(thread, 30000) != WAIT_OBJECT_0) {
            *error = L"remote LoadLibraryW did not complete within 30s";
            break;
        }

        // The thread's exit code is a truncated HMODULE on x64, so it cannot be
        // trusted as a handle. Ask the module list instead.
        if (!IsDllLoaded(pid, baseName)) {
            *error = L"remote LoadLibraryW returned but the DLL is not mapped "
                     L"(missing dependency, or wrong architecture?)";
            break;
        }

        ok = true;
    } while (false);

    if (thread) {
        CloseHandle(thread);
    }
    if (remotePath) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    }
    CloseHandle(process);
    return ok;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    LUID luid{};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Luid = luid;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr,
                                   nullptr) &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(token);
    return ok;
}

}  // namespace loader
