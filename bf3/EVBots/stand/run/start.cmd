@echo off
rem EVBots stand: double-click (or explorer.exe start.cmd from an admin session). Arguments go to start.ps1,
rem e.g. start.cmd -NoTests. Log: C:\Games\trash\<date>\run\start-HHmmss.log
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" %*
if errorlevel 1 pause
