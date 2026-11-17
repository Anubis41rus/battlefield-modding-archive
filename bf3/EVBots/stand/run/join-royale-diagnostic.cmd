@echo off
rem Client resource diagnostics; normal Royale join remains quiet.
call "%~dp0join-royale.cmd" -VerboseClient %*
exit /b %errorlevel%
