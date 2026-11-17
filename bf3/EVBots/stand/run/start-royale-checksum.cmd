@echo off
rem Output-only native checksum diagnostics; checksum validation remains enabled.
call "%~dp0start.cmd" -ModProfile Royale -NoTests -ChecksumDiagnostics %*
exit /b %errorlevel%
