@echo off
title E4 builder
rem Put your E4 Xbox Live Arcade zip next to this file, then double-click it.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0kit\build_windows.ps1"
echo.
pause
