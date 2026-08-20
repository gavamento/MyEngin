@echo off
rem shot_verify.bat — 決定的スクリーンショットの回帰テスト (M52c、engine_spec.md 11.3 系)
rem   tests\golden\*.png と撮り直した tests\actual\*.png をピクセル比較する。
rem   差があれば diff ヒートマップを tests\actual\<name>.diff.png に残して exit 1。
rem
rem   使い方:  tools\shot_verify.bat            … 照合する (CI と同じ)
rem            tools\shot_verify.bat --update   … golden を撮り直す (差分を目視してからコミット)
rem
rem ★撮影は必ず --warp (ソフトウェアラスタライザ) で行う。実 GPU と WARP は同じシーンでも
rem   maxDiff=2 で 7 割の画素が違う (M52c 実測。FXAA/トーンマップの丸め) ので、
rem   golden とランナーのラスタライザを揃えないと回帰テストにならない。
rem   逆に Debug と Release は WARP 同士でビット一致する (実測) ので撮影は Release だけ。
rem
rem ★--font-embedded も必須。フォントアトラスは assets\fonts → システム日本語 TTF の順で
rem   探すので、golden を撮った機械と CI ランナーで別のフォントになりうる
rem   (英語版 Windows Server には日本語 TTF が無い)。内蔵 8x8 に固定して機種差を消す。
rem
rem ★ビルドはしない。replay_verify.bat の後に回す前提 (CI もその順序)。
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set UPDATE=0
if /i "%~1"=="--update" set UPDATE=1

set REL=bin\x64\Release
if not exist %REL%\Runtime.exe (
    echo [shot_verify] %REL%\Runtime.exe not found - build Release first ^(tools\replay_verify.bat^) & exit /b 1
)
if not exist %REL%\Editor.exe (
    echo [shot_verify] %REL%\Editor.exe not found - build Release first ^(tools\replay_verify.bat^) & exit /b 1
)

rem チャンネル差の許容。同一マシンなら 0 で一致するが、WARP は OS 同梱 (d3d10warp.dll) で
rem ランナーと開発機でビルドが違いうる。実 GPU との差ですら maxDiff=2 だったので 2 を既定にし、
rem 実測値は毎回ログに出す (数字を隠さないことがこのテストの価値)
set TOL=2
if defined MYE_SHOT_TOL set TOL=%MYE_SHOT_TOL%

set GOLDEN=tests\golden
set ACTUAL=tests\actual
if not exist %GOLDEN% mkdir %GOLDEN%
if not exist %ACTUAL% mkdir %ACTUAL%

rem 撮影条件はこの 1 行に固定する (golden を撮った条件と照合の条件が食い違わないように)。
rem --screenshot 指定で EngineLoop が決定的撮影モードに入る = frame 番号 == tick 番号
set SHOT=--warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3

rem ---- コードから組み直すシーン (replay_verify と同じ流儀) ----
rem parts はモデル由来のサブアセット ID が絶対パスのハッシュなのでシーンファイルを
rem コミットできない。flow は builtin のみだが正解はコード側なので同じく生成物
echo === build generated scenes (parts / flow) ===
set PARTS_SCENE=cache\parts_showcase.scene.json
if exist %PARTS_SCENE% del /q %PARTS_SCENE%
%REL%\Editor.exe --parts-demo --save-scene-on-start --frames 2 --no-audio --warp || exit /b 1
if not exist %PARTS_SCENE% (echo [shot_verify] parts showcase scene was not written & exit /b 1)

set FLOW_TITLE=assets\scenes\flow_title.scene.json
if exist %FLOW_TITLE% del /q %FLOW_TITLE%
%REL%\Editor.exe --flow-demo --frames 2 --no-audio --warp || exit /b 1
if not exist %FLOW_TITLE% (echo [shot_verify] flow title scene was not written & exit /b 1)

set FAILED=0

rem ---- 5 本。既定デモの 2 経路 (Forward / Deferred) + 生成シーン 2 本 + UI プローブ ----
rem RT デモは WARP では重すぎるので CI 対象外 (ローカル任意)
call :shot demo_forward
call :shot demo_deferred --deferred
call :shot parts --scene %PARTS_SCENE%
call :shot flow_title --scene %FLOW_TITLE%
call :shot ui_probe --scene assets\scenes\ui_probe.scene.json

echo.
if %UPDATE%==1 (
    echo [shot_verify] golden updated in %GOLDEN% - review the images before committing
    exit /b 0
)
if not %FAILED%==0 (
    echo [FAIL] screenshot regression: %FAILED% shot^(s^) differ ^(see %ACTUAL%\*.diff.png^)
    echo        expected images are in %GOLDEN%. If the change is intended:
    echo          tools\shot_verify.bat --update
    exit /b 1
)
echo [PASS] screenshot regression (5 shots, warp, tol=%TOL%)
exit /b 0

rem -------------------------------------------------------------------- :shot
rem %1 = 名前 / %2.. = Runtime へ渡す追加引数 (シーン指定など)
:shot
set NAME=%1
shift
set EXTRA=%1 %2 %3
set OUT=%ACTUAL%\%NAME%.png
if %UPDATE%==1 set OUT=%GOLDEN%\%NAME%.png
if exist "%OUT%" del /q "%OUT%"

echo === shot: %NAME% ===
%REL%\Runtime.exe %SHOT% %EXTRA% --screenshot %OUT%
if errorlevel 1 (
    echo [shot_verify] %NAME%: runtime exited with an error
    set /a FAILED+=1
    goto :eof
)
if not exist "%OUT%" (
    echo [shot_verify] %NAME%: no screenshot was written
    set /a FAILED+=1
    goto :eof
)
if %UPDATE%==1 goto :eof

if not exist "%GOLDEN%\%NAME%.png" (
    echo [shot_verify] %NAME%: no golden image - run "tools\shot_verify.bat --update" once and commit it
    set /a FAILED+=1
    goto :eof
)
%REL%\Editor.exe --img-diff %GOLDEN%\%NAME%.png %OUT% --tol %TOL% --diff-out %ACTUAL%\%NAME%.diff.png
if errorlevel 1 set /a FAILED+=1
goto :eof
