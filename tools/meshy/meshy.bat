@echo off
REM Pulse Meshy generation launcher (double-clickable, no execution-policy friction).
REM Forwards all args to meshy_gen.ps1. See README.md for usage.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0meshy_gen.ps1" %*
