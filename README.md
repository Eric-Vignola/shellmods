# shellmods

Three Windhawk mods, running without Windhawk.

| Mod | Upstream | What it does |
|---|---|---|
| `taskbar64.dll` | [taskbar-icon-size](https://windhawk.net/mods/taskbar-icon-size) | Taskbar height and icon size |
| `ctxmenu64.dll` | [explorer-context-menu-classic](https://windhawk.net/mods/explorer-context-menu-classic) | Classic right-click menu, no "Show more options" |
| `filesizes64.dll` | [explorer-details-better-file-sizes](https://windhawk.net/mods/explorer-details-better-file-sizes) | Folder sizes and MB/GB in Explorer details |

Windows 11, x86-64. Built with Visual Studio 2022.

## The idea

A Windhawk mod is an ordinary DLL compiled against a small C API — about twenty
`InternalWh_*` functions. Everything a mod can do goes through that boundary. So
the mods here are **upstream source, essentially unmodified**, and everything
this project adds sits behind that boundary:

```
mods/*/​*.wh.cpp          upstream mod source (see upstream.patch for the diff)
shim/include/            the three upstream Windhawk headers, byte-identical
        ↕ ---------------- InternalWh_* : the entire contract
shim/src/                our implementation of it
  shim_hooks.cpp           Wh_SetFunctionHook      -> MinHook
  shim_symbols.cpp         WindhawkUtils::HookSymbols -> offsets/*.sym
  shim_disasm.cpp          Wh_Disasm               -> Zydis
  shim_core.cpp            settings, logging       -> shellmods.ini
  shim_modmain.cpp         DllMain and mod lifecycle
symgen/                  resolves symbol names to offsets, offline
loader/                  injects the DLLs and keeps them injected
```

Keeping the headers verbatim is the point: updating a mod is
`curl` + `python tools\port_mods.py`, not a merge.

How much the mods actually had to change to build under MSVC instead of
clang/MinGW:

| Mod | Changed lines | What |
|---|---|---|
| ctxmenu | **0** | compiles unmodified |
| taskbar | 4 | `__PRETTY_FUNCTION__` → `__FUNCSIG__`, in log statements |
| filesizes | ~10 + a deletion | `__builtin_return_address` → `_ReturnAddress`, two clang-only lambda calling conventions, and an obsolete MinGW codegen workaround that upstream itself marks unnecessary |

Every one of those is expressed as an asserted patch in
[tools/port_mods.py](tools/port_mods.py), so a future upstream change that
invalidates one fails loudly instead of silently.

## Running these commands

Every example below uses a relative path with a separator (`dist\symgen.exe`,
`.\shellmods.exe`), because that form works in both shells.

If you write an absolute path in **PowerShell**, it needs the call operator:

```powershell
& "C:\path	o\shellmods.exe" --install
```

Without the `&`, PowerShell reads the quoted path as a string expression and
then `--install` as the decrement operator, and you get
`Unexpected token 'install' in expression or statement`. A bare `shellmods.exe`
does not work either — PowerShell does not search the current directory. In
`cmd.exe` both forms are fine.

## Install

Build the solution, then:

```bash
dist\symgen.exe
```

That reads `dist\symreq\*.symreq`, resolves those symbols against **your**
Windows build (see below), and writes `dist\offsets\*.sym`.

It needs `msdia140.dll`, which ships with any Visual Studio 2015 or newer,
including the free Build Tools. It is found automatically; `--msdia <path>`
overrides. Then:

```bash
dist\shellmods.exe --once
```

To make it permanent:

```bash
dist\shellmods.exe --install
```

One executable, one startup entry. No service, and **no elevation** — Explorer
runs as you, so injecting into it needs no special rights. The only thing that
needs admin is `AllProcesses=1` for the filesizes mod (see below).

`--watch` stays resident to re-inject when Explorer restarts, which it does more
often than you'd think. Every wait is on a handle rather than a poll, so idle
cost is nil.

## Commands

| | |
|---|---|
| `--once` | inject and exit |
| `--watch` | inject, then stay resident and re-inject when Explorer restarts |
| `--stop` | ask a running `--watch` loader to exit |
| `--status` | what is injected, whether autostart is set — changes nothing |
| `--install` | register the HKCU Run entry pointing at this exe |
| `--uninstall` | remove that one registry value |
| `--disable` | create the `DISABLE` kill switch |
| `--enable` | remove it |
| `--verbose` | log every injection attempt |

`--install` takes the path from `GetModuleFileNameW`, so it is always the binary
actually running and re-running it after moving the folder fixes the entry. It
writes `HKCU` only, and `--uninstall` deletes the single value `shellmods` —
never the key, which would take every other application's autostart with it.

`--stop` signals a named event rather than matching on `shellmods.exe`, so it
cannot terminate an unrelated process, and the loader exits through its normal
path instead of being killed mid-operation.

## Turning it off

```bash
dist\shellmods.exe --disable
```

That makes every already-injected mod **unhook itself within a second**, without
restarting Explorer — each mod watches `dist\` for changes. It also stops the
loader injecting anything new. `--enable` removes the flag.

One asymmetry worth knowing: `--enable` does not bring the mods back. A DLL that
is already mapped cannot be re-initialised by injecting again — `LoadLibrary`
just bumps its refcount and `DllMain` does not run a second time — so restoring
them means restarting Explorer. The loader says so rather than reporting a
success that did nothing.

The other two escape hatches are checked inside the mod DLLs, not just in the
loader: hold **Shift** while the loader starts, or boot into **safe mode**. Safe
mode matters because it is your way back in if a hook ever does break the shell.

## Settings

Edit `dist\shellmods.ini`. Changes apply live — no restart. Every value is one of
the mod's upstream Windhawk settings, pre-filled with upstream's default.

There are two files, on purpose:

| | |
|---|---|
| `shellmods.default.ini` | shipped defaults, tracked in git. Don't edit. |
| `shellmods.ini` | yours. Gitignored, so local tuning is never published. Created from the default on first run and never overwritten after. |

One trap worth repeating from the file's own header: **a key that is absent reads
as 0**, not as the upstream default. Deleting a line sets it to zero.

### Folder sizes (the filesizes mod)

This mod has two independent features, and only one is on by default:

- **MB/GB instead of Explorer's KB-for-everything** — `disableKbOnlySizes=1`,
  on out of the box. Works with no symbols and no extra software.
- **Folder sizes in the Size column** — `calculateFolderSizes`, which upstream
  ships as `disabled` because calculating them by walking the tree is slow.

If you have [Everything](https://www.voidtools.com/) running, set
`calculateFolderSizes=everything` and folder sizes become instant — Explorer
queries Everything's index over IPC instead of walking anything. Everything needs
**Index file size** and **Index folder size** both ticked under
*Tools → Options → Indexes*, and the Lite build will not work.

Two things about that mode:

- It needs Explorer restarted to take effect. The setting decides whether the mod
  hooks `windows.storage.dll` during init, and those hooks cannot be added later.
  The shim says so in the log rather than pretending it applied.
- Add `Everything.exe` to that mod's `Targets`. With folder sizes coming from
  Everything, using *Open Path* from Everything's own window makes Everything
  call Explorer, which calls back into Everything — the mod hooks
  `SHOpenFolderAndSelectItems` inside Everything.exe to move that onto a worker
  thread and avoid the deadlock.

The loader will report `OpenProcess failed (5)` for Everything's session-0
service process. That is expected and harmless: the service has no UI thread, so
there is nothing there to hook.

### Logging

Two levels, on purpose:

- The shim's own diagnostics — which symbol files loaded, and whether any have
  gone stale — **always** go to `dist\shellmods.log`. A handful of lines per
  injection. These are never suppressed, because a stale symbol file is exactly
  the thing you need told about when the mods quietly stop working.
- `Log=1` under `[shellmods]` additionally traces every hook the mods take. Only
  turn this on to diagnose something: the taskbar mod's hooks run on essentially
  every paint, so it produces around **a megabyte a minute**. It takes effect
  immediately, both directions.

## Windows updates: the part that needs your attention

The taskbar and filesizes mods hook functions that Microsoft does not export.
They are found by mangled name in the module's PDB — things like:

```
private: double __cdecl winrt::SystemTray::implementation::SystemTrayController::GetFrameSize(...)
```

`symgen.exe` resolves those names to offsets by downloading the matching PDB from
Microsoft's public symbol server and reading it with the DIA SDK. It records the
`TimeDateStamp` and `SizeOfImage` of the exact binary it read.

**When Windows updates those binaries, the recorded offsets are wrong.** The shim
compares fingerprints at load time, and on a mismatch it **refuses every symbol
hook in that module** and logs why. It does not hook a stale address — that is
how you get a shell that crashes in a loop.

So after a Patch Tuesday that touches the shell, the mods go quiet and you run:

```bash
dist\symgen.exe
```

…then restart Explorer. No compiler needed, no rebuild — the offsets are data,
not code.

`symgen` finds module paths by asking the **running Explorer** what it has
mapped, which matters because half of these DLLs are not in System32:
`Taskbar.View.dll` and `SystemTray.dll` live under
`SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy`, and Microsoft moves them
between releases.

## What you give up versus Windhawk

Worth being straight about. Windhawk's engine does several things this does not:

- **No hook chaining.** MinHook cannot layer two hooks on one function. Fine
  here — the three mods hook disjoint targets — but it constrains adding a
  fourth.
- **No safe unload.** Windhawk scans thread call stacks to know when it can
  remove a hook that a thread is still executing inside. We don't, so `DISABLE`
  unhooks and then leaves the DLL mapped and inert rather than unmapping it.
- **No crash recovery.** Windhawk disables a mod that repeatedly crashes its
  host. Here, that is what `DISABLE` and safe mode are for.
- **No settings UI**, no marketplace, no in-process compiler.
- **`Wh_GetUrlContent` is a stub** and symbol enumeration via
  `Wh_FindFirstSymbol` only covers names in the `.sym` file. Neither is used by
  these three mods.

Also: an unsigned executable calling `CreateRemoteThread` on `explorer.exe` looks
exactly like malware, because that is the technique malware uses. Expect
Defender to take an interest, and plan on a path exclusion or your own signing
certificate.

## Licensing

The three mods are upstream work by [m417z](https://github.com/m417z) /
[Ramen Software](https://github.com/ramensoftware), taken from
[windhawk-mods](https://github.com/ramensoftware/windhawk-mods).
`taskbar-icon-size` states GPL-3.0 in its source header; that governs any
distribution of the derived DLL. The Windhawk headers in `shim/include` are
likewise upstream, vendored unmodified.

Vendored: [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause),
[Zydis](https://github.com/zyantific/zydis) + Zycore (MIT).

`msdia140.dll` is redistributable under the Visual Studio license and is copied
from your own VS install by the build. See [BUILD.md](BUILD.md).
