@echo off
rem Прицельная проверка выхода из AirSuperiority (19.10, сборка 201): XP5_002 AirSuperiority, затем MP_007 TDM — здесь дважды висел сервер.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "XP5_002 AirSuperiority0 1;MP_007 TeamDeathMatch0 1" -MaxMin 15
