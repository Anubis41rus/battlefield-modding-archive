@echo off
rem Autonomous run with default flags and 20 min of play after the tests (covers a round change with short rounds).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 20
