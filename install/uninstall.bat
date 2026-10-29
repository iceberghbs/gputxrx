@echo off
setlocal EnableExtensions
title gputxrx uninstaller

rem Removes gputxrx from the user PATH and deletes install\bin.

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0remove_from_path.ps1" "%~dp0bin"

if exist "%~dp0bin\gputxrx.exe" del /F /Q "%~dp0bin\gputxrx.exe"
if exist "%~dp0bin" rd /Q "%~dp0bin" 2>nul

echo.
echo [gputxrx] uninstalled. Already-open terminals still see the old
echo PATH until they are closed.
echo.
pause
exit /b 0
