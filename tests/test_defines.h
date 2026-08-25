// The test calls Wh_Disasm through the real upstream header, so it needs the
// same three macros a mod gets. See mods/<mod>/mod_defines.h.
#pragma once

#define WH_MOD
#define WH_MOD_ID L"disasm-test"
#define WH_MOD_VERSION L"1.0"
