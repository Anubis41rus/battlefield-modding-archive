@echo off
rem Join Royale client (pass extra args to join.ps1, e.g. -ClientGameDir "D:\Games-cache\royale\client-runtime").
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0join.ps1" -ModProfile Royale %*
