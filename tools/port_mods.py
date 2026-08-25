#!/usr/bin/env python3
"""Turn pristine Windhawk mod sources into MSVC-buildable sources.

The three files in upstream/ are byte-for-byte what ramensoftware/windhawk-mods
publishes. Windhawk builds them with clang targeting MinGW; we build them with
MSVC, and a handful of constructs differ. Rather than hand-editing the sources
and losing track of what we changed, every difference is expressed here as a
named patch with an expected hit count.

Run this after downloading a newer upstream mod:

    python tools\\port_mods.py

If upstream changes such that a patch no longer applies, this fails loudly with
the patch name instead of silently producing a source that no longer matches
what we reviewed. That is the whole point: the failure tells you exactly which
assumption broke.

It also writes mods/<mod>/upstream.patch so the diff is reviewable without
running anything.
"""

from __future__ import annotations

import difflib
import io
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


class Patch:
    """One textual substitution with an asserted number of hits."""

    def __init__(self, name: str, pattern: str, replacement: str, count: int,
                 regex: bool = False, flags: int = 0):
        self.name = name
        self.pattern = pattern
        self.replacement = replacement
        self.count = count
        self.regex = regex
        self.flags = flags

    def apply(self, text: str) -> str:
        if self.regex:
            found = len(re.findall(self.pattern, text, self.flags))
            if found != self.count:
                raise SystemExit(
                    f"patch {self.name!r}: expected {self.count} match(es), "
                    f"found {found}. Upstream changed; review before shipping.")
            return re.sub(self.pattern, self.replacement, text, flags=self.flags)

        found = text.count(self.pattern)
        if found != self.count:
            raise SystemExit(
                f"patch {self.name!r}: expected {self.count} match(es), "
                f"found {found}. Upstream changed; review before shipping.")
        return text.replace(self.pattern, self.replacement)


# ---------------------------------------------------------------------------
# taskbar-icon-size
# ---------------------------------------------------------------------------
# Only one incompatibility in 2,643 lines of C++/WinRT: a GCC/clang spelling of
# the current function's signature, used purely to label a log line.
TASKBAR_PATCHES = [
    Patch("pretty-function", "__PRETTY_FUNCTION__", "__FUNCSIG__", 4),
]

# ---------------------------------------------------------------------------
# explorer-details-better-file-sizes
# ---------------------------------------------------------------------------
FILESIZES_PATCHES = [
    # The mod carries a workaround for an llvm-mingw code-generation bug
    # (mstorsjo/llvm-mingw#459) that patched the CRT's setlocale and hooked
    # __cxa_throw to stop a message box at process shutdown. Its own comment
    # notes the workaround is obsolete for Windhawk v1.6+. It is MinGW-specific
    # in every respect -- cxxabi.h, __cxxabiv1::__cxa_throw, MinGW's _errno --
    # so under MSVC it is both unbuildable and unnecessary.
    Patch(
        "drop-mingw-shutdown-workaround",
        r"// A workaround for https://github\.com/mstorsjo/llvm-mingw/issues/459\."
        r".*?\n\}  // namespace ProcessShutdownMessageBoxFix\n",
        "// shellmods: the upstream ProcessShutdownMessageBoxFix namespace was\n"
        "// removed here. It worked around an llvm-mingw codegen bug and is\n"
        "// MinGW-specific (cxxabi.h, __cxxabiv1::__cxa_throw, MinGW _errno).\n"
        "// Upstream notes it is unnecessary for Windhawk v1.6+; it is likewise\n"
        "// unnecessary under MSVC. See tools/port_mods.py.\n",
        1,
        regex=True,
        flags=re.DOTALL,
    ),
    # That namespace was the only reason the mod had a DllMain, and ours lives
    # in the shim, so it has to go regardless.
    Patch(
        "drop-mingw-workaround-dllmain",
        r"\nBOOL WINAPI DllMain\(HINSTANCE hinstDLL, DWORD fdwReason, "
        r"LPVOID lpReserved\) \{.*?\n\}\n",
        "\n// shellmods: upstream's DllMain existed only to drive\n"
        "// ProcessShutdownMessageBoxFix. shim/src/shim_modmain.cpp owns\n"
        "// DllMain for every mod.\n",
        1,
        regex=True,
        flags=re.DOTALL,
    ),
    Patch(
        "drop-mingw-workaround-call",
        "    ProcessShutdownMessageBoxFix::LogErrorIfAny();\n\n",
        "",
        1,
    ),
    # A GCC/clang builtin. MSVC spells it _ReturnAddress, from <intrin.h>.
    Patch("return-address", "__builtin_return_address(0)", "_ReturnAddress()", 2),
    Patch(
        "return-address-include",
        "#include <windhawk_utils.h>\n",
        "#include <windhawk_utils.h>\n\n"
        "// shellmods: for _ReturnAddress, MSVC's __builtin_return_address.\n"
        "#include <intrin.h>\n",
        1,
    ),
    # clang allows a calling convention on a lambda; MSVC does not, and does not
    # need it. A captureless lambda under MSVC converts to a function pointer of
    # whichever calling convention the target expects, so dropping WINAPI leaves
    # the EnumThreadWindows and CreateThread callbacks correct.
    Patch("lambda-cc-bool", ") WINAPI -> BOOL {", ") -> BOOL {", 1),
    Patch("lambda-cc-dword", ") WINAPI -> DWORD {", ") -> DWORD {", 1),
]

# ---------------------------------------------------------------------------
# explorer-context-menu-classic
# ---------------------------------------------------------------------------
# Compiles under MSVC with no changes at all.
CTXMENU_PATCHES: list[Patch] = []

MODS = [
    ("taskbar", "taskbar-icon-size", TASKBAR_PATCHES),
    ("ctxmenu", "explorer-context-menu-classic", CTXMENU_PATCHES),
    ("filesizes", "explorer-details-better-file-sizes", FILESIZES_PATCHES),
]


def main() -> int:
    for folder, upstream_stem, patches in MODS:
        src = ROOT / "upstream" / f"{upstream_stem}.wh.cpp"
        if not src.exists():
            raise SystemExit(f"missing {src}")

        original = src.read_text(encoding="utf-8")
        ported = original
        for patch in patches:
            ported = patch.apply(ported)

        out_dir = ROOT / "mods" / folder
        out_dir.mkdir(parents=True, exist_ok=True)
        out = out_dir / f"{folder}.wh.cpp"
        with io.open(out, "w", encoding="utf-8", newline="") as f:
            f.write(ported)

        diff = "".join(
            difflib.unified_diff(
                original.splitlines(keepends=True),
                ported.splitlines(keepends=True),
                fromfile=f"upstream/{upstream_stem}.wh.cpp",
                tofile=f"mods/{folder}/{folder}.wh.cpp",
            ))
        with io.open(out_dir / "upstream.patch", "w", encoding="utf-8",
                     newline="") as f:
            f.write(diff or "(no changes)\n")

        changed = sum(1 for line in diff.splitlines()
                      if line.startswith(("+", "-"))
                      and not line.startswith(("+++", "---")))
        print(f"{folder:10s} {len(patches)} patch(es), {changed} changed line(s)"
              f" -> {out.relative_to(ROOT)}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
