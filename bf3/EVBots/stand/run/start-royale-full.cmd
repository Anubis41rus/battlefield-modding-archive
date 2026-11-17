@echo off
rem Royale with prepared Linux overlay data (experimental). Requires matching client assets for checksum parity.
call "%~dp0start.cmd" -ModProfile Royale -NoTests -RoyaleDataDirectory "D:\Games-cache\royale\server-assets-csharp-v3\Data" %*
exit /b %errorlevel%
