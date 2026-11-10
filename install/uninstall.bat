@echo off
setlocal EnableExtensions
title gputxrx uninstaller

rem Removes gputxrx from the user PATH and deletes install\bin.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0remove_from_path.ps1" "%~dp0bin"

for %%F in (gputxrx.exe gputxdiff gputxdiff.cmd gputxdiff.ps1 gputxdiff.impl.ps1 gpurxdiff gpurxdiff.cmd gpurxdiff.ps1 gpurxdiff.impl.ps1) do (
    if exist "%~dp0bin\%%F" del /F /Q "%~dp0bin\%%F"
)
if exist "%~dp0bin" rd /S /Q "%~dp0bin" 2>nul

echo.
echo [gputxrx] uninstalled. Already-open terminals still see the old
echo PATH until they are closed.
echo.
pause
exit /b 0
