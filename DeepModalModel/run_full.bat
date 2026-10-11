@echo off
rem DeepModalModel: full run (50,000 shapes) into data\v1.
rem An existing data\v1 is resumed automatically (Ctrl+C to stop, same command to resume,
rem --force to start over). Takes about 20-40 minutes on 12 cores.
rem Usage: run_full.bat [TARGET]   (default 50000)
rem Comments are ASCII on purpose: cmd reads .bat as cp932 and breaks on UTF-8 Japanese.
setlocal
cd /d "%~dp0"
set TARGET=50000
if not "%~1"=="" set TARGET=%~1
python generate.py run --out data\v1 --target %TARGET% --seed 20261011 --jobs 10 --editor-jobs 4
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
python generate.py stats --out data\v1
