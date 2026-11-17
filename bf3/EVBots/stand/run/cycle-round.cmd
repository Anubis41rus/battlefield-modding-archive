@echo off
rem Round change check (18.10): TDM round end, then the next map (MapList) — vehicles and flags after Level:Loaded. Stops everything.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 15
