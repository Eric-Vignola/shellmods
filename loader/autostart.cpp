#include "autostart.h"

#include <cstdio>

namespace loader {

namespace {

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"shellmods";

// Local\ rather than Global\: the loader is per-session, per-user, and so is the
// thing that stops it. A Global\ name would need privileges we otherwise never
// ask for.
constexpr wchar_t kStopEventName[] = L"Local\\shellmods.stop";

std::wstring FormatError(const wchar_t* what, LSTATUS status) {
    wchar_t buffer[256];
    _snwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE, L"%s failed (%ld)", what,
                 status);
    return buffer;
}

std::wstring ThisExePath() {
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        return std::wstring();
    }
    return path;
}

}  // namespace

bool InstallAutostart(std::wstring* error) {
    const std::wstring exePath = ThisExePath();
    if (exePath.empty()) {
        *error = L"cannot determine this executable's path";
        return false;
    }

    // Quoted because the path contains spaces on most machines, and an unquoted
    // Run value is split on the first space -- "C:\Program" becomes the
    // executable and the rest becomes arguments.
    const std::wstring command = L"\"" + exePath + L"\" --watch";

    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                     KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        *error = FormatError(L"RegCreateKeyEx", status);
        return false;
    }

    const DWORD bytes =
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    status = RegSetValueExW(key, kRunValue, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            bytes);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        *error = FormatError(L"RegSetValueEx", status);
        return false;
    }
    return true;
}

bool RemoveAutostart(std::wstring* error) {
    HKEY key = nullptr;
    LSTATUS status =
        RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;  // No Run key at all: nothing to remove.
    }
    if (status != ERROR_SUCCESS) {
        *error = FormatError(L"RegOpenKeyEx", status);
        return false;
    }

    // Value, not key. See the note in autostart.h.
    status = RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);

    if (status == ERROR_FILE_NOT_FOUND) {
        return true;  // Already absent.
    }
    if (status != ERROR_SUCCESS) {
        *error = FormatError(L"RegDeleteValue", status);
        return false;
    }
    return true;
}

bool QueryAutostart(std::wstring* command) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    wchar_t buffer[1024];
    DWORD bytes = sizeof(buffer);
    DWORD type = 0;
    LSTATUS status =
        RegQueryValueExW(key, kRunValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(buffer), &bytes);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }

    size_t chars = bytes / sizeof(wchar_t);
    while (chars > 0 && buffer[chars - 1] == 0) {
        chars--;
    }
    command->assign(buffer, chars);
    return true;
}

bool SetKillSwitch(const std::wstring& baseDir, bool on, std::wstring* error) {
    const std::wstring path = baseDir + L"\\DISABLE";

    if (!on) {
        if (!DeleteFileW(path.c_str()) &&
            GetLastError() != ERROR_FILE_NOT_FOUND) {
            *error = FormatError(L"DeleteFile",
                                 static_cast<LSTATUS>(GetLastError()));
            return false;
        }
        return true;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error =
            FormatError(L"CreateFile", static_cast<LSTATUS>(GetLastError()));
        return false;
    }

    // Content is irrelevant -- every check is for existence -- but a line of
    // explanation costs nothing and this file will be found by someone
    // wondering what it is.
    const char note[] =
        "Delete this file to re-enable shellmods.\r\n"
        "While it exists, injected mods unhook themselves and the loader\r\n"
        "refuses to inject anything new.\r\n";
    DWORD written = 0;
    WriteFile(file, note, sizeof(note) - 1, &written, nullptr);
    CloseHandle(file);
    return true;
}

HANDLE CreateStopEvent() {
    // Manual reset: once stop is requested, every wait should see it.
    return CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
}

bool SignalStop() {
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!event) {
        return false;  // No loader is watching.
    }
    const bool ok = SetEvent(event) != FALSE;
    CloseHandle(event);
    return ok;
}

bool IsWatcherRunning() {
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
    if (!event) {
        return false;
    }
    CloseHandle(event);
    return true;
}

}  // namespace loader
