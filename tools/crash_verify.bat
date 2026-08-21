@echo off
rem crash_verify.bat — クラッシュバンドルの実地検証 (M52f)
rem   5 経路 (av / purecall / terminate / invalidparam / stackoverflow) それぞれで
rem   **本当に落として**、
rem     1. crash\<timestamp>\ に crash.txt + crash.rep + minidump.dmp が揃うこと
rem     2. その crash.rep を素で --replay-verify すると落ちる直前 tick まで
rem        ハッシュ一致で再生できること (= 状態が本当に復元されている証拠)
rem     3. 同じ --crash-test 引数を足して再生すると**同じ tick でまた落ちる**こと
rem        (= 合成クラッシュの引き金はフラグ側にあるので、再現側にも同じ引数を渡す。
rem          実バグの引き金は sim 状態なので、その場合は 2. の素の再生だけで再現する)
rem   を確認する。
rem
rem   使い方:  tools\crash_verify.bat [Debug|Release]   (既定 Debug)
rem
rem ★CI では回さない。プロセスを故意に落とすので、ランナーの WER やジョブ制御と相性が悪く、
rem   「赤くなったのが本物の失敗か仕込みか」を区別しづらい。ローカル手順として残すのが目的。
rem ★この bat は %BIN%\crash\ を**毎回消してから**回す (バンドルの個数で判定するため)。
rem   本物のクラッシュ報告を置いている場合は先に退避すること。
rem ★ビルドはしない。replay_verify.bat で焼いた exe をそのまま使う。
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set CFG=Debug
if /i "%~1"=="Release" set CFG=Release
set BIN=bin\x64\%CFG%
set CRASH=%BIN%\crash
if not exist %BIN%\Runtime.exe (
    echo [crash_verify] %BIN%\Runtime.exe not found - build %CFG% first & exit /b 1
)

set COMMON=--frames 400 --warp --no-audio
set FAILED=0

call :case av 30
call :case purecall 45
call :case terminate 60
call :case invalidparam 75
rem スタックオーバーフローが最悪ケース: ハンドラに残るスタックは 1 ページ程度しか無い。
rem 事前確保の作業領域と「minidump は別スレッド (新品のスタック) で書く」がここで効く
call :case stackoverflow 90

echo.
if not %FAILED%==0 (
    echo [FAIL] crash bundle: %FAILED% case^(s^) failed
    exit /b 1
)
echo [PASS] crash bundle (5 kinds x [bundle written / rep replays / rep reproduces])
exit /b 0

rem -------------------------------------------------------------------- :case
rem %1 = --crash-test の種別 / %2 = 落とす tick
:case
set KIND=%1
set AT=%2
echo.
echo === crash-test %KIND% (tick %AT%, %CFG%) ===
rd /s /q %CRASH% 2>nul

rem ---- 1. 落として、バンドルが出ること ----
%BIN%\Runtime.exe --crash-test %KIND% --crash-at-tick %AT% %COMMON%
rem ★errorlevel の比較は必ず数値で行う。SEH で落ちた exit code は 0xC0000005 =
rem   符号付きだと負なので、"if errorlevel 1" (= 1 以上か) は**偽になる**
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] %KIND%: exited cleanly - it did not crash
    set /a FAILED+=1
    goto :eof
)
echo   exit code = !ERRORLEVEL!
set DIR=
for /d %%D in (%CRASH%\*) do set DIR=%%D
if "!DIR!"=="" (
    echo   [FAIL] %KIND%: no crash bundle was written
    set /a FAILED+=1
    goto :eof
)
for %%F in (crash.txt crash.rep minidump.dmp) do (
    if not exist "!DIR!\%%F" (
        echo   [FAIL] %KIND%: !DIR!\%%F is missing
        set /a FAILED+=1
        goto :eof
    )
)
echo   bundle = !DIR!

rem ---- 2. 素の再生: 落ちる直前 tick までハッシュ一致 ----
rem     最後の 1 tick は未完了 (期待ハッシュ無し) なので照合対象外。
rem     ここが PASS することが「同じ世界を復元できた」の機械的な証拠
%BIN%\Runtime.exe --replay-verify "!DIR!\crash.rep" %COMMON%
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] %KIND%: crash.rep did not replay hash-identical
    set /a FAILED+=1
    goto :eof
)
echo   replay: PASS

rem ---- 3. 同じ引き金を渡すと同じ tick でまた落ちる ----
%BIN%\Runtime.exe --replay-verify "!DIR!\crash.rep" --crash-test %KIND% --crash-at-tick %AT% %COMMON%
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] %KIND%: the crash did not reproduce from crash.rep
    set /a FAILED+=1
    goto :eof
)
set COUNT=0
for /d %%D in (%CRASH%\*) do set /a COUNT+=1
if !COUNT! LSS 2 (
    echo   [FAIL] %KIND%: the reproduction run did not write its own bundle
    set /a FAILED+=1
    goto :eof
)
echo   reproduce: PASS (bundles = !COUNT!)
goto :eof
