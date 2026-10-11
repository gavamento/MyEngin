@echo off
rem DeepModalModel: FEM stage. Feeds list_part00..09.txt of a run into tools\deepmodal\dataset.py
rem (--stage small) one part after another, so stats.json appears every 5,000 shapes.
rem Existing npz files are skipped by dataset.py, so the same command resumes after a stop.
rem Expect 35-40 hours for 50,000 shapes with 11 jobs (see fem_hours_est_jobs11 in stats.json).
rem Usage: run_dataset.bat [RUN_DIR] [OUT_DIR] [JOBS]
rem   defaults: DeepModalModel\data\v1  tools\deepmodal\data\synth_v1  11
rem Comments are ASCII on purpose: cmd reads .bat as cp932 and breaks on UTF-8 Japanese.
setlocal
cd /d "%~dp0\..\"
set RUN_DIR=DeepModalModel\data\v1
set OUT_DIR=tools\deepmodal\data\synth_v1
set JOBS=11
if not "%~1"=="" set RUN_DIR=%~1
if not "%~2"=="" set OUT_DIR=%~2
if not "%~3"=="" set JOBS=%~3
if not exist "%RUN_DIR%\list_part00.txt" (
    echo [run_dataset] %RUN_DIR%\list_part00.txt not found. Run "generate.py finalize" first.
    exit /b 1
)
for /L %%k in (0,1,9) do (
    call :run_part "%RUN_DIR%\list_part0%%k.txt"
    if errorlevel 1 exit /b 1
)
echo [run_dataset] all parts done -^> %OUT_DIR%
exit /b 0

:run_part
echo [run_dataset] %date% %time% part %~1
python tools\deepmodal\dataset.py --stage small --list "%~1" --out "%OUT_DIR%" --jobs %JOBS%
exit /b %ERRORLEVEL%
