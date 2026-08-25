// Binds taskbar-icon-size to the shim. See shim/src/shim_runtime.h for why the
// callback set is declared here rather than discovered as DLL exports.
#include <windows.h>

#include "shim_runtime.h"

// Declared with C++ linkage on purpose: the upstream Windhawk headers do not
// declare these, so the definitions in the mod source have C++ linkage, and a
// mismatched extern "C" here would not resolve.
BOOL Wh_ModInit();
void Wh_ModAfterInit();
void Wh_ModBeforeUninit();
void Wh_ModUninit();
void Wh_ModSettingsChanged();

const wchar_t* const g_shellModsModId = L"taskbar-icon-size";

// This mod defines all five.
const shim::ModCallbacks g_shellModsCallbacks = {
    Wh_ModInit,
    Wh_ModAfterInit,
    Wh_ModBeforeUninit,
    Wh_ModUninit,
    Wh_ModSettingsChanged,
    nullptr,
};
