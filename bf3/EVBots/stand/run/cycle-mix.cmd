@echo off
rem Rush, SQDM, AirSuperiority, CQL in a row (Rush zone, CrateArmed, jets, level changes) - 20.10
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_001 RushLarge0 1;MP_001 SquadDeathMatch0 1;XP5_002 AirSuperiority0 1;MP_001 ConquestLarge0 1" -MaxMin 30
