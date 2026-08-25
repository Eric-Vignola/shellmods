@echo off
rem Re-resolve every symbol offset against the currently installed Windows, then
rem restart Explorer so the mods pick it up.
rem
rem This is the whole maintenance story: run it after a Windows update that
rem touches the shell. No compiler needed -- the offsets are data, not code.
setlocal
set DIST=%~dp0..\dist

if not exist "%DIST%\symgen.exe" (
    echo Build the solution first: symgen.exe is missing from %DIST%
    exit /b 1
)

echo Resolving symbols...
"%DIST%\symgen.exe" --out "%DIST%\offsets"
if errorlevel 1 (
    echo.
    echo symgen failed. The mods will refuse to hook rather than use stale
    echo offsets, so nothing is broken -- but nothing is applied either.
    exit /b 1
)

echo.
echo Restarting Explorer...
taskkill /f /im explorer.exe >nul 2>&1
rem Explorer is normally restarted automatically; start it if it was not.
timeout /t 3 /nobreak >nul
tasklist /fi "imagename eq explorer.exe" | find /i "explorer.exe" >nul || start "" explorer.exe

echo.
echo Done. Check %DIST%\shellmods.log if something looks wrong
echo (set Log=1 under [shellmods] in shellmods.ini first).
