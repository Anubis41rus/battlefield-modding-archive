@echo off
rem start.ps1 -Force: restart the stand even if a client is connected (only when the user asked for the restart).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -Force
if errorlevel 1 pause
