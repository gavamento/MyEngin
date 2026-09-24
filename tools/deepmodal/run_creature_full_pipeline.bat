@echo off
setlocal
cd /d "%~dp0\..\.."

set EPOCHS=350
set BATCH_SIZE=16
set LR=2e-4
set LR_MIN=2e-5
set LR_HALVE_EVERY=50
set EVAL_EVERY=25
set JOBS=12

if not "%~1"=="" set EPOCHS=%~1

echo ========================================================
echo  Deep-Modal Creature / Enemy Full Auto Pipeline
echo  Stage 1: Export remaining OBJ from Thingi10K
echo  Stage 2: Multiprocess FEM modal analysis -> NPZ
echo  Stage 3: Resume deepmodal_creatures.pt training
echo  Stage 4: Export to .dmnet and sync to demo project
echo ========================================================

echo.
echo [Step 1/4] Extracting creature OBJ models...
python tools\deepmodal\export_creatures_obj.py --list tools\deepmodal\creature_list.txt --limit 2000 --out tools\deepmodal\data\raw\creatures

if %ERRORLEVEL% NEQ 0 (
    echo [Pipeline] ERROR: OBJ export failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo.
echo [Step 2/4] Running FEM modal analysis on new meshes...
python tools\deepmodal\dataset.py --stage small --list tools\deepmodal\creatures_obj_list.txt --out tools\deepmodal\data\creatures --jobs %JOBS%

if %ERRORLEVEL% NEQ 0 (
    echo [Pipeline] ERROR: FEM dataset generation failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo.
echo [Step 3/4] Training model on expanded creature dataset (%EPOCHS% epochs)...
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
    echo [Pipeline] ERROR: Training failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

echo.
echo [Step 4/4] Exporting FP16 binary (.dmnet) and syncing...
python tools\deepmodal\export.py ^
    --checkpoint tools\deepmodal\runs\deepmodal_creatures.pt ^
    --out assets\deepmodal\deepmodal_creatures.dmnet ^
    --amp-scale 11478.0

if %ERRORLEVEL% NEQ 0 (
    echo [Pipeline] ERROR: Export failed with code %ERRORLEVEL%
    exit /b %ERRORLEVEL%
)

if exist "C:\Users\akita\Documents\MyEngineProjects\deepmodaldemo\assets\deepmodal" (
    copy /y "assets\deepmodal\deepmodal_creatures.dmnet" "C:\Users\akita\Documents\MyEngineProjects\deepmodaldemo\assets\deepmodal\deepmodal_creatures.dmnet"
)

echo.
echo ========================================================
echo  [Pipeline] Full training and export completed successfully!
echo ========================================================
