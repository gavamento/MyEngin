@echo off
rem DeepModalModel: pilot run (1,000 shapes) for family tuning and statistics.
rem Usage: run_pilot.bat [SEED]   (default 1)
rem Comments are ASCII on purpose: cmd reads .bat as cp932 and breaks on UTF-8 Japanese.
setlocal
cd /d "%~dp0"
set SEED=1
if not "%~1"=="" set SEED=%~1
python generate.py run --out data\pilot --target 1000 --seed %SEED% --jobs 10 --editor-jobs 4 --chunk 1000 --batch 250 --force
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
python generate.py stats --out data\pilot
