// Binds explorer-context-menu-classic to the shim.
#include <windows.h>

#include "shim_runtime.h"

// C++ linkage: see the note in mods/taskbar/mod_glue.cpp.
BOOL Wh_ModInit();
void Wh_ModUninit();
void Wh_ModSettingsChanged();

const wchar_t* const g_shellModsModId = L"explorer-context-menu-classic";

// No Wh_ModAfterInit or Wh_ModBeforeUninit in this mod.
const shim::ModCallbacks g_shellModsCallbacks = {
    Wh_ModInit, nullptr, nullptr, Wh_ModUninit, Wh_ModSettingsChanged, nullptr,
};
