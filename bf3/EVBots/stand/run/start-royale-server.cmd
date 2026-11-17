@echo off
rem Original Royale server check; no client launch and no bot self-tests.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -ModProfile Royale -NoTests -NoJoin %*
exit /b %errorlevel%
