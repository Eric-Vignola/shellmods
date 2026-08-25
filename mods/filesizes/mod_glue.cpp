// Binds explorer-details-better-file-sizes to the shim.
#include <windows.h>

#include "shim_runtime.h"

// C++ linkage: see the note in mods/taskbar/mod_glue.cpp.
BOOL Wh_ModInit();
void Wh_ModUninit();
// This mod uses the reload-requesting form of the settings callback.
BOOL Wh_ModSettingsChanged(BOOL* bReload);

const wchar_t* const g_shellModsModId = L"explorer-details-better-file-sizes";

const shim::ModCallbacks g_shellModsCallbacks = {
    Wh_ModInit, nullptr, nullptr, Wh_ModUninit, nullptr,
    Wh_ModSettingsChanged,
};
