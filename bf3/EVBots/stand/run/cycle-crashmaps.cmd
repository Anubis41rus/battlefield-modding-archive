@echo off
rem Карты, на которых у друга падала старая сборка (25.10): Operation Firestorm (MP_012), Nebandan Flats (XP5_002),
rem Kharg Island (MP_018) — CQL, без человека и автоклика (cycle-modes.ps1). Отказ, если клиент запущен.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_012 ConquestLarge0 1;XP5_002 ConquestLarge0 1;MP_018 ConquestLarge0 1" -MaxMin 40
