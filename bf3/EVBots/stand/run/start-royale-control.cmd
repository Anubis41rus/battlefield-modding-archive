@echo off
rem Isolated server control: stock Data, no native BotMod proxy, no client.
call "%~dp0start.cmd" -ModProfile Royale -NoTests -NoJoin -RoyaleServerDirectory "D:\Games-cache\royale\server-control" -RoyaleDataDirectory "C:\Games\BF-Portable\Battlefield_3_Server\Data" %*
exit /b %errorlevel%
