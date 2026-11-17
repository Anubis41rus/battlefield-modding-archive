@echo off
rem EVBots stand: as cycle.cmd (autonomous, ONLY when the user is away), but the human stays spawned 20 min after
rem TESTS-DONE and the stand stays up (-KeepUp): for in-game checks by the agent (F12 menu, mouse, fullscreen).
rem Stop afterwards with run\stop.ps1. Log: C:\Games\trash\<date>\run\cycle-HHmmss.log
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 20 -KeepUp %*
