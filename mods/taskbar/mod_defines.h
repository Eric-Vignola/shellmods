// Force-included ahead of everything else in this project (see the .vcxproj).
//
// Windhawk supplies these three as compiler defines. Doing the same through
// MSBuild's PreprocessorDefinitions means fighting its quote escaping, and
// WH_MOD_ID has to survive as a real wide string literal -- windhawk_utils.h
// concatenates it into a RegisterWindowMessage name. A forced include sidesteps
// the command line entirely.
#pragma once

#define WH_MOD
#define WH_MOD_ID L"taskbar-icon-size"
#define WH_MOD_VERSION L"1.3.7"
