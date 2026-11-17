@echo off
rem 23.10 (build 211): XP5_002 ConquestLarge0 with a human, full tickets (long round: jets lost -> mod respawns them).
rem MapList.txt must already be "XP5_002 ConquestLarge0 1" (backup: backup\2026-10-23\MapList-pre-xp5.txt). Log: trash\<date>\run\start-*.log
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -StandTickets 100 %*
if errorlevel 1 pause
