// Wh_GetUrlContent / Wh_FreeUrlContent.
//
// Windhawk offers mods an HTTP fetch, mainly so that HookSymbols can consult its
// online symbol-cache before falling back to downloading a PDB. Our symbol
// resolution happens offline in symgen.exe, and none of the three mods calls
// this directly, so there is nothing here to implement -- but the symbols still
// have to exist, because the inline wrappers in windhawk_api.h reference them.
//
// Returning null is a valid documented outcome of Wh_GetUrlContent (it is what
// the real engine returns on a failed request), so a mod that did start calling
// this would take its own error path rather than misbehave.

#include <windows.h>

#include "shim_runtime.h"
#include "windhawk_api.h"

extern "C" {

const WH_URL_CONTENT* InternalWh_GetUrlContent(
    void* mod,
    PCWSTR url,
    const WH_GET_URL_CONTENT_OPTIONS* options) {
    UNREFERENCED_PARAMETER(mod);
    UNREFERENCED_PARAMETER(options);
    shim::Logf(L"Wh_GetUrlContent is not implemented in shellmods (url: %s)",
               url ? url : L"(null)");
    return nullptr;
}

void InternalWh_FreeUrlContent(void* mod, const WH_URL_CONTENT* content) {
    UNREFERENCED_PARAMETER(mod);
    UNREFERENCED_PARAMETER(content);
}

}  // extern "C"
