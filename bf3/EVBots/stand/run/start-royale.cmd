@echo off
rem Royale stand profile (parity-safe: vanilla server data, stock Win32 client joinable). The human presses Deploy in the game.
call "%~dp0start.cmd" -ModProfile Royale -NoTests %*
exit /b %errorlevel%
