@echo off
rem Night autonomous run (18.10): tests + 8 min of play, then the cycle stops client, server and backend.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 8
