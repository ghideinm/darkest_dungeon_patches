@echo off
setlocal
call "%~dp0windows_start.cmd" -Diagnose %*
exit /b %errorlevel%
