// shellmods.exe -- injects the mod DLLs and keeps them injected.
//
// Windhawk does this from a service, with a hook on CreateProcessInternalW so
// that injection propagates down process trees. We need much less: two of the
// three mods target explorer.exe only, so the whole job is "inject into
// explorer, and do it again when explorer restarts". The third mod is declared
// upstream as @include *, and that mode is available here but off by default --
// see the AllProcesses note in shellmods.ini.
//
// Deliberately not elevated for the default configuration. explorer.exe runs at
// medium integrity as the logged-on user, so CreateRemoteThread into it needs no
// special rights.

#include <windows.h>

#include <shlwapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

#include "autostart.h"
#include "inject.h"

#pragma comment(lib, "shlwapi.lib")

namespace {

struct ModSpec {
    const wchar_t* id;
    const wchar_t* dll;
    // Upstream @include for this mod, and thus what it wants to be injected
    // into. Only filesizes asks for more than the shell.
    bool allProcessesCapable;
};

// Ordered most-important-first, which is also the order the user ranked them.
constexpr ModSpec kMods[] = {
    {L"taskbar-icon-size", L"taskbar64.dll", false},
    {L"explorer-context-menu-classic", L"ctxmenu64.dll", false},
    {L"explorer-details-better-file-sizes", L"filesizes64.dll", true},
};

std::wstring g_baseDir;
bool g_verbose = false;

void Print(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    vwprintf(format, args);
    wprintf(L"\n");
    va_end(args);
}

void Verbose(const wchar_t* format, ...) {
    if (!g_verbose) {
        return;
    }
    va_list args;
    va_start(args, format);
    vwprintf(format, args);
    wprintf(L"\n");
    va_end(args);
}

std::wstring DirOfThisExe() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    PathRemoveFileSpecW(path);
    return path;
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring IniPath() {
    return g_baseDir + L"\\shellmods.ini";
}

bool ModEnabled(const ModSpec& mod) {
    return GetPrivateProfileIntW(mod.id, L"Enabled", 1, IniPath().c_str()) != 0;
}

bool ModWantsAllProcesses(const ModSpec& mod) {
    return mod.allProcessesCapable &&
           GetPrivateProfileIntW(mod.id, L"AllProcesses", 0,
                                 IniPath().c_str()) != 0;
}

// The two conditions under which we refuse to touch anything. Both are checked
// again inside every mod DLL, because the loader is not the only way a DLL can
// end up mapped.
const wchar_t* BlockReason() {
    if (GetSystemMetrics(SM_CLEANBOOT) != 0) {
        return L"Windows is in safe mode";
    }
    if (FileExists(g_baseDir + L"\\DISABLE")) {
        return L"the DISABLE kill switch is present";
    }
    if (GetAsyncKeyState(VK_SHIFT) < 0) {
        return L"Shift is held";
    }
    return nullptr;
}

std::vector<std::wstring> ReadProcessList(const wchar_t* modId) {
    // Comma-separated, e.g. Targets=explorer.exe,notepad.exe
    wchar_t buffer[4096];
    DWORD n = GetPrivateProfileStringW(modId, L"Targets", L"explorer.exe",
                                       buffer, ARRAYSIZE(buffer),
                                       IniPath().c_str());
    buffer[n] = 0;

    std::vector<std::wstring> result;
    std::wstring current;
    for (const wchar_t* p = buffer;; p++) {
        if (*p == L',' || *p == 0) {
            size_t begin = current.find_first_not_of(L" \t");
            size_t end = current.find_last_not_of(L" \t");
            if (begin != std::wstring::npos) {
                result.push_back(current.substr(begin, end - begin + 1));
            }
            current.clear();
            if (*p == 0) {
                break;
            }
            continue;
        }
        current += *p;
    }
    return result;
}

// Everything running that we should not inject into, regardless of settings.
// Mirrors upstream's @exclude list for filesizes plus our own process.
bool IsExcluded(const std::wstring& imageName) {
    static const wchar_t* kExcluded[] = {
        L"shellmods.exe", L"symgen.exe",     L"conhost.exe",
        L"csrss.exe",     L"wininit.exe",    L"services.exe",
        L"lsass.exe",     L"smss.exe",       L"winlogon.exe",
        L"LockApp.exe",   L"SearchHost.exe", L"backgroundTaskHost.exe",
        L"MsMpEng.exe",   L"audiodg.exe",    L"fontdrvhost.exe",
    };
    for (const wchar_t* excluded : kExcluded) {
        if (_wcsicmp(imageName.c_str(), excluded) == 0) {
            return true;
        }
    }
    return false;
}

int InjectMod(const ModSpec& mod) {
    const std::wstring dllPath = g_baseDir + L"\\mods\\" + mod.dll;
    if (!FileExists(dllPath)) {
        Print(L"  %s: %s is missing, skipping", mod.id, mod.dll);
        return 0;
    }

    const bool allProcesses = ModWantsAllProcesses(mod);
    int injected = 0;
    // Counted separately from `injected`: a DLL that was already mapped is the
    // desired end state even though this run did nothing.
    int loaded = 0;

    if (allProcesses) {
        // The upstream @include * behaviour. Reaching anything we do not own
        // needs SeDebugPrivilege, which in turn needs the loader to be elevated;
        // without it this still works for our own processes and quietly skips
        // the rest.
        Print(L"  %s: AllProcesses=1", mod.id);
        if (!loader::EnableDebugPrivilege()) {
            Print(L"  %s: no SeDebugPrivilege; only same-user, same-integrity "
                  L"processes will be reached",
                  mod.id);
        }

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (entry.th32ProcessID <= 4 || IsExcluded(entry.szExeFile)) {
                        continue;
                    }
                    std::wstring error;
                    if (loader::InjectDll(entry.th32ProcessID, dllPath, &error)) {
                        injected++;
                        Verbose(L"    %s (%lu): ok", entry.szExeFile,
                                entry.th32ProcessID);
                    } else {
                        // Overwhelmingly normal: protected processes, other
                        // users, different architecture.
                        Verbose(L"    %s (%lu): %s", entry.szExeFile,
                                entry.th32ProcessID, error.c_str());
                    }
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        Print(L"  %s: injected into %d process(es)", mod.id, injected);
        return injected;
    }

    const std::vector<std::wstring> targets = ReadProcessList(mod.id);
    if (targets.empty()) {
        Print(L"  %s: Targets is empty in shellmods.ini, nothing to do", mod.id);
        return 0;
    }

    for (const std::wstring& target : targets) {
        const std::vector<DWORD> pids = loader::FindProcesses(target);
        if (pids.empty()) {
            Verbose(L"    %s: not running", target.c_str());
            continue;
        }
        for (DWORD pid : pids) {
            std::wstring error;
            bool alreadyLoaded = false;
            if (loader::InjectDll(pid, dllPath, &error, &alreadyLoaded)) {
                loaded++;
                if (alreadyLoaded) {
                    // Say so rather than claiming an injection: this call did
                    // nothing, and if the mod had unhooked itself it is still
                    // unhooked.
                    Print(L"  %s -> %s (%lu): already loaded, nothing to do",
                          mod.id, target.c_str(), pid);
                } else {
                    injected++;
                    Print(L"  %s -> %s (%lu)", mod.id, target.c_str(), pid);
                }
            } else {
                Print(L"  %s -> %s (%lu): %s", mod.id, target.c_str(), pid,
                      error.c_str());
            }
        }
    }
    return loaded;
}

int InjectAll() {
    const wchar_t* blocked = BlockReason();
    if (blocked) {
        Print(L"not injecting: %s", blocked);
        return 0;
    }

    int total = 0;
    for (const ModSpec& mod : kMods) {
        if (!ModEnabled(mod)) {
            Print(L"  %s: disabled in shellmods.ini", mod.id);
            continue;
        }
        total += InjectMod(mod);
    }
    return total;
}

// Explorer restarts more often than people expect: it crashes, it gets restarted
// from Task Manager, and some updates cycle it. Each restart drops every hook we
// installed, so the resident job is to notice and inject again.
//
// Waiting on handles rather than polling means this costs nothing while idle.
// Every wait includes the stop event, so `shellmods.exe --stop` gets a prompt
// exit from whichever wait we happen to be sitting in.
int Watch() {
    HANDLE stopEvent = loader::CreateStopEvent();
    if (!stopEvent) {
        Print(L"cannot create the stop event (%lu); --stop will not work",
              GetLastError());
    }

    Print(L"watching explorer.exe; Ctrl+C or `shellmods.exe --stop` to stop");

    for (;;) {
        std::vector<DWORD> pids = loader::FindProcesses(L"explorer.exe");
        if (pids.empty()) {
            // No shell yet, or between restarts. The only place we poll, and
            // only while Explorer is absent -- and even here the wait doubles as
            // a stop check rather than being a blind Sleep.
            if (stopEvent &&
                WaitForSingleObject(stopEvent, 1000) == WAIT_OBJECT_0) {
                break;
            }
            if (!stopEvent) {
                Sleep(1000);
            }
            continue;
        }

        InjectAll();

        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pids.front());
        if (!process) {
            Print(L"cannot wait on explorer.exe (%lu); falling back to polling",
                  GetLastError());
            if (stopEvent &&
                WaitForSingleObject(stopEvent, 5000) == WAIT_OBJECT_0) {
                break;
            }
            if (!stopEvent) {
                Sleep(5000);
            }
            continue;
        }

        bool stopping = false;
        if (stopEvent) {
            const HANDLE waits[] = {process, stopEvent};
            stopping = WaitForMultipleObjects(2, waits, FALSE, INFINITE) ==
                       WAIT_OBJECT_0 + 1;
        } else {
            WaitForSingleObject(process, INFINITE);
        }
        CloseHandle(process);

        if (stopping) {
            break;
        }

        Print(L"explorer.exe exited; waiting for the new one");
        // Let the new shell finish creating its taskbar before injecting. The
        // mods handle late attach, but there is no reason to race the window.
        if (stopEvent &&
            WaitForSingleObject(stopEvent, 2000) == WAIT_OBJECT_0) {
            break;
        }
        if (!stopEvent) {
            Sleep(2000);
        }
    }

    if (stopEvent) {
        CloseHandle(stopEvent);
    }
    Print(L"stopped watching (mods already injected stay live until Explorer "
          L"restarts; use --disable to unhook them now)");
    return 0;
}

int Usage() {
    wprintf(
        L"shellmods -- load Windhawk mods without Windhawk\n"
        L"\n"
        L"usage: shellmods.exe <command> [--verbose]\n"
        L"\n"
        L"running the mods\n"
        L"  --watch        inject, then stay resident and re-inject whenever\n"
        L"                 explorer.exe restarts. This is the startup mode.\n"
        L"  --once         inject and exit.\n"
        L"  --stop         ask a running --watch loader to exit. Does not\n"
        L"                 unload anything already injected.\n"
        L"  --status       report what is injected and whether autostart is\n"
        L"                 registered. Changes nothing.\n"
        L"\n"
        L"starting with Windows\n"
        L"  --install      register HKCU\\...\\Run -> this exe with --watch.\n"
        L"                 Per-user, no elevation. Re-run after moving the\n"
        L"                 folder; the path is taken from this executable.\n"
        L"  --uninstall    remove that one registry value. Already-injected\n"
        L"                 mods keep running until Explorer restarts.\n"
        L"\n"
        L"turning the mods off\n"
        L"  --disable      create the DISABLE kill switch. Injected mods\n"
        L"                 unhook themselves within about a second, with no\n"
        L"                 Explorer restart.\n"
        L"  --enable       remove it again.\n"
        L"\n"
        L"  --verbose      log every injection attempt, including the expected\n"
        L"                 failures in all-processes mode.\n"
        L"\n"
        L"Holding Shift at startup, or booting into safe mode, also stops the\n"
        L"mods loading -- both are checked inside the mod DLLs, not just here.\n");
    return 2;
}

int Status() {
    Print(L"base dir:  %s", g_baseDir.c_str());

    std::wstring autostart;
    if (loader::QueryAutostart(&autostart)) {
        Print(L"autostart: %s", autostart.c_str());
    } else {
        Print(L"autostart: not registered  (--install to add it)");
    }

    Print(L"watcher:   %s",
          loader::IsWatcherRunning() ? L"running" : L"not running");

    const wchar_t* blocked = BlockReason();
    Print(L"blocked:   %s", blocked ? blocked : L"no");

    for (const ModSpec& mod : kMods) {
        const std::wstring dllPath = g_baseDir + L"\\mods\\" + mod.dll;
        Print(L"%s", mod.id);
        Print(L"  dll:      %s%s", mod.dll,
              FileExists(dllPath) ? L"" : L"  (MISSING)");
        Print(L"  enabled:  %s", ModEnabled(mod) ? L"yes" : L"no");

        int loaded = 0;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (entry.th32ProcessID > 4 &&
                        loader::IsDllLoaded(entry.th32ProcessID, mod.dll)) {
                        loaded++;
                    }
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        Print(L"  injected: %d process(es)", loaded);
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    g_baseDir = DirOfThisExe();

    // At most one command; --verbose is a modifier.
    std::wstring command;

    for (int i = 1; i < argc; i++) {
        const std::wstring arg = argv[i];
        if (arg == L"--verbose" || arg == L"-v") {
            g_verbose = true;
            continue;
        }
        if (arg == L"--watch" || arg == L"--once" || arg == L"--status" ||
            arg == L"--stop" || arg == L"--install" || arg == L"--uninstall" ||
            arg == L"--disable" || arg == L"--enable") {
            if (!command.empty()) {
                Print(L"pick one command: %s and %s were both given",
                      command.c_str(), arg.c_str());
                return 2;
            }
            command = arg;
            continue;
        }
        return Usage();
    }

    if (command.empty()) {
        return Usage();
    }

    // shellmods.ini is the live config and is not tracked in git; only
    // shellmods.default.ini ships. Seed one from the other on first run.
    //
    // The split exists because the two roles conflict: a single tracked file is
    // simultaneously the shipped default and whatever the local machine is tuned
    // to, so any `git add -A` republishes someone's taskbar dimensions as the
    // project's defaults. Copying, rather than reading the default directly as a
    // fallback, means the user has one obvious file to edit.
    if (!FileExists(IniPath())) {
        const std::wstring defaults = g_baseDir + L"\\shellmods.default.ini";
        if (FileExists(defaults) &&
            CopyFileW(defaults.c_str(), IniPath().c_str(), TRUE)) {
            Print(L"created %s from shellmods.default.ini", IniPath().c_str());
            Print(L"edit that file to change settings; it is never overwritten "
                  L"once it exists");
        } else {
            Print(L"warning: neither %s nor shellmods.default.ini found; every "
                  L"setting falls back to 0, which is probably not what you want",
                  IniPath().c_str());
        }
    }

    std::wstring error;

    if (command == L"--install") {
        if (!loader::InstallAutostart(&error)) {
            Print(L"could not register autostart: %s", error.c_str());
            return 1;
        }
        std::wstring registered;
        loader::QueryAutostart(&registered);
        Print(L"autostart registered:");
        Print(L"  %s", registered.c_str());
        Print(L"Takes effect at your next logon. To start it now: "
              L"shellmods.exe --watch");
        return 0;
    }

    if (command == L"--uninstall") {
        if (!loader::RemoveAutostart(&error)) {
            Print(L"could not remove autostart: %s", error.c_str());
            return 1;
        }
        Print(L"autostart removed.");
        Print(L"Mods already injected keep running until Explorer restarts; "
              L"--disable unhooks them now.");
        return 0;
    }

    if (command == L"--disable") {
        if (!loader::SetKillSwitch(g_baseDir, true, &error)) {
            Print(L"could not create the kill switch: %s", error.c_str());
            return 1;
        }
        Print(L"kill switch created: %s\\DISABLE", g_baseDir.c_str());
        Print(L"Injected mods unhook themselves within about a second.");
        return 0;
    }

    if (command == L"--enable") {
        if (!loader::SetKillSwitch(g_baseDir, false, &error)) {
            Print(L"could not remove the kill switch: %s", error.c_str());
            return 1;
        }
        Print(L"kill switch removed.");
        // Deliberately not "run --once": a DLL that is already mapped cannot be
        // re-initialised by injecting again. LoadLibrary would just bump its
        // refcount without running DllMain, so the mod would stay unhooked while
        // the loader cheerfully reported success.
        Print(L"Mods that already unhooked stay inert -- their DLL is still "
              L"mapped, so re-injecting does nothing.");
        Print(L"Restart Explorer to load them again.");
        return 0;
    }

    if (command == L"--stop") {
        if (!loader::SignalStop()) {
            Print(L"no --watch loader is running.");
            return 1;
        }
        Print(L"asked the running loader to stop.");
        return 0;
    }

    if (command == L"--status") {
        return Status();
    }

    if (command == L"--watch") {
        return Watch();
    }

    // --once. Success means the mods ended up loaded, which includes the case
    // where they already were -- re-running this must not look like a failure.
    const int loaded = InjectAll();
    Print(L"%d mod instance(s) loaded", loaded);
    return loaded > 0 ? 0 : 1;
}
