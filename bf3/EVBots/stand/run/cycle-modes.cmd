@echo off
rem Автономная проверка всех режимов за один старт (19.10, сборка 201): без человека и автоклика, уровень после набора тестов меняет RCON.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle-modes.ps1" %*
