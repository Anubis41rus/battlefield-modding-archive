@echo off
call "%~dp0start-royale-checksum.cmd" -NoJoin %*
exit /b %errorlevel%
