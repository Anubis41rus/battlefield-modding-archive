@echo off
rem Royale parity mode (explicit vanilla server data).
call "%~dp0start.cmd" -ModProfile Royale -NoTests -RoyaleDataDirectory "C:\Games\BF-Portable\Battlefield_3_Server\Data" %*
exit /b %errorlevel%
