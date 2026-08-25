// Locating and fetching a module's PDB, without symsrv.dll.
//
// DIA can do this itself if you hand it a "srv*cache*server" search path, but
// that routes through symsrv.dll, which needs its own consent file (symsrv.yes)
// before it will talk to Microsoft's server and fails silently without it. Doing
// the fetch ourselves means one less DLL to redistribute, an explicit error
// message when something goes wrong, and no hidden consent gate -- at the cost
// of about a hundred lines.
#pragma once

#include <windows.h>

#include <string>

namespace symgen {

struct PdbInfo {
    // Basename only, as it appears in the module's CodeView debug record.
    std::wstring name;
    GUID guid{};
    DWORD age = 0;
};

// Reads the CodeView (RSDS) debug record out of a PE file on disk.
bool ReadPdbInfo(const std::wstring& modulePath, PdbInfo* info);

// The symbol server layout, shared with symsrv so an existing cache is reused:
//   <cacheDir>\<name>\<GUID><age>\<name>
std::wstring CachedPdbPath(const std::wstring& cacheDir, const PdbInfo& info);

// Downloads the PDB into the cache if it is not already there. On success,
// `outPath` receives the local path. `error` receives a human-readable reason on
// failure.
bool EnsurePdb(const std::wstring& cacheDir,
               const std::wstring& serverUrl,
               const PdbInfo& info,
               std::wstring* outPath,
               std::wstring* error);

}  // namespace symgen
