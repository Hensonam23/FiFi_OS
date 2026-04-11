@echo off
:: FiFi OS Installer — Windows launcher
:: Right-click this file and select "Run as administrator"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo.
    echo  ERROR: This installer must be run as Administrator.
    echo.
    echo  Right-click install.bat and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
