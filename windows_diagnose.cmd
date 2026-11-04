@echo off
setlocal
set "dd_game_root=%~1"
if not defined dd_game_root set "dd_game_root=C:\Program Files (x86)\Steam\steamapps\common\DarkestDungeon"
"%~dp0dd_windows_launcher.exe" --game "%dd_game_root%\_windows\win64\Darkest.exe" --diagnose
set "dd_result=%errorlevel%"
echo Native launcher exit code: %dd_result%
if exist "%dd_game_root%\app.log" copy /Y "%dd_game_root%\app.log" "%~dp0windows_game_app.log" >nul
pause
exit /b %dd_result%
