@echo off
setlocal EnableExtensions
title gputxrx installer

rem ============================================================
rem  Double-click installer for gputxrx.
rem  1. Builds the project if gputxrx.exe is not built yet.
rem  2. Copies gputxrx.exe and the gputxdiff/gpurxdiff wrappers
rem     into install\bin.
rem  3. Adds install\bin to the *user* PATH (no admin needed),
rem     so "gputxrx" works in any NEW terminal window.
rem ============================================================

set "ROOT=%~dp0.."
set "EXE=%ROOT%\build\Release\gputxrx.exe"
set "BIN_DIR=%~dp0bin"

echo [gputxrx] installer started.

if exist "%EXE%" goto :have_exe

echo [gputxrx] gputxrx.exe not found - building (this can take a minute)...
cmake -S "%ROOT%" -B "%ROOT%\build" -G "Visual Studio 18 2026" -A x64
if errorlevel 1 goto :buildfail
cmake --build "%ROOT%\build" --config Release
if errorlevel 1 goto :buildfail

:have_exe
if not exist "%EXE%" goto :buildfail

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
copy /Y "%EXE%" "%BIN_DIR%\gputxrx.exe" >nul
if errorlevel 1 goto :copyfail

echo [gputxrx] copied to "%BIN_DIR%\gputxrx.exe"

rem Incremental git-bundle transfer wrappers (gputxdiff / gpurxdiff).
rem The .ps1 implementations are installed as *.impl.ps1: PowerShell command
rem resolution prefers a same-named .ps1 over the .cmd launcher in the same
rem directory, which would bypass the launcher's -ExecutionPolicy Bypass.
rem The extensionless files are Git Bash shims (bash has no PATHEXT).
if exist "%BIN_DIR%\gputxdiff.ps1" del /F /Q "%BIN_DIR%\gputxdiff.ps1"
if exist "%BIN_DIR%\gpurxdiff.ps1" del /F /Q "%BIN_DIR%\gpurxdiff.ps1"
for %%F in (gputxdiff.cmd gputxdiff gpurxdiff.cmd gpurxdiff) do (
    if not exist "%~dp0%%F" goto :copyfail
    copy /Y "%~dp0%%F" "%BIN_DIR%\%%F" >nul
    if errorlevel 1 goto :copyfail
)
for %%F in (gputxdiff gpurxdiff) do (
    if not exist "%~dp0%%F.ps1" goto :copyfail
    copy /Y "%~dp0%%F.ps1" "%BIN_DIR%\%%F.impl.ps1" >nul
    if errorlevel 1 goto :copyfail
)
echo [gputxrx] copied gputxdiff / gpurxdiff wrappers to "%BIN_DIR%"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0add_to_path.ps1" "%BIN_DIR%"
if errorlevel 1 goto :pathfail

echo.
echo ============================================================
echo  [gputxrx] installed successfully.
echo  Location: %BIN_DIR%\gputxrx.exe
echo.
echo  Open a NEW terminal window and run:
echo      gputxrx --help
echo      gputxrx tx ^<directory^>
echo      gputxrx rx ^<identifier^> --out ^<directory^>
echo      gputxdiff ^<repo-dir^> ^<branch^> [^<branch^>...] ^<base-commit^>
echo      gpurxdiff ^<repo-dir^> ^<identifier^>
echo ============================================================
echo.
pause
exit /b 0

:buildfail
echo.
echo [gputxrx] ERROR: build failed.
echo Make sure CMake and Visual Studio 18 (2026) are installed, then
echo either rerun this script or build manually:
echo     cmake -S . -B build -G "Visual Studio 18 2026" -A x64
echo     cmake --build build --config Release
echo.
pause
exit /b 1

:copyfail
echo.
echo [gputxrx] ERROR: could not copy a required file to "%BIN_DIR%".
echo.
pause
exit /b 1

:pathfail
echo.
echo [gputxrx] ERROR: could not update the user PATH.
echo The binary was still copied to "%BIN_DIR%" - you can add
echo that directory to PATH manually.
echo.
pause
exit /b 1
