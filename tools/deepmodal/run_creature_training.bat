@echo off
setlocal
cd /d "%~dp0\..\.."

set EPOCHS=200
set BATCH_SIZE=16
set LR=2e-4
set LR_MIN=2e-5
set LR_HALVE_EVERY=40
set EVAL_EVERY=20

if not "%~1"=="" set EPOCHS=%~1

echo ========================================================
echo  Deep-Modal Creature / Enemy Training Runner
echo  Data: tools\deepmodal\data\creatures (574 samples)
echo  Resume: tools\deepmodal\runs\deepmodal_creatures.pt
echo  Epochs: %EPOCHS%, BatchSize: %BATCH_SIZE%, LR: %LR%
echo ========================================================

python tools\deepmodal\train.py ^
    --data tools\deepmodal\data\creatures ^
    --resume tools\deepmodal\runs\deepmodal_creatures.pt ^
    --out tools\deepmodal\runs\deepmodal_creatures.pt ^
    --epochs %EPOCHS% ^
    --lr %LR% ^
    --lr-min %LR_MIN% ^
    --lr-halve-every %LR_HALVE_EVERY% ^
    --batch-size %BATCH_SIZE% ^
    --eval-every %EVAL_EVERY%

if %ERRORLEVEL% NEQ 0 (
    echo [CreatureTrain] ERROR: Training failed with exit code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo.
echo ========================================================
echo  Exporting FP16 binary (.dmnet)
echo ========================================================
python tools\deepmodal\export.py ^
    --checkpoint tools\deepmodal\runs\deepmodal_creatures.pt ^
    --out assets\deepmodal\deepmodal_creatures.dmnet ^
    --amp-scale 11478.0

if %ERRORLEVEL% NEQ 0 (
    echo [CreatureTrain] ERROR: Export failed with exit code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

if exist "C:\Users\akita\Documents\MyEngineProjects\deepmodaldemo\assets\deepmodal" (
    echo.
    echo ========================================================
    echo  Syncing to demo project
    echo ========================================================
    copy /y "assets\deepmodal\deepmodal_creatures.dmnet" "C:\Users\akita\Documents\MyEngineProjects\deepmodaldemo\assets\deepmodal\deepmodal_creatures.dmnet"
)

echo.
echo [CreatureTrain] All steps completed successfully!
