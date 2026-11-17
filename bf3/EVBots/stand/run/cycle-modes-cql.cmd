@echo off
rem Быстрая проверка сборки (19.10): один режим MP_007 ConquestLarge0, без человека и автоклика, около 5 минут.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" -Maps "MP_007 ConquestLarge0 1" -MaxMin 10
