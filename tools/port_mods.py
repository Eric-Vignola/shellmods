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
# Upstream reads five pixel sizes straight out of the INI and uses them without
# looking at them. A key missing from the INI reads as 0 (shim_core.cpp,
# InternalWh_GetIntSetting), and a zero button width written into the live
# taskbar fail-fasts the whole of Explorer -- Windows asserts it in
# TaskbarCollapsibleLayout.h, which takes the shell down with 0xC0000409 and no
# recovery. An INI that is empty, truncated or caught mid-write produces
# exactly that, and copying a file over shellmods.ini while Explorer runs is
# enough to do it, because each mod re-reads the INI as soon as its timestamp
# or size moves.
LOADSETTINGS_ORIGINAL = """void LoadSettings() {
    g_settings.taskbarHeight = Wh_GetIntSetting(L"TaskbarHeight");
    g_settings.iconSize = Wh_GetIntSetting(L"IconSize");
    g_settings.taskbarButtonWidth = Wh_GetIntSetting(L"TaskbarButtonWidth");
    g_settings.iconSizeSmall = Wh_GetIntSetting(L"IconSizeSmall");
    g_settings.taskbarButtonWidthSmall =
        Wh_GetIntSetting(L"TaskbarButtonWidthSmall");
}"""

LOADSETTINGS_GUARDED = r"""// Every setting here is a pixel size that ends up written into the taskbar's
// own layout state, and Windows does not treat those as advisory. The
// collapsible layout introduced for the Windows 11 taskbar fail-fasts the
// whole of Explorer when the button width it reads back is exactly zero:
//
//   Taskbar.View.dll  winrt::Taskbar::implementation::
//                     TaskbarCollapsibleLayoutXamlTraits::
//                     GetExpandedSingleButtonMinWidth(double)
//   -> ...\Src\Components\Taskbar\Taskbar\lib\TaskbarCollapsibleLayout.h:27
//   -> 0xC0000409 subcode 7, no dialog, no recovery, the shell simply goes
//
// Zero is easy to arrive at by accident, because a key missing from the INI
// reads as 0 by design -- see InternalWh_GetIntSetting. So an INI that is
// empty, truncated, or caught halfway through being written hands every one of
// these a zero. That is not hypothetical: copying a file over shellmods.ini
// while Explorer is running does it, since each mod re-reads the INI the
// moment its timestamp or size moves, and a copy truncates before it writes.
//
// A value outside the sane range is therefore refused rather than propagated.
// Before any good value exists that means upstream's default; on a reload it
// means keeping what was working a moment ago, which is what the user still
// wants while they finish editing the file. The ceiling matches the one
// TaskListButton_UpdateVisualStates_Hook already applies to the values it
// reads back out of Windows.
void LoadSettings() {
    struct Setting {
        PCWSTR name;
        int* target;
        int upstreamDefault;
    };

    static constexpr int kMaxSize = 10000;

    const Setting settings[] = {
        {L"TaskbarHeight", &g_settings.taskbarHeight, 48},
        {L"IconSize", &g_settings.iconSize, 24},
        {L"TaskbarButtonWidth", &g_settings.taskbarButtonWidth, 44},
        {L"IconSizeSmall", &g_settings.iconSizeSmall, 16},
        {L"TaskbarButtonWidthSmall", &g_settings.taskbarButtonWidthSmall, 32},
    };

    for (const Setting& setting : settings) {
        const int value = Wh_GetIntSetting(setting.name);
        if (value > 0 && value < kMaxSize) {
            *setting.target = value;
            continue;
        }

        // *target is zero only before the first good load, because nothing
        // else ever writes a rejected value into it.
        const int kept =
            *setting.target > 0 ? *setting.target : setting.upstreamDefault;
        Wh_Log(L"%s=%d is out of range; keeping %d. An empty or half-written "
               L"shellmods.ini reads every key as 0, and a zero button width "
               L"fail-fasts Explorer.",
               setting.name, value, kept);
        *setting.target = kept;
    }
}"""

# Two changes. One is an incompatibility: a GCC/clang spelling of the current
# function's signature, used purely to label a log line. The other is a safety
# fix that upstream does not have -- see the comment the patch inserts.
TASKBAR_PATCHES = [
    Patch("pretty-function", "__PRETTY_FUNCTION__", "__FUNCSIG__", 4),
    Patch("guard-non-positive-sizes", LOADSETTINGS_ORIGINAL,
          LOADSETTINGS_GUARDED, 1),
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
