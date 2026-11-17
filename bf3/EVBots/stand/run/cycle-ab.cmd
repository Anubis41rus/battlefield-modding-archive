@echo off
rem Autonomous A/B run: group A = VU create sequence (ServerPlayerCreateMessage + LevelLoaded) + isAI 0 (scores), group B as before.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cycle.ps1" -UserAway -PlayMin 15 -ExtraEnv "EVBOTS_VU_CREATE=ab,EVBOTS_BOT_AI=ab"
