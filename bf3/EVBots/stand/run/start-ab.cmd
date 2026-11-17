@echo off
rem Stand with the A/B experiment: group A = VU create sequence + AI flag 0 (VU-like player), group B = as before.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -ExtraEnv "EVBOTS_VU_CREATE=ab,EVBOTS_BOT_AI=ab"
if errorlevel 1 pause
