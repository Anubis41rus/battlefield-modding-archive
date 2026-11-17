@echo off
rem Server-only Royale validation using a separate local dedicated Data copy.
call "%~dp0start-royale-server.cmd" -RoyaleDataDirectory "D:\Games\BF3LegacyLan\server\dedicated\Data" -RoyaleServerDirectory "D:\Games-cache\royale\server-runtime-local-data" %*
exit /b %errorlevel%
