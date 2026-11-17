@echo off
rem 26.10 (238-4): which client frequency patch group triggers the license error. Full restart first (client, server,
rem backend), then the bisect joins the client itself and closes bf3. Result: run\freq-bisect.txt
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-Process bf3 -ErrorAction SilentlyContinue | Stop-Process -Force"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stop.ps1" -Force
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start.ps1" -Force -NoJoin
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0freq-bisect.ps1" %*
