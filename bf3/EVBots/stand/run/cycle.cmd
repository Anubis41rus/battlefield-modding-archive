@echo off
rem EVBots stand: autonomous run with Deploy autoclick - ONLY when the user is away. Arguments go to cycle.ps1,
rem e.g. cycle.cmd -PlayMin 10. Log: C:\Games\trash\<date>\run\cycle-HHmmss.log
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway %*
