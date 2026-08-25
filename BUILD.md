# Building

## Requirements

- **Visual Studio 2022** with the *Desktop development with C++* workload.
  Verified against toolset v143 (14.40.33807) and Windows SDK 10.0.26100.
- The **DIA SDK**, which ships with VS 2022 at
  `%VSINSTALLDIR%DIA SDK` — no separate install. Only `symgen` uses it.
- Nothing else. No CMake, no vcpkg, no Python needed to build. Python 3.7+ is
  needed only to regenerate project files or re-port a mod.

Deliberately *not* required: ATL, MFC, WIL, or the C++/CLI components. The COM
plumbing `symgen` needs is a 40-line local wrapper rather than `CComPtr`, so a
minimal C++ install is enough.

## Build

Open `shellmods.sln`, pick **Release | x64**, Build Solution. Or:

```bash
msbuild shellmods.sln /p:Configuration=Release /p:Platform=x64 /m
```

x64 only — two of the three mods are declared `@architecture x86-64` upstream.

Each project stages itself into `dist\` after building, so a successful build
leaves a ready-to-run layout:

```
dist\
  shellmods.exe        the loader
  symgen.exe           the symbol resolver
  msdia140.dll         copied from your VS install
  shellmods.ini        default settings (checked in, not generated)
  mods\
    taskbar64.dll
    ctxmenu64.dll
    filesizes64.dll
  offsets\             created by symgen.exe
```

## First run

```bash
dist\symgen.exe
```

Resolves the symbol names the mods need against your Windows build. It reads each
module's CodeView record, downloads the matching PDB from
`https://msdl.microsoft.com/download/symbols` over plain HTTPS, and walks it with
DIA. First run downloads a few hundred MB of PDBs into
`%TEMP%\shellmods-symbols`; later runs reuse the cache.

Module paths come from the **running Explorer's** module list, so Explorer should
be running when you do this. That is not a convenience — `Taskbar.View.dll`,
`SystemTray.dll` and `SearchUx.UI.dll` are not in System32, they live in
`SystemApps\MicrosoftWindows.Client.*_cw5n1h2txyewy`, and those paths move
between Windows releases.

Then:

```bash
dist\shellmods.exe --once
```

## Tests

```bash
build\Release\disasm_test.exe
```

One test, guarding the thing most likely to break silently: `taskbar-icon-size`
recovers a struct field offset by running a regex over the *text* of a
disassembled instruction, so the disassembler's exact spelling is part of the
contract. The test asserts Zydis still formats `F2 0F 10 41 50` as
`movsd xmm0, qword ptr [rcx+0x50]` and that the mod's own regex extracts `0x50`
from it.

## Regenerating

The `.vcxproj` files and `shellmods.sln` are generated and **committed** — you
only need these to change the layout:

```bash
python tools\gen_projects.py
```

To pull a newer version of a mod: replace the file in `upstream\`, then

```bash
python tools\port_mods.py
python tools\extract_symreq.py
```

`port_mods.py` re-applies the MSVC compatibility patches, each with an asserted
match count. If upstream changed such that a patch no longer applies, it fails
and names the patch rather than silently producing something unreviewed. It also
rewrites `mods\<mod>\upstream.patch` so the diff stays reviewable.

`extract_symreq.py` re-derives the symbol name lists from the ported sources.

## Notes on non-obvious build settings

Collected in `build\common.props`:

- **`/Zc:preprocessor` is off on purpose.** The vendored `windhawk_api.h` defines
  `Wh_Log` with the GNU `, ##__VA_ARGS__` comma-swallowing extension. MSVC's
  traditional preprocessor accepts it; the conformant one does not. Leaving the
  traditional preprocessor in place is what lets the upstream header stay
  byte-identical.
- **`NOMINMAX`.** `<windows.h>` defines `min`/`max` as macros, and the mods call
  `std::max`. Upstream never hits this because MinGW's headers do not define
  them.
- **`/bigobj`.** `taskbar-icon-size` is one 2,600-line C++/WinRT translation unit
  and exceeds the default section limit.
- **`/FS`.** Required whenever `/MP` is on and several `cl.exe` instances share a
  `.pdb`.
- **Static CRT (`/MT`).** A mod DLL gets loaded into `explorer.exe`. Depending on
  a redistributable VC runtime being installed and version-matched would be one
  more way for injection to fail obscurely.
- **`WH_MOD_ID` arrives via a forced include**, not `/D`. It has to survive as a
  real wide string literal because `windhawk_utils.h` concatenates it into a
  `RegisterWindowMessage` name, and MSBuild's quote escaping in
  `PreprocessorDefinitions` is not reliable enough for that.
- **Suppressed warnings**, both in mod projects only: C4996 (the mods use
  upstream's own deprecated `Wh_SetFunctionHookT`, and `/sdl` makes that an
  error) and, for filesizes, C4005 (it includes both `<ntstatus.h>` and
  `<windows.h>`). Both would otherwise require editing upstream source to silence
  a diagnostic that changes nothing.

## Redistribution

`msdia140.dll` is copied from your Visual Studio installation by the `symgen`
post-build step. It is on Microsoft's redistributable list for Visual Studio, so
shipping it beside `symgen.exe` is fine; the file is not checked in.

`symsrv.dll` is deliberately **not** used. Routing PDB downloads through it would
work, but symsrv gates access to Microsoft's symbol server behind a `symsrv.yes`
consent file and fails silently without one. Fetching over HTTPS directly means
one fewer binary to redistribute and an actual error message when something
fails.
