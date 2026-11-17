@echo off
rem EVBots stand: join the client to the RUNNING server (no restart), auto-Deploy, tests, 20 min in game.
rem ONLY when the user is away. Log: C:\Games\trash\<date>\run\play-join-HHmmss.log
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0play-join.ps1" -UserAway %*
