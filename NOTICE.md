# Third-party code and licensing

This repository is published under **GPL-3.0** (see `LICENSE`), because it
distributes derivative works of GPL-3.0 mods. The detail is below.

## The three mods

`upstream/` holds pristine copies of three mods from
[ramensoftware/windhawk-mods](https://github.com/ramensoftware/windhawk-mods),
by [m417z](https://github.com/m417z) (Michael Maltsev) / Ramen Software.
`mods/<name>/<name>.wh.cpp` are those same files with the MSVC compatibility
patches in `tools/port_mods.py` applied; `mods/<name>/upstream.patch` shows
exactly what changed.

| Mod | Stated license |
|---|---|
| `taskbar-icon-size` | GPL-3.0 (stated in the file header) |
| `explorer-details-better-file-sizes` | GPL-3.0 (stated in the file header). Its Everything SDK excerpt carries an MIT notice inline. |
| `explorer-context-menu-classic` | No license header in the file; see the upstream repository. |

**Why the whole repository is GPL-3.0:** distributing a derivative work of those
mods means the distributed whole must be offered under GPL-3.0 — including
`shim/`, `loader/` and `symgen/`, which are otherwise original work here. That is
the normal consequence of building on GPL code, and it is deliberate: `LICENSE`
carries the GPL-3.0 text.

## Vendored dependencies

Committed under `vendor/`, unmodified, at the versions below. Only the files the
build actually consumes are kept — headers, sources, licences and READMEs. The
upstream test suites, examples, docs and CI configuration were removed, so these
trees are a subset of upstream rather than a copy of it.

| Library | Version | License | Upstream |
|---|---|---|---|
| MinHook | main | BSD-2-Clause | https://github.com/TsudaKageyu/minhook |
| Zydis | v4.1.1 | MIT | https://github.com/zyantific/zydis |
| Zycore | (Zydis submodule) | MIT | https://github.com/zyantific/zycore-c |

Both are GPL-compatible, so they raise no conflict with the above.

## Windhawk headers

`shim/include/windhawk_api.h`, `windhawk_api_internal.h` and `windhawk_utils.h`
are byte-identical copies of the headers published in the `windhawk-mods`
repository (`.vscode/windhawk_headers_1.7.3/`). They are deliberately unmodified
so that upstream mod sources compile against them without edits — see
`shim/include/UPSTREAM_VERSION.txt`.

## Not redistributed here

`msdia140.dll` is **not** committed. The build copies it from your local Visual
Studio installation, where it is covered by the Visual Studio license and is on
Microsoft's redistributable list. Anyone building this repository supplies their
own copy the same way, via the `symgen` post-build step.

PDBs downloaded by `symgen.exe` are cached outside the repository, under
`%TEMP%\shellmods-symbols`, and are never committed.
