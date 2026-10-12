@echo off
rem MyEngine setup: installs missing dependencies with winget, then starts the editor.
rem The logic is in Setup.ps1 (Windows PowerShell 5.1 ships with Windows; pwsh may not be installed).
rem This file is ASCII only: cmd.exe reads .bat in the system code page of the receiving PC.
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup.ps1" %*
set RC=%ERRORLEVEL%
echo.
pause
exit /b %RC%
