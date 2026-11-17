@echo off
call "%~dp0start.cmd" -ModProfile Royale -NoTests -NoJoin %*
exit /b %errorlevel%
