@echo off
rem gpurxdiff - receive side of gputxdiff (git bundle via gputxrx rx).
rem Usage: gpurxdiff REPO-DIR IDENTIFIER   (transmitter must still be alive)
rem See gpurxdiff.ps1 and README.md for details.
setlocal
rem Installed layout: gpurxdiff.impl.ps1 (renamed on install so PowerShell
rem command resolution cannot shadow this launcher); source layout: .ps1.
set "IMPL=%~dp0gpurxdiff.impl.ps1"
if not exist "%IMPL%" set "IMPL=%~dp0gpurxdiff.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%IMPL%" %*
exit /b %ERRORLEVEL%
