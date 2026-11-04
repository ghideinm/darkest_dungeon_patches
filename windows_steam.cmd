@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0windows_steam.ps1" %*
set "dd_result=%errorlevel%"
if not "%dd_result%"=="0" pause
exit /b %dd_result%
