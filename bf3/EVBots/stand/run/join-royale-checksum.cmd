@echo off
call "%~dp0join-royale-diagnostic.cmd" -ChecksumDiagnostics %*
exit /b %errorlevel%
