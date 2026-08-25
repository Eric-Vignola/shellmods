// DLL entry point and mod lifecycle driver.
//
// Windhawk runs this sequence from its engine on a thread it owns. We do it from
// a thread of our own, started from DllMain, because the loader injects us with
// CreateRemoteThread(LoadLibraryW) and therefore DllMain runs under the loader
// lock -- where calling LoadLibrary, initialising MinHook or touching COM would
// deadlock.

#include <windows.h>

#include "shim_runtime.h"

namespace shim {
namespace {

HMODULE g_thisModule;
bool g_hooksLive = false;

void RunInit() {
    const ModCallbacks& cb = g_shellModsCallbacks;

    if (cb.init && !cb.init()) {
        Logf(L"Wh_ModInit returned FALSE, mod is inert");
        return;
    }

    // Windhawk applies every queued hook once, after Wh_ModInit returns.
    if (!ApplyHookOperations()) {
        Logf(L"applying hooks failed, mod is inert");
        return;
    }
    g_hooksLive = true;

    if (cb.afterInit) {
        cb.afterInit();
    }
    Logf(L"initialised");
}

void RunUninit() {
    if (!g_hooksLive) {
        return;
    }
    const ModCallbacks& cb = g_shellModsCallbacks;

    // Upstream order: the mod gets a chance to act while its hooks are still
    // installed, then the hooks come out, then the mod drains whatever is still
    // in flight inside its detours.
    if (cb.beforeUninit) {
        cb.beforeUninit();
    }
    HookShutdownAll();
    if (cb.uninit) {
        cb.uninit();
    }

    g_hooksLive = false;
    Logf(L"unhooked");

    // Deliberately not FreeLibrary. MinHook can retarget a thread sitting on a
    // patched instruction, but it cannot know about a thread executing deep
    // inside one of our detours. Staying mapped and inert costs a couple of
    // megabytes in explorer.exe; unmapping under a live thread costs a crash.
}

// FindFirstChangeNotification watches a directory, not a file, and dist\ holds
// shellmods.log as well as shellmods.ini. Acting on every notification would
// mean our own log write wakes us up, and reacting to that wake-up logs again --
// a loop that never settles. So the notification is only a hint: what decides
// whether anything happened is whether the INI's own timestamp and size moved.
struct IniStamp {
    FILETIME written{};
    DWORD sizeLow = 0;
    DWORD sizeHigh = 0;

    bool operator==(const IniStamp& other) const {
        return written.dwLowDateTime == other.written.dwLowDateTime &&
               written.dwHighDateTime == other.written.dwHighDateTime &&
               sizeLow == other.sizeLow && sizeHigh == other.sizeHigh;
    }
};

IniStamp ReadIniStamp() {
    IniStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(IniPath().c_str(), GetFileExInfoStandard, &data)) {
        stamp.written = data.ftLastWriteTime;
        stamp.sizeLow = data.nFileSizeLow;
        stamp.sizeHigh = data.nFileSizeHigh;
    }
    return stamp;
}

// Watching dist\ rather than waiting on a named event means every process
// hosting this mod picks up a change independently, with no coordination and no
// broadcast to get wrong. Two things are worth reacting to: an edit to
// shellmods.ini, and the DISABLE kill switch appearing.
DWORD WINAPI ControlThread(LPVOID) {
    if (!Startup(g_thisModule)) {
        return 0;
    }

    RunInit();

    HANDLE watch = FindFirstChangeNotificationW(
        g_baseDir.c_str(), FALSE,
        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    if (watch == INVALID_HANDLE_VALUE) {
        Logf(L"watch on %s failed (%lu); settings and DISABLE are static for "
             L"this process",
             g_baseDir.c_str(), GetLastError());
        return 0;
    }

    IniStamp lastStamp = ReadIniStamp();

    for (;;) {
        if (WaitForSingleObject(watch, INFINITE) != WAIT_OBJECT_0) {
            break;
        }

        // Coalesce the burst of notifications a text editor produces when it
        // saves.
        Sleep(250);
        FindNextChangeNotification(watch);

        if (GetFileAttributesW(DisableFlagPath().c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            Logf(L"DISABLE appeared, backing out");
            RunUninit();
            break;
        }

        // Anything other than the INI actually changing -- our own log write,
        // most often -- is not a settings change and must not be treated as one.
        const IniStamp stamp = ReadIniStamp();
        if (stamp == lastStamp) {
            continue;
        }
        lastStamp = stamp;

        // Pick up [shellmods] changes before handing the mod its own. Log in
        // particular has to be re-read here: it is consulted on every hook
        // entry, so a stale value means the log keeps growing after you turn it
        // off.
        ReloadGlobalSettings();

        if (!g_hooksLive) {
            continue;
        }

        const ModCallbacks& cb = g_shellModsCallbacks;
        if (cb.settingsChangedReload) {
            BOOL reload = FALSE;
            cb.settingsChangedReload(&reload);
            if (reload) {
                // This mod wants a full re-init, which Windhawk does by
                // unloading and reloading it. We cannot safely unmap ourselves,
                // so say so plainly instead of half-applying the change.
                Logf(L"mod requested a reload; restart Explorer to apply");
            }
        } else if (cb.settingsChanged) {
            cb.settingsChanged();
        }
    }

    FindCloseChangeNotification(watch);
    return 0;
}

}  // namespace
}  // namespace shim

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    UNREFERENCED_PARAMETER(reserved);

    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            shim::g_thisModule = module;
            DisableThreadLibraryCalls(module);

            HANDLE thread = CreateThread(nullptr, 0, shim::ControlThread,
                                         nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
            break;
        }

        case DLL_PROCESS_DETACH:
            // Nothing. At process exit other threads are already gone or being
            // killed, and unpatching code they may be standing on is a worse
            // outcome than leaving it patched in a dying process.
            break;
    }

    return TRUE;
}
