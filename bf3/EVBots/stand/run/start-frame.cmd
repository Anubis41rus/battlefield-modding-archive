@echo off
rem 238-3 (A1): Lua tick of the server from the engine frame, like VU (EVBOTS_SERVER_TICK=frame). User on site, no auto-click.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -ExtraEnv "EVBOTS_SERVER_TICK=frame"
if errorlevel 1 pause
