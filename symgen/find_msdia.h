// Locating msdia140.dll without redistributing it.
//
// symgen needs Microsoft's DIA implementation to read a PDB. It ships with every
// Visual Studio installation, and it is on Microsoft's redistributable list --
// but copying it into a public release means publishing a Microsoft binary, and
// this project would rather not. So instead of carrying a copy, we go and find
// the one already on the machine.
#pragma once

#include <string>

namespace symgen {

// Search order, first hit wins:
//   1. `explicitPath`, if the user passed --msdia. An explicit request is never
//      second-guessed: if it is wrong, that is the error you want to see.
//   2. Beside symgen.exe, so dropping a copy in still works.
//   3. Every Visual Studio installation vswhere reports, newest first.
//   4. A shallow scan of the default Visual Studio directories, in case
//      vswhere is missing.
//
// Returns an empty string if none of those turn up a readable file; `tried`
// receives a human-readable account of where we looked, for the error message.
std::wstring LocateMsdia(const std::wstring& explicitPath,
                         const std::wstring& exeDir,
                         std::wstring* tried);

}  // namespace symgen
