@echo off
rem Convenience launcher for Windows.
rem Put this file next to kuni.exe (portable build) and double-click it.

chcp 65001 >nul
title Kuni

if not exist "config.toml" (
    echo config.toml not found in %CD%.
    echo Copy your config.toml next to kuni.exe and start again.
    pause
    exit /b 1
)

if exist "kuni.exe" (
    kuni.exe %*
) else if exist "bin\kuni.exe" (
    bin\kuni.exe %*
) else (
    echo kuni.exe not found. Run this script from the folder containing kuni.exe.
    pause
    exit /b 1
)

echo.
echo Kuni has exited. Press any key to close this window.
pause >nul
