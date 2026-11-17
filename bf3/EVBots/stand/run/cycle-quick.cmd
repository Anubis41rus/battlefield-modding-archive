@echo off
rem Fast autonomous check (20.10): 2 short levels without a human and without autoclick, about 6-8 minutes.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_001 ConquestSmall0 1;MP_001 TeamDeathMatch0 1" -MaxMin 12 %*
