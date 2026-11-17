@echo off
rem Full check (18.10): Rush, ConquestLarge with all vehicles and aircraft, SquadDeathMatch; 25 min of play; stops everything.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 25
