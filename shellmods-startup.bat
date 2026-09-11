@echo off
setlocal

rem Where shellmods lives. %~dp0 is the directory holding this file, so the
rem copy that sits beside shellmods.exe needs no editing.
rem
rem A copy placed in shell:startup does need editing: %~dp0 would then be the
rem Startup folder. Either hardcode the install directory in that copy, or --
rem better -- put a shortcut to this file in shell:startup rather than a second
rem copy, so there is only ever one of these to maintain.
set "SM=%~dp0"
if "%SM:~-1%"=="\" set "SM=%SM:~0,-1%"
cd /d "%SM%"

rem symgen only needs to re-run when Explorer's binaries change, i.e. after a
rem Windows update. The test below is the same one the shim applies at load
rem time: the PE TimeDateStamp and SizeOfImage recorded in each .sym header,
rem against the binary that .sym was generated from. Skip symgen only when
rem every one of them still matches. Pass /force to resolve symbols regardless.
rem
rem File modification times are no use for this. Windows servicing back-dates
rem the binaries it replaces, so a .sym written weeks later reads as "newer"
rem than a module it no longer matches, and an mtime test skips symgen exactly
rem when it must not.
if /i "%~1"=="/force" goto symgen

powershell -NoProfile -Command "$ErrorActionPreference='Stop'; try { $syms=@(Get-ChildItem -LiteralPath ($env:SM+'\offsets') -Filter *.sym -ErrorAction SilentlyContinue); if (-not $syms) { exit 1 }; $live=@{}; try { foreach ($m in (Get-Process explorer -ErrorAction SilentlyContinue).Modules) { $live[$m.ModuleName]=$m.FileName } } catch {}; foreach ($s in $syms) { $h=Get-Content -LiteralPath $s.FullName -TotalCount 6; $t=@($h -match '^# timestamp='); $z=@($h -match '^# sizeofimage='); $c=@($h -match '^# source='); if (-not $t -or -not $z -or -not $c) { exit 1 }; $ts=[Convert]::ToUInt32($t[0].Substring(12),16); $si=[Convert]::ToUInt32($z[0].Substring(14),16); $p=$live[$s.BaseName]; if (-not $p) { $p=$c[0].Substring(9) }; if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { exit 1 }; $f=[IO.File]::OpenRead($p); try { $b=New-Object IO.BinaryReader($f); $f.Position=0x3C; $e=$b.ReadUInt32(); $f.Position=$e+8; $lt=$b.ReadUInt32(); $f.Position=$e+80; $lz=$b.ReadUInt32() } finally { $f.Close() }; if ($lt -ne $ts -or $lz -ne $si) { exit 1 } } } catch { exit 1 }; exit 0"
if errorlevel 1 goto symgen

echo Offsets still match every module they were generated from; skipping symgen.
goto inject

:symgen
"%SM%\symgen.exe"
if errorlevel 1 echo symgen reported failures -- injecting anyway, the shim refuses stale hooks.

:inject
"%SM%\shellmods.exe" --once
