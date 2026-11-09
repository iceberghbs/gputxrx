@echo off
rem gpurxdiff - receive side of gputxdiff (git bundle via gputxrx rx).
rem Usage: gpurxdiff REPO-DIR IDENTIFIER   (transmitter must still be alive)
rem See gpurxdiff.ps1 and README.md for details.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gpurxdiff.ps1" %*
exit /b %ERRORLEVEL%
