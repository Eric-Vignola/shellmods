// Autostart registration, the kill switch, and stopping a running loader.
//
// These exist as flags rather than as a README full of reg.exe incantations for
// one reason above all: the command line
//
//     reg add ...\Run /v shellmods /t REG_SZ /d "\"C:\...\shellmods.exe\" --watch" /f
//
// has two layers of quoting, and getting them wrong writes a value that is
// syntactically fine and silently fails at the next logon. Building the string
// in code and handing it to RegSetValueExW removes that whole class of mistake,
// along with the child process, the shell, and the hardcoded path.
#pragma once

#include <windows.h>

#include <string>

namespace loader {

// Writes HKCU\...\Run\shellmods pointing at this executable with --watch. The
// path comes from GetModuleFileNameW, never from argv, so it is always the
// binary that is actually running and it repairs itself if you move the folder.
bool InstallAutostart(std::wstring* error);

// Removes our single value.
//
// RegDeleteValueW, never RegDeleteKeyW. The Run key belongs to every
// application that starts with Windows; deleting the key rather than the value
// would take all of them with it. That is the one way this could do real damage,
// so it is worth stating as an invariant rather than leaving it implied.
//
// Idempotent: a missing value is success, not an error.
bool RemoveAutostart(std::wstring* error);

// Current value, if any. Returns false when autostart is not registered.
bool QueryAutostart(std::wstring* command);

// The DISABLE kill switch. Creating it makes every already-injected mod unhook
// itself within about a second, and stops the loader injecting anything new.
// Independent of autostart: removing the Run entry does not unload what is
// already inside Explorer.
bool SetKillSwitch(const std::wstring& baseDir, bool on, std::wstring* error);

// ---------------------------------------------------------------------------
// Stopping a resident --watch loader
// ---------------------------------------------------------------------------
//
// Deliberately not taskkill /im shellmods.exe. Matching by image name would
// happily terminate an unrelated process that shares it, and terminating rather
// than signalling kills the loader wherever it happens to be. A named event lets
// the watch loop notice and return through its normal path.

// Opened by --watch, waited on alongside the Explorer process handle.
HANDLE CreateStopEvent();

// Sets that event if a loader is running. Returns false when none is.
bool SignalStop();

// True when a --watch loader exists. Tested by whether the stop event can be
// opened, which is exact -- counting processes named shellmods.exe would also
// match a concurrent --once run, or our own --status process.
bool IsWatcherRunning();

}  // namespace loader
