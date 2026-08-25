// Process/mod-wide setup, logging, settings and persistent values.
//
// Windhawk keeps all of this in its engine and service; here it collapses into
// one INI file plus one registry key, which is enough for a fixed set of mods
// with a fixed set of settings.

#include <windows.h>

#include <shlwapi.h>

#include <cstdio>
#include <mutex>
#include <new>
#include <string>

#include "shim_runtime.h"

#pragma comment(lib, "shlwapi.lib")

namespace shim {

std::wstring g_modId;
std::wstring g_baseDir;
bool g_logEnabled = false;

namespace {

std::mutex g_logMutex;

std::wstring Join(const std::wstring& dir, PCWSTR leaf) {
    std::wstring r = dir;
    if (!r.empty() && r.back() != L'\\') {
        r += L'\\';
    }
    r += leaf;
    return r;
}

// Windows safe mode. Refusing to load here is the difference between a bad hook
// being an inconvenience and being unrecoverable, since safe mode is the user's
// way back in after we break the shell.
bool InSafeMode() {
    return GetSystemMetrics(SM_CLEANBOOT) != 0;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Setting names arrive as printf-style formats because Windhawk supports
// indexed settings, e.g. Wh_GetIntSetting(L"rules[%d].enabled", i).
std::wstring FormatName(PCWSTR valueName, va_list args) {
    WCHAR buffer[512];
    int n = _vsnwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE, valueName, args);
    return n < 0 ? std::wstring() : std::wstring(buffer);
}

// Settings and values both address into a per-mod namespace. Settings are
// read-only user configuration from the INI; values are mod-owned scratch
// state, which Windhawk persists on the mod's behalf and we keep in the
// registry.
HKEY OpenValuesKey(bool write) {
    std::wstring subKey = L"Software\\shellmods\\" + g_modId;
    HKEY key = nullptr;
    LSTATUS status;
    if (write) {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr,
                                 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    } else {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0,
                               KEY_QUERY_VALUE, &key);
    }
    return status == ERROR_SUCCESS ? key : nullptr;
}

}  // namespace

std::wstring IniPath() {
    return Join(g_baseDir, L"shellmods.ini");
}

std::wstring LogPath() {
    return Join(g_baseDir, L"shellmods.log");
}

std::wstring OffsetsDir() {
    return Join(g_baseDir, L"offsets");
}

std::wstring StorageDir() {
    return Join(Join(g_baseDir, L"storage"), g_modId.c_str());
}

std::wstring DisableFlagPath() {
    return Join(g_baseDir, L"DISABLE");
}

bool Startup(HMODULE thisModule) {
    g_modId = g_shellModsModId;

    WCHAR modulePath[MAX_PATH];
    DWORD len =
        GetModuleFileNameW(thisModule, modulePath, ARRAYSIZE(modulePath));
    if (len == 0 || len >= ARRAYSIZE(modulePath)) {
        return false;
    }

    // ...\dist\mods\taskbar64.dll -> ...\dist
    PathRemoveFileSpecW(modulePath);
    PathRemoveFileSpecW(modulePath);
    g_baseDir = modulePath;

    if (InSafeMode() || FileExists(DisableFlagPath())) {
        return false;
    }

    const std::wstring ini = IniPath();
    ReloadGlobalSettings();

    // Per-mod off switch, so one misbehaving mod can be parked without
    // disturbing the other two.
    if (GetPrivateProfileIntW(g_modId.c_str(), L"Enabled", 1, ini.c_str()) == 0) {
        Logf(L"mod is disabled in shellmods.ini");
        return false;
    }

    return true;
}

void Log(PCWSTR format, va_list args) {
    WCHAR text[2048];
    int n = _vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, format, args);
    if (n < 0) {
        return;
    }

    WCHAR line[2200];
    _snwprintf_s(line, ARRAYSIZE(line), _TRUNCATE, L"[shellmods:%s:%lu] %s\r\n",
                 g_modId.c_str(), GetCurrentProcessId(), text);

    OutputDebugStringW(line);

    if (g_baseDir.empty()) {
        return;
    }

    // Best effort. A mod that cannot write its log must still run, and a
    // failure here must never propagate into the host process.
    std::lock_guard<std::mutex> lock(g_logMutex);
    HANDLE file = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    int chars = lstrlenW(line);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, chars, nullptr, 0,
                                    nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), 0);
        WideCharToMultiByte(CP_UTF8, 0, line, chars, utf8.data(), bytes,
                            nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                  nullptr);
    }
    CloseHandle(file);
}

void Logf(PCWSTR format, ...) {
    va_list args;
    va_start(args, format);
    Log(format, args);
    va_end(args);
}

// Re-reads the settings that live in the [shellmods] section rather than in a
// mod's own section. Called at startup and again whenever the watcher sees the
// INI change -- without the second call, toggling Log would appear to do nothing
// until Explorer restarted, and with logging on the mods write a megabyte a
// minute because every hook entry logs.
void ReloadGlobalSettings() {
    const bool wasEnabled = g_logEnabled;
    g_logEnabled =
        GetPrivateProfileIntW(L"shellmods", L"Log", 0, IniPath().c_str()) != 0;

    if (wasEnabled && !g_logEnabled) {
        // Say so on the way out, so the last line explains the silence.
        Logf(L"logging disabled");
    }
}

}  // namespace shim

using namespace shim;

extern "C" {

BOOL InternalWh_IsLogEnabled(void* mod) {
    UNREFERENCED_PARAMETER(mod);
    return g_logEnabled ? TRUE : FALSE;
}

void InternalWh_Log(void* mod, PCWSTR format, va_list args) {
    UNREFERENCED_PARAMETER(mod);
    Log(format, args);
}

int InternalWh_GetIntSetting(void* mod, PCWSTR valueName, va_list args) {
    UNREFERENCED_PARAMETER(mod);
    std::wstring name = FormatName(valueName, args);
    if (name.empty()) {
        return 0;
    }
    // Defaults live in shellmods.ini, generated from each mod's upstream
    // WindhawkModSettings block. A key missing from the INI reads as 0, which
    // matches how Windhawk treats an unset boolean or numeric setting.
    return static_cast<int>(GetPrivateProfileIntW(g_modId.c_str(), name.c_str(),
                                                  0, IniPath().c_str()));
}

PCWSTR InternalWh_GetStringSetting(void* mod, PCWSTR valueName, va_list args) {
    UNREFERENCED_PARAMETER(mod);
    std::wstring name = FormatName(valueName, args);

    WCHAR buffer[2048];
    DWORD n = 0;
    if (!name.empty()) {
        n = GetPrivateProfileStringW(g_modId.c_str(), name.c_str(), L"", buffer,
                                     ARRAYSIZE(buffer), IniPath().c_str());
    }
    if (n >= ARRAYSIZE(buffer)) {
        n = ARRAYSIZE(buffer) - 1;
    }
    buffer[n] = 0;

    // Contract: the caller releases this with Wh_FreeStringSetting.
    WCHAR* result = new (std::nothrow) WCHAR[static_cast<size_t>(n) + 1];
    if (!result) {
        return nullptr;
    }
    memcpy(result, buffer, (static_cast<size_t>(n) + 1) * sizeof(WCHAR));
    return result;
}

void InternalWh_FreeStringSetting(void* mod, PCWSTR string) {
    UNREFERENCED_PARAMETER(mod);
    delete[] const_cast<WCHAR*>(string);
}

int InternalWh_GetIntValue(void* mod, PCWSTR valueName, int defaultValue) {
    UNREFERENCED_PARAMETER(mod);
    HKEY key = OpenValuesKey(false);
    if (!key) {
        return defaultValue;
    }
    DWORD data = 0;
    DWORD size = sizeof(data);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type,
                                      reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return defaultValue;
    }
    return static_cast<int>(data);
}

BOOL InternalWh_SetIntValue(void* mod, PCWSTR valueName, int value) {
    UNREFERENCED_PARAMETER(mod);
    HKEY key = OpenValuesKey(true);
    if (!key) {
        return FALSE;
    }
    DWORD data = static_cast<DWORD>(value);
    LSTATUS status =
        RegSetValueExW(key, valueName, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? TRUE : FALSE;
}

size_t InternalWh_GetStringValue(void* mod,
                                 PCWSTR valueName,
                                 PWSTR stringBuffer,
                                 size_t bufferChars) {
    UNREFERENCED_PARAMETER(mod);
    if (bufferChars == 0) {
        return 0;
    }
    stringBuffer[0] = 0;

    HKEY key = OpenValuesKey(false);
    if (!key) {
        return 0;
    }
    DWORD size = static_cast<DWORD>(bufferChars * sizeof(WCHAR));
    DWORD type = 0;
    LSTATUS status =
        RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<BYTE*>(stringBuffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        stringBuffer[0] = 0;
        return 0;
    }
    // RegQueryValueExW counts the terminator; the Windhawk contract does not.
    size_t chars = size / sizeof(WCHAR);
    while (chars > 0 && stringBuffer[chars - 1] == 0) {
        chars--;
    }
    stringBuffer[chars] = 0;
    return chars;
}

BOOL InternalWh_SetStringValue(void* mod, PCWSTR valueName, PCWSTR value) {
    UNREFERENCED_PARAMETER(mod);
    HKEY key = OpenValuesKey(true);
    if (!key) {
        return FALSE;
    }
    DWORD bytes = static_cast<DWORD>((lstrlenW(value) + 1) * sizeof(WCHAR));
    LSTATUS status =
        RegSetValueExW(key, valueName, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? TRUE : FALSE;
}

size_t InternalWh_GetBinaryValue(void* mod,
                                 PCWSTR valueName,
                                 void* buffer,
                                 size_t bufferSize) {
    UNREFERENCED_PARAMETER(mod);
    HKEY key = OpenValuesKey(false);
    if (!key) {
        return 0;
    }
    DWORD size = static_cast<DWORD>(bufferSize);
    DWORD type = 0;
    LSTATUS status = RegQueryValueExW(key, valueName, nullptr, &type,
                                      static_cast<BYTE*>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_BINARY) {
        return 0;
    }
    return size;
}

BOOL InternalWh_SetBinaryValue(void* mod,
                               PCWSTR valueName,
                               const void* buffer,
                               size_t bufferSize) {
    UNREFERENCED_PARAMETER(mod);
    HKEY key = OpenValuesKey(true);
    if (!key) {
        return FALSE;
    }
    LSTATUS status = RegSetValueExW(key, valueName, 0, REG_BINARY,
                                    static_cast<const BYTE*>(buffer),
                                    static_cast<DWORD>(bufferSize));
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? TRUE : FALSE;
}

BOOL InternalWh_DeleteValue(void* mod, PCWSTR valueName) {
    UNREFERENCED_PARAMETER(mod);
    std::wstring subKey = L"Software\\shellmods\\" + g_modId;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS) {
        return FALSE;
    }
    LSTATUS status = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? TRUE : FALSE;
}

size_t InternalWh_GetModStoragePath(void* mod,
                                    PWSTR pathBuffer,
                                    size_t bufferChars) {
    UNREFERENCED_PARAMETER(mod);
    std::wstring path = StorageDir();
    if (pathBuffer && bufferChars > path.size()) {
        memcpy(pathBuffer, path.c_str(), (path.size() + 1) * sizeof(WCHAR));
    }
    return path.size();
}

}  // extern "C"
