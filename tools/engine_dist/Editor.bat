@echo off
rem Starts the MyEngine editor. Run Setup.bat once first to install the dependencies.
rem This file is ASCII only: cmd.exe reads .bat in the system code page of the receiving PC.
cd /d "%~dp0"
start "" "%~dp0bin\x64\Release\Editor.exe" %*
