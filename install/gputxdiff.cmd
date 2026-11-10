@echo off
rem gputxdiff - incremental git-bundle transfer via gputxrx (transmit side).
rem Usage: gputxdiff REPO-DIR BRANCH [BRANCH...] BASE-COMMIT [-- TX FLAGS]
rem A leading caret on BASE-COMMIT is accepted and ignored.
rem See gputxdiff.ps1 and README.md for details.
setlocal
rem Installed layout: gputxdiff.impl.ps1. The .ps1 is renamed on install so
rem PowerShell command resolution (which prefers a same-named .ps1 over this
rem .cmd) cannot bypass this launcher and its -ExecutionPolicy Bypass.
rem Source layout (run directly from the repo): gputxdiff.ps1.
set "IMPL=%~dp0gputxdiff.impl.ps1"
if not exist "%IMPL%" set "IMPL=%~dp0gputxdiff.ps1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%IMPL%" %*
exit /b %ERRORLEVEL%
