@echo off
setlocal
call "%~dp0windows_start.cmd" -ResourcesOnly -Diagnose %*
exit /b %errorlevel%
