@echo off
rem Level change without round end (22.10, build 211): MP_001 Rush, then mapList.runNextRound to MP_001 SQDM.
rem On 20.10 09:24 this missed Level:Destroy - spawn exceptions, bots without bodies. No human, no autoclick; about 10 minutes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_001 RushLarge0 1;MP_001 SquadDeathMatch0 1" -MaxMin 12 %*
