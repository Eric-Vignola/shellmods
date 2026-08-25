#!/usr/bin/env python3
"""Emit the .vcxproj files and shellmods.sln.

Eight nearly-identical XML files is exactly the kind of thing that rots when
edited by hand, so they are generated. The generated files ARE committed -- you
do not need Python to build, only to change the project layout.

    python tools\\gen_projects.py
"""

from __future__ import annotations

import io
import pathlib
import uuid

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Stable per-project GUIDs, so regenerating does not churn the solution.
NAMESPACE = uuid.UUID("6f1d4a2e-8c3b-4f1a-9d55-73b0f9a1c001")
VCXPROJ_TYPE = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}"


def guid_for(name: str) -> str:
    return "{" + str(uuid.uuid5(NAMESPACE, name)).upper() + "}"


class Project:
    def __init__(self, name, folder, kind, sources, *,
                 includes=(), defines=(), libs=(), refs=(),
                 target_name=None, target_ext=None, subsystem=None,
                 post_build=None, lib_dirs=(), compile_as_c=False,
                 extra_props="", forced_include=None,
                 disable_warnings=(), exclude_sources=()):
        self.name = name
        self.folder = folder
        self.kind = kind  # StaticLibrary | DynamicLibrary | Application
        self.sources = sources
        self.includes = includes
        self.defines = defines
        self.libs = libs
        self.refs = refs
        self.target_name = target_name or name
        self.target_ext = target_ext
        self.subsystem = subsystem
        self.post_build = post_build
        self.lib_dirs = lib_dirs
        self.compile_as_c = compile_as_c
        self.extra_props = extra_props
        self.forced_include = forced_include
        self.disable_warnings = disable_warnings
        self.exclude_sources = exclude_sources

    @property
    def guid(self) -> str:
        return guid_for(self.name)

    @property
    def path(self) -> pathlib.Path:
        return ROOT / self.folder / f"{self.name}.vcxproj"

    def rel_to_project(self, path: str) -> str:
        """Rewrite a repo-root-relative path as project-relative."""
        depth = len(pathlib.PurePath(self.folder).parts)
        return "\\".join([".."] * depth) + "\\" + path.replace("/", "\\")


# Vendored dependencies. MSBuild expands ** in Include, so these need no
# per-file maintenance when the vendored library is updated.
MINHOOK = Project(
    "minhook", "vendor", "StaticLibrary",
    sources=["minhook\\src\\**\\*.c"],
    includes=["vendor/minhook/include"],
    compile_as_c=True,
)

ZYDIS = Project(
    "zydis", "vendor", "StaticLibrary",
    sources=["zydis\\src\\**\\*.c",
             "zydis\\dependencies\\zycore\\src\\**\\*.c"],
    includes=["vendor/zydis/include",
              "vendor/zydis/src",
              "vendor/zydis/dependencies/zycore/include"],
    defines=["ZYDIS_STATIC_BUILD", "ZYCORE_STATIC_BUILD"],
    compile_as_c=True,
)

SHIM = Project(
    "shim", "shim", "StaticLibrary",
    sources=["src\\*.cpp"],
    # shim_modmain.cpp holds DllMain, and it must NOT end up in the static
    # library. An .obj is only pulled out of a .lib to resolve an undefined
    # symbol, and MSVC's CRT ships its own no-op DllMain stub -- so the linker
    # satisfies DllMain from the CRT and our entry point is silently dropped.
    # The DLL then loads and does absolutely nothing, with no error anywhere.
    # Each mod project compiles shim_modmain.cpp directly instead, where an
    # object linked into the target always beats one offered by a library.
    exclude_sources=["src\\shim_modmain.cpp"],
    includes=["shim/include",
              "shim/src",
              "vendor/minhook/include",
              "vendor/zydis/include",
              "vendor/zydis/dependencies/zycore/include"],
    defines=["ZYDIS_STATIC_BUILD", "ZYCORE_STATIC_BUILD"],
)

SYMGEN = Project(
    "symgen", "symgen", "Application",
    sources=["main.cpp", "pdb_fetch.cpp", "find_msdia.cpp"],
    includes=["$(VSInstallDir)DIA SDK\\include"],
    lib_dirs=["$(VSInstallDir)DIA SDK\\lib\\amd64"],
    defines=["_CONSOLE"],
    subsystem="Console",
    # No msdia140.dll is copied anywhere. symgen locates the copy already
    # installed with Visual Studio at runtime (symgen/find_msdia.cpp), so
    # nothing Microsoft-owned is staged into dist and nothing Microsoft-owned
    # can end up in a published release.
    post_build=[
        'if not exist "$(ShellModsRoot)dist" mkdir "$(ShellModsRoot)dist"',
        'copy /y "$(TargetPath)" "$(ShellModsRoot)dist\\" >nul',
    ],
)

LOADER = Project(
    "loader", "loader", "Application",
    sources=["main.cpp", "inject.cpp", "autostart.cpp"],
    target_name="shellmods",
    defines=["_CONSOLE"],
    subsystem="Console",
    post_build=[
        'if not exist "$(ShellModsRoot)dist" mkdir "$(ShellModsRoot)dist"',
        'copy /y "$(TargetPath)" "$(ShellModsRoot)dist\\" >nul',
    ],
)


def mod(name, mod_id, version, libs, extra_disabled_warnings=()):
    return Project(
        name, f"mods/{name}", "DynamicLibrary",
        sources=[f"{name}.wh.cpp", "mod_glue.cpp",
                 r"..\..\shim\src\shim_modmain.cpp"],
        # $(ProjectDir) so the forced include of mod_defines.h resolves for
        # shim_modmain.cpp too -- it is compiled into this project but lives
        # under shim/src, and forced includes resolve relative to the source
        # file's own directory first.
        includes=["$(ProjectDir)", "shim/include", "shim/src"],
        # WH_MOD, WH_MOD_ID and WH_MOD_VERSION arrive via mod_defines.h instead
        # of /D -- see the comment at the top of that header.
        forced_include="mod_defines.h",
        # C4996: the mods call WindhawkUtils::Wh_SetFunctionHookT, which upstream
        # deprecated in favour of SetFunctionHook but still ships and still
        # works. /sdl promotes C4996 to an error, and the alternative -- editing
        # upstream source to silence upstream's own deprecation notice -- is
        # worse than suppressing it here.
        disable_warnings=[4996] + list(extra_disabled_warnings),
        libs=libs,
        refs=[SHIM, MINHOOK, ZYDIS],
        target_name=f"{name}64",
        subsystem="Windows",
        post_build=[
            'if not exist "$(ShellModsRoot)dist\\mods" '
            'mkdir "$(ShellModsRoot)dist\\mods"',
            'copy /y "$(TargetPath)" "$(ShellModsRoot)dist\\mods\\" >nul',
            # Staging fails while the mod is loaded in Explorer, which is the
            # normal state once you are actually using this. Left alone that
            # surfaces as a bare "copy ... exited with code 1"; say what to do
            # about it instead.
            'if errorlevel 1 (',
            '  echo [shellmods] Could not stage $(TargetFileName): it is '
            'loaded in a running process.',
            '  echo [shellmods] "$(ShellModsRoot)dist\\shellmods.exe" '
            '--status lists which processes hold it.',
            '  echo [shellmods] Restart those processes, then build again. '
            'Note that is not always just Explorer -- filesizes is also '
            'injected into Everything.exe.',
            '  exit /b 1',
            ')',
        ],
    )


# Libraries taken from each mod's upstream @compilerOptions line, translated
# from -lfoo to foo.lib.
MODS = [
    mod("taskbar", "taskbar-icon-size", "1.3.7",
        ["ole32.lib", "oleaut32.lib", "runtimeobject.lib", "shcore.lib",
         "version.lib"]),
    mod("ctxmenu", "explorer-context-menu-classic", "1.0.2",
        ["shlwapi.lib"]),
    # comsuppw is not in upstream's -l list because MinGW's <comutil.h>
    # equivalent is header-only; MSVC's needs _com_issue_error from the COM
    # support library.
    # C4005: this mod includes both <ntstatus.h> and <windows.h>, which define
    # the same STATUS_* macros. It is the standard, harmless collision; the
    # canonical fix is WIN32_NO_STATUS, but that would mean editing upstream
    # source to quiet a warning that changes nothing.
    mod("filesizes", "explorer-details-better-file-sizes", "1.5.1",
        ["ole32.lib", "oleaut32.lib", "propsys.lib",
         "$(ShellModsComSupportLib)"],
        extra_disabled_warnings=[4005]),
]

# Guards the Wh_Disasm text contract that taskbar-icon-size depends on.
DISASM_TEST = Project(
    "disasm_test", "tests", "Application",
    sources=["disasm_test.cpp"],
    includes=["shim/include", "shim/src"],
    forced_include="test_defines.h",
    refs=[SHIM, MINHOOK, ZYDIS],
    defines=["_CONSOLE"],
    subsystem="Console",
)

ALL = [MINHOOK, ZYDIS, SHIM, SYMGEN, LOADER, DISASM_TEST] + MODS

CONFIGS = [("Debug", "x64"), ("Release", "x64")]


def render(project: Project) -> str:
    out = []
    w = out.append
    w('<?xml version="1.0" encoding="utf-8"?>')
    w('<Project DefaultTargets="Build" ToolsVersion="Current" '
      'xmlns="http://schemas.microsoft.com/developer/msbuild/2003">')

    w('  <ItemGroup Label="ProjectConfigurations">')
    for config, platform in CONFIGS:
        w(f'    <ProjectConfiguration Include="{config}|{platform}">')
        w(f'      <Configuration>{config}</Configuration>')
        w(f'      <Platform>{platform}</Platform>')
        w('    </ProjectConfiguration>')
    w('  </ItemGroup>')

    w('  <PropertyGroup Label="Globals">')
    w(f'    <ProjectGuid>{project.guid}</ProjectGuid>')
    w(f'    <RootNamespace>{project.name}</RootNamespace>')
    w(f'    <ProjectName>{project.name}</ProjectName>')
    w('  </PropertyGroup>')

    w('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props" />')
    w('  <PropertyGroup Label="Configuration">')
    w(f'    <ConfigurationType>{project.kind}</ConfigurationType>')
    w('  </PropertyGroup>')
    w(f'  <Import Project="{project.rel_to_project("build/common.props")}" />')
    w('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props" />')

    w('  <PropertyGroup>')
    w(f'    <TargetName>{project.target_name}</TargetName>')
    if project.target_ext:
        w(f'    <TargetExt>{project.target_ext}</TargetExt>')
    w('  </PropertyGroup>')
    if project.extra_props:
        w(project.extra_props)

    w('  <ItemDefinitionGroup>')
    w('    <ClCompile>')
    if project.includes:
        dirs = ";".join(
            inc if inc.startswith("$(") else project.rel_to_project(inc)
            for inc in project.includes)
        w(f'      <AdditionalIncludeDirectories>{dirs};'
          '%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>')
    if project.defines:
        w(f'      <PreprocessorDefinitions>{";".join(project.defines)};'
          '%(PreprocessorDefinitions)</PreprocessorDefinitions>')
    if project.disable_warnings:
        w('      <DisableSpecificWarnings>'
          + ";".join(str(n) for n in project.disable_warnings)
          + ';%(DisableSpecificWarnings)</DisableSpecificWarnings>')
    if project.forced_include:
        w(f'      <ForcedIncludeFiles>{project.forced_include};'
          '%(ForcedIncludeFiles)</ForcedIncludeFiles>')
    if project.compile_as_c:
        w('      <CompileAs>CompileAsC</CompileAs>')
        # The vendored C libraries are not ours to make warning-clean.
        w('      <WarningLevel>Level1</WarningLevel>')
        w('      <SDLCheck>false</SDLCheck>')
        w('      <LanguageStandard_C>stdc11</LanguageStandard_C>')
        # Zydis and Zycore both contain a String.c. Flattening every object into
        # one directory would have the second silently overwrite the first, so
        # mirror the source tree under IntDir.
        w('      <ObjectFileName>$(IntDir)%(RelativeDir)</ObjectFileName>')
    w('    </ClCompile>')
    w('    <Link>')
    if project.libs:
        w(f'      <AdditionalDependencies>{";".join(project.libs)};'
          '%(AdditionalDependencies)</AdditionalDependencies>')
    if project.lib_dirs:
        w(f'      <AdditionalLibraryDirectories>{";".join(project.lib_dirs)};'
          '%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>')
    if project.subsystem:
        w(f'      <SubSystem>{project.subsystem}</SubSystem>')
    w('    </Link>')
    w('  </ItemDefinitionGroup>')

    w('  <ItemGroup>')
    for source in project.sources:
        w(f'    <ClCompile Include="{source}" />')
    for source in project.exclude_sources:
        w(f'    <ClCompile Remove="{source}" />')
    w('  </ItemGroup>')

    if project.refs:
        w('  <ItemGroup>')
        for ref in project.refs:
            rel = project.rel_to_project(f"{ref.folder}/{ref.name}.vcxproj")
            w(f'    <ProjectReference Include="{rel}">')
            w(f'      <Project>{ref.guid}</Project>')
            w('    </ProjectReference>')
        w('  </ItemGroup>')

    w('  <Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets" />')

    if project.post_build:
        # The commands are full of quoted paths, so they have to be escaped for
        # an XML attribute. Newlines become &#xD;&#xA; so Exec runs them as a
        # multi-line batch script.
        script = "&#xD;&#xA;".join(
            line.replace("&", "&amp;").replace("<", "&lt;")
                .replace(">", "&gt;").replace('"', "&quot;")
            for line in project.post_build)
        w('  <Target Name="ShellModsStage" AfterTargets="Build">')
        w(f'    <Exec Command="{script}" />')
        w('  </Target>')

    w('</Project>')
    return "\n".join(out) + "\n"


def render_solution() -> str:
    out = []
    w = out.append
    w("Microsoft Visual Studio Solution File, Format Version 12.00")
    w("# Visual Studio Version 17")
    w("VisualStudioVersion = 17.0.31903.59")
    w("MinimumVisualStudioVersion = 10.0.40219.1")

    for project in ALL:
        rel = f"{project.folder}\\{project.name}.vcxproj".replace("/", "\\")
        w(f'Project("{VCXPROJ_TYPE}") = "{project.name}", "{rel}", '
          f'"{project.guid}"')
        w("EndProject")

    w("Global")
    w("\tGlobalSection(SolutionConfigurationPlatforms) = preSolution")
    for config, platform in CONFIGS:
        w(f"\t\t{config}|{platform} = {config}|{platform}")
    w("\tEndGlobalSection")
    w("\tGlobalSection(ProjectConfigurationPlatforms) = postSolution")
    for project in ALL:
        for config, platform in CONFIGS:
            w(f"\t\t{project.guid}.{config}|{platform}.ActiveCfg = "
              f"{config}|{platform}")
            w(f"\t\t{project.guid}.{config}|{platform}.Build.0 = "
              f"{config}|{platform}")
    w("\tEndGlobalSection")
    w("\tGlobalSection(SolutionProperties) = preSolution")
    w("\t\tHideSolutionNode = FALSE")
    w("\tEndGlobalSection")
    w("EndGlobal")
    return "\n".join(out) + "\n"


def main() -> int:
    for project in ALL:
        project.path.parent.mkdir(parents=True, exist_ok=True)
        with io.open(project.path, "w", encoding="utf-8", newline="\r\n") as f:
            f.write(render(project))
        print(f"  {project.path.relative_to(ROOT)}")

    sln = ROOT / "shellmods.sln"
    with io.open(sln, "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(render_solution())
    print(f"  {sln.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
