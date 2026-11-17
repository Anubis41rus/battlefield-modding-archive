@echo off
rem Air/vehicle check (18.10): tests + 12 min of play (bots reach helicopters and jets), then the cycle stops everything.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 12
