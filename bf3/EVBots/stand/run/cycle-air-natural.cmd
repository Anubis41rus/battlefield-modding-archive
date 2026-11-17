@echo off
rem Aircraft + natural round end (22.10, build 211): MP_007 ConquestLarge (jets, helis, AA) ends by tickets, then MP_001 TDM.
rem Checks: missiles reach the mod (flares), no bot body put into an occupied vehicle, score reset, spawns in round 2. About 16 minutes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_007 ConquestLarge0 1;MP_001 TeamDeathMatch0 1" -MaxMin 16 -ExtraEnv "EVBOTS_TESTS_CYCLE=0" %*
