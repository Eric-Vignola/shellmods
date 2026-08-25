#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace loader {

// Loads `dllPath` into `pid` via CreateRemoteThread(LoadLibraryW). Requires the
// target to be the same architecture and reachable with our token, which for
// explorer.exe running as the same user needs no elevation at all.
//
// Returns true if the DLL is loaded when we are done. `alreadyLoaded` (optional)
// distinguishes "we loaded it" from "it was already there" -- worth reporting
// separately, because the second case does nothing at all. A DLL that has been
// mapped once cannot be re-initialised by injecting again: LoadLibrary just
// bumps its refcount and DllMain does not run a second time. Restoring a mod
// that has unhooked itself needs Explorer restarted, not another injection.
bool InjectDll(DWORD pid,
               const std::wstring& dllPath,
               std::wstring* error,
               bool* alreadyLoaded = nullptr);

// True if `pid` already has a module with this base name mapped.
bool IsDllLoaded(DWORD pid, const std::wstring& dllBaseName);

// PIDs of every running process with this image name, newest last.
std::vector<DWORD> FindProcesses(const std::wstring& imageName);

// Enables SeDebugPrivilege. Needed only to reach processes we do not own, which
// in practice means the all-processes mode of the filesizes mod.
bool EnableDebugPrivilege();

}  // namespace loader
