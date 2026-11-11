@echo off
rem cull_verify.bat — カリングの回帰検査 (M99i、engine_spec.md 6.14)
rem   1. 視錐台 ON / OFF (--no-frustum-culling) の全画素一致 (img-diff --tol 0)。カリングが絵を変えていないことの証拠。
rem        既定ベンチ / --culling-demo / --culling-2d-demo / --particle-demo (CPU・GPU) / --2d-demo (昼・夜) / --render-demo (Forward・Deferred)
rem        既定ベンチでは drawCalls が OFF で増え、culled が OFF で 0 になることも見る
rem   2. カリング用カメラの固定: --cull-freeze-frame 10 に --render-bench-cut-frame 20 を重ねても culled がカット無しの値と一致する
rem        (固定が効いていないと、カットした視点の値に動く。カットで値が動くことも確かめて検査が空振りしないようにする)
rem   3. counts の機械比較 (tools\cull_counts.ps1): Release --warp を基準に、Release 実 GPU と Debug 実 GPU の counts が完全一致する
rem        既定ベンチ / --culling-demo / --culling-2d-demo / --culling-distance-demo / 既定ベンチ + --cull-min-screen-size 0.02
rem   4. (任意) M99 前の exe との counts 比較: MYE_CULL_BASE_ROOT に基点のチェックアウト (Release ビルド済み) を渡す。
rem        新しい欄 (culledDistance / culledScreenSize / culledNonMesh / unculledGpuEmitters) を除いて既定ベンチの counts が一致する
rem
rem   使い方:  tools\cull_verify.bat
rem   環境変数:
rem     MYE_CULL_SKIP_CONFIGS=1  … 3 を飛ばす (実 GPU・Debug ビルドが無い CI 用。1 / 2 は WARP だけで回る)
rem     MYE_CULL_BASE_ROOT=<dir> … 4 を回す
rem
rem ★ビルドはしない (shot_verify.bat と同じ。Release を先にビルドしておく。3 は Debug も要る)。
rem ★撮影条件は shot_verify.bat に揃える (--warp --font-embedded --no-fxaa)。cache\ の保存済みシーンは先に消す
rem   (残っていると組み直さずに読まれて、シーン側の変更が検査に乗らない)。
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set REL=bin\x64\Release
set DBG=bin\x64\Debug
if not exist %REL%\Runtime.exe (
    echo [cull_verify] %REL%\Runtime.exe not found - build Release first & exit /b 1
)
if not exist %REL%\Editor.exe (
    echo [cull_verify] %REL%\Editor.exe not found - build Release first & exit /b 1
)
if not defined MYE_CULL_SKIP_CONFIGS (
    if not exist %DBG%\Runtime.exe (
        echo [cull_verify] %DBG%\Runtime.exe not found - build Debug, or set MYE_CULL_SKIP_CONFIGS=1 & exit /b 1
    )
)

set OUT=tests\actual\cull
if not exist %OUT% mkdir %OUT%
set LOG=%OUT%\cull_verify.log
if exist %LOG% del /q %LOG%
set COUNTS=pwsh -NoProfile -ExecutionPolicy Bypass -File tools\cull_counts.ps1

rem 保存済みシーンが残っていると組み直さずに読まれる (shot_verify.bat と同じ理由)
for %%S in (render_bench culling_showcase culling_2d_showcase culling_distance_showcase particle_showcase 2d_showcase render_showcase) do (
    if exist cache\%%S.scene.json del /q cache\%%S.scene.json
)

rem 撮影条件。Runtime の既定は実 GPU なので --warp はここで足す (3 だけ実 GPU でも回すため %WARP% で出し入れする)
set COMMON=--no-audio --font-embedded --width 960 --height 540 --no-fxaa
set WARP=--warp
set RT=%REL%\Runtime.exe

set FAILED=0
set CHECKS=0

echo === 1. frustum culling on/off: pixel match (tol 0) ===
call :pair bench 3 --render-bench-demo
call :pair culling 3 --culling-demo
call :pair culling2d 3 --culling-2d-demo
call :pair particle_cpu 120 --particle-demo --particle-backend cpu
call :pair particle_gpu 120 --particle-demo --particle-backend gpu
call :pair 2d 3 --2d-demo
call :pair 2d_night 120 --2d-demo --2d-night
call :pair render_forward 3 --render-demo
call :pair render_deferred 3 --render-demo --deferred

rem 既定ベンチ: OFF は何も落とさず (culled = 0)、その分 drawCalls が増える
call :dump %OUT%\bench_on.json 3 --render-bench-demo
call :dump %OUT%\bench_off.json 3 --render-bench-demo --no-frustum-culling
call :check_zero %OUT%\bench_off.json 1 culled
call :check_diff %OUT%\bench_on.json %OUT%\bench_off.json 1 drawCalls

echo === 2. frozen culling camera ===
rem 同じ固定 (10 フレーム目) で、カット (20 フレーム目) の有り / 無しの culled が一致する。固定無しのカットは値が動く
set FREEZE_FIELDS=culled,culledDistance,culledScreenSize,culledNonMesh
call :dump %OUT%\freeze_nocut.json 30 --render-bench-demo --cull-freeze-frame 10
call :dump %OUT%\freeze_cut.json 30 --render-bench-demo --cull-freeze-frame 10 --render-bench-cut-frame 20
call :dump %OUT%\nofreeze_cut.json 30 --render-bench-demo --render-bench-cut-frame 20
call :check_only %OUT%\freeze_nocut.json %OUT%\freeze_cut.json 1 "%FREEZE_FIELDS%"
call :check_only_diff %OUT%\freeze_nocut.json %OUT%\nofreeze_cut.json 1 culled

if defined MYE_CULL_SKIP_CONFIGS goto :skip_configs
echo === 3. counts: Release --warp vs Release (GPU) vs Debug (GPU) ===
call :config_counts bench 3 --render-bench-demo
call :config_counts culling 3 --culling-demo
call :config_counts culling2d 3 --culling-2d-demo
call :config_counts distance 3 --culling-distance-demo
call :config_counts bench_size 3 --render-bench-demo --cull-min-screen-size 0.02
:skip_configs

if not defined MYE_CULL_BASE_ROOT goto :skip_base
echo === 4. counts vs the pre-M99 build (%MYE_CULL_BASE_ROOT%) ===
if not exist "%MYE_CULL_BASE_ROOT%\%REL%\Runtime.exe" (
    echo [cull_verify] base Runtime.exe not found under %MYE_CULL_BASE_ROOT% & set /a FAILED+=1 & goto :skip_base
)
call :dump %OUT%\bench_warp.json 3 --render-bench-demo
pushd "%MYE_CULL_BASE_ROOT%"
if exist cache\render_bench.scene.json del /q cache\render_bench.scene.json
%REL%\Runtime.exe %WARP% %COMMON% --frames 6 --shot-frame 3 --render-bench-demo --render-stats-dump "%~dp0..\%OUT%\bench_base.json" >> "%~dp0..\%LOG%" 2>&1
popd
set /a CHECKS+=1
%COUNTS% -A %OUT%\bench_base.json -B %OUT%\bench_warp.json -Ignore "culledDistance,culledScreenSize,culledNonMesh,unculledGpuEmitters"
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
:skip_base

echo.
if not %FAILED%==0 (
    echo [FAIL] culling verify: %FAILED% of %CHECKS% check^(s^) failed ^(runtime log: %LOG%, images / json: %OUT%^)
    exit /b 1
)
echo [PASS] culling verify ^(%CHECKS% checks, frustum on/off pixel match at tol 0, frozen camera, counts^)
exit /b 0

rem -------------------------------------------------------------------- :collect
rem 残りの引数 (%1 以降) を EXTRA へ集める。%* は shift しても変わらないので 1 つずつ足す
:collect
set EXTRA=
:collect_loop
if "%~1"=="" goto :eof
set EXTRA=%EXTRA% %1
shift
goto :collect_loop

rem -------------------------------------------------------------------- :pair
rem %1 = 名前 / %2 = 撮影フレーム / %3.. = Runtime へ渡す追加引数。視錐台 ON と OFF を撮って全画素を比べる
:pair
set NAME=%1
set SHOTFRAME=%2
shift
shift
call :collect %1 %2 %3 %4 %5 %6
set /a RUNFRAMES=%SHOTFRAME%+3
set /a CHECKS+=1
echo === pair: %NAME% ===
for %%M in (on off) do (
    set FLAG=
    if %%M==off set FLAG=--no-frustum-culling
    set IMG=%OUT%\%NAME%_%%M.png
    if exist "!IMG!" del /q "!IMG!"
    %RT% %WARP% %COMMON% --frames !RUNFRAMES! --shot-frame %SHOTFRAME% !EXTRA! !FLAG! --screenshot "!IMG!" >> %LOG% 2>&1
    rem ★"if errorlevel 1" は使わない (SEH の負の終了コードを見落とす。shot_verify.bat と同じ)
    if !ERRORLEVEL! NEQ 0 (
        echo [cull_verify] %NAME% ^(%%M^): runtime exited with an error
        set /a FAILED+=1
        goto :eof
    )
    if not exist "!IMG!" (
        echo [cull_verify] %NAME% ^(%%M^): no screenshot was written
        set /a FAILED+=1
        goto :eof
    )
)
%REL%\Editor.exe --img-diff %OUT%\%NAME%_on.png %OUT%\%NAME%_off.png --tol 0 --diff-out %OUT%\%NAME%.diff.png
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
goto :eof

rem -------------------------------------------------------------------- :dump
rem %1 = 出力 json / %2 = 撮影フレーム / %3.. = 追加引数。%RT% と %WARP% の構成で統計を取る
:dump
set DUMP=%1
set SHOTFRAME=%2
shift
shift
call :collect %1 %2 %3 %4 %5 %6
set /a RUNFRAMES=%SHOTFRAME%+3
if exist "%DUMP%" del /q "%DUMP%"
%RT% %WARP% %COMMON% --frames %RUNFRAMES% --shot-frame %SHOTFRAME% %EXTRA% --render-stats-dump "%DUMP%" >> %LOG% 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo [cull_verify] %DUMP%: runtime exited with an error
    set /a FAILED+=1
    goto :eof
)
if not exist "%DUMP%" (
    echo [cull_verify] %DUMP%: no stats were written
    set /a FAILED+=1
)
goto :eof

rem -------------------------------------------------------------------- :check_zero
rem %1 = json / %2 = viewKey / %3 = 欄名。その欄が 0 であること
:check_zero
set /a CHECKS+=1
if not exist "%~1" (set /a FAILED+=1 & goto :eof)
%COUNTS% -A %1 -B %1 -View %2 -Only %3 -Zero
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
goto :eof

rem -------------------------------------------------------------------- :check_diff
rem %1 / %2 = json / %3 = viewKey / %4 = 欄名。その欄が違うこと (OFF で drawCalls が増える検査)
:check_diff
set /a CHECKS+=1
if not exist "%~1" (set /a FAILED+=1 & goto :eof)
if not exist "%~2" (set /a FAILED+=1 & goto :eof)
%COUNTS% -A %1 -B %2 -View %3 -Only %4 -Greater
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
goto :eof

rem -------------------------------------------------------------------- :check_only
rem %1 / %2 = json / %3 = viewKey / %4 = 欄名 (空白区切りを引用符で 1 つに)。その欄が完全一致すること
:check_only
set /a CHECKS+=1
if not exist "%~1" (set /a FAILED+=1 & goto :eof)
if not exist "%~2" (set /a FAILED+=1 & goto :eof)
%COUNTS% -A %1 -B %2 -View %3 -Only "%~4"
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
goto :eof

rem -------------------------------------------------------------------- :check_only_diff
rem %1 / %2 = json / %3 = viewKey / %4 = 欄名。その欄が違うこと (固定の検査が空振りしていない証拠)
:check_only_diff
set /a CHECKS+=1
if not exist "%~1" (set /a FAILED+=1 & goto :eof)
if not exist "%~2" (set /a FAILED+=1 & goto :eof)
%COUNTS% -A %1 -B %2 -View %3 -Only %4 -ExpectDiff
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
goto :eof

rem -------------------------------------------------------------------- :config_counts
rem %1 = 名前 / %2 = 撮影フレーム / %3.. = 追加引数。Release --warp を基準に実 GPU の Release と Debug を比べる
:config_counts
set NAME=%1
set SHOTFRAME=%2
shift
shift
call :collect %1 %2 %3 %4 %5 %6
echo === counts: %NAME% ===
set REF=%OUT%\cfg_%NAME%_rel_warp.json
set RT=%REL%\Runtime.exe
set WARP=--warp
call :dump %REF% %SHOTFRAME% %EXTRA%
set RT=%REL%\Runtime.exe
set WARP=
call :dump %OUT%\cfg_%NAME%_rel_gpu.json %SHOTFRAME% %EXTRA%
set RT=%DBG%\Runtime.exe
call :dump %OUT%\cfg_%NAME%_dbg_gpu.json %SHOTFRAME% %EXTRA%
set RT=%REL%\Runtime.exe
set WARP=--warp
for %%C in (rel_gpu dbg_gpu) do (
    set /a CHECKS+=1
    if exist "%OUT%\cfg_%NAME%_%%C.json" (
        %COUNTS% -A %REF% -B %OUT%\cfg_%NAME%_%%C.json
        if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
    ) else (
        set /a FAILED+=1
    )
)
goto :eof
