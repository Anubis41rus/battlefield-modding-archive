@echo off
rem Natural round end (22.10, build 211): MP_001 TDM ends by tickets, then XP3_Shield ConquestLarge (jets, helis) from MapList.
rem No runNextRound (EVBOTS_TESTS_CYCLE=0), no human, no autoclick; about 14 minutes. Checks score reset and spawns in round 2.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_001 TeamDeathMatch0 1;XP3_Shield ConquestLarge0 1" -MaxMin 14 -ExtraEnv "EVBOTS_TESTS_CYCLE=0" %*
