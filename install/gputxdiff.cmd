@echo off
rem gputxdiff - incremental git-bundle transfer via gputxrx (transmit side).
rem Usage: gputxdiff REPO-DIR BRANCH [BRANCH...] BASE-COMMIT [-- TX FLAGS]
rem A leading caret on BASE-COMMIT is accepted and ignored.
rem See gputxdiff.ps1 and README.md for details.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0gputxdiff.ps1" %*
exit /b %ERRORLEVEL%
