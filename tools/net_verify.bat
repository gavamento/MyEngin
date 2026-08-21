@echo off
rem net_verify.bat — UDP + 遅延ロックステップの実地検証 (M52h)
rem   同一マシンで host / joiner の 2 プロセスを実際に起動し、両方に .rep を録らせて
rem     1. 2 本の .rep が**完全一致**すること (= 本当に同じ tick 列を回した証明)
rem     2. それが「--local-players 2 --synth-input のローカル 1 プロセス実行」の .rep とも
rem        一致すること (= ネット越しの入力が、ローカル 2P と 1 バイトも変わらないこと)
rem   を機械検証する。判定は `fc /b` ではなく `--rep-diff` (M52h) — 割れたときに
rem   「どの tick の どのレーンの どのフィールドか」まで 1 行で出る。
rem
rem   使い方:  tools\net_verify.bat [ticks]     (既定 300)
rem
rem   ケース A: Debug ↔ Debug   / ロス 0%%
rem   ケース B: Debug ↔ Release / ロス 20%%
rem     ★B が本命。**構成を跨いでロックステップが成立すること**まで見ている
rem       (Debug/Release のビット一致は replay_verify が別途担保しているので、ここが
rem        割れたらネット層が sim を汚したという意味になる)。ロスを混ぜているのは
rem       「再送機構を持たず直近 8 tick の冗長送信だけで復元できる」の実地確認。
rem
rem ★CI では回さない。2 プロセス同時起動 + UDP ポート待受 + 実時間タイムアウトという
rem   組み合わせは、赤くなったときに「本物の失敗か runner の都合か」を切り分けづらい。
rem   ロジックの回帰は Editor.exe --selftest の Net session スイート (ループバックで
rem   host/join を 1 プロセス内に立てる) が CI 側で押さえている。
rem ★ビルドはしない。replay_verify.bat で焼いた Debug/Release の exe をそのまま使う。
setlocal enabledelayedexpansion
cd /d "%~dp0.."

rem ---- 自己呼び出しによるバックグラウンド実行 (下の :bg) ----
rem ★host は非同期に立てたいが、GUI サブシステムの exe なので `start` で投げると
rem   終了コードを拾えない。自分自身を __bg モードで呼び直すと、普通の bat の文脈で
rem   !ERRORLEVEL! が読める (cmd /c "... & echo !ERRORLEVEL!" のエスケープ地獄を回避)
if "%~1"=="__bg" goto :bg

set TICKS=300
if not "%~1"=="" set TICKS=%~1

set DBG=bin\x64\Debug
set REL=bin\x64\Release
if not exist %DBG%\Runtime.exe (echo [net_verify] %DBG%\Runtime.exe not found - build Debug first & exit /b 1)
if not exist %REL%\Runtime.exe (echo [net_verify] %REL%\Runtime.exe not found - build Release first & exit /b 1)

rem --local-demo のシーンはコードから毎回組む。保存済みが残っているとロード経路に
rem 落ちてコード側の正解と食い違う (replay_verify と同じ理由)
if exist cache\local_players.scene.json del /q cache\local_players.scene.json
del /q cache\net_*.rep cache\net_*.log cache\net_*.code 2>nul

set COMMON=--local-demo --synth-input --warp --no-audio --replay-ticks %TICKS%
set FAILED=0

echo === reference: local 2-player recording (Debug, %TICKS% ticks) ===
%DBG%\Runtime.exe --local-demo --local-players 2 --synth-input --warp --no-audio --replay-ticks %TICKS% --replay-record cache\net_local.rep > cache\net_local.log 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] the local reference recording failed - see cache\net_local.log
    exit /b 1
)

call :case A 7801 0   "%DBG%\Runtime.exe" "%DBG%\Runtime.exe"
call :case B 7802 20  "%DBG%\Runtime.exe" "%REL%\Runtime.exe"

echo.
if not %FAILED%==0 (
    echo [FAIL] net lockstep: %FAILED% case^(s^) failed
    exit /b 1
)
echo [PASS] net lockstep (2 cases x [host==joiner / == local 2P reference])
exit /b 0

rem -------------------------------------------------------------------- :case
rem %1 = ケース名 / %2 = ポート / %3 = ロス%% / %4 = host の exe / %5 = joiner の exe
:case
set NAME=%~1
set PORT=%~2
set LOSS=%~3
set HOSTEXE=%~4
set JOINEXE=%~5
set HREP=cache\net_%NAME%_host.rep
set JREP=cache\net_%NAME%_join.rep
set HCODE=cache\net_%NAME%_host.code
echo.
echo === case %NAME%: host=%HOSTEXE% join=%JOINEXE% port=%PORT% loss=%LOSS%%% ===
del /q "%HREP%" "%JREP%" "%HCODE%" 2>nul

start "mye net host" /b cmd /c call "%~f0" __bg "%HCODE%" "%HOSTEXE% %COMMON% --net-loss %LOSS% --net-host %PORT% --replay-record %HREP% > cache\net_%NAME%_host.log 2>&1"
%JOINEXE% %COMMON% --net-loss %LOSS% --net-join 127.0.0.1:%PORT% --replay-record %JREP% > cache\net_%NAME%_join.log 2>&1
set JCODE=!ERRORLEVEL!

rem host の終了を待つ (最大 120 秒)。GUI サブシステムなので終了コードは :bg が置く
set WAIT=0
:waithost
if exist "%HCODE%" goto :hostdone
set /a WAIT+=1
if !WAIT! GTR 240 (
    echo   [FAIL] %NAME%: the host did not exit within 120 s
    set /a FAILED+=1
    goto :eof
)
ping -n 2 127.0.0.1 >nul
goto :waithost
:hostdone
set /p HEXIT=<"%HCODE%"

if not "!JCODE!"=="0" (
    echo   [FAIL] %NAME%: the joiner exited with !JCODE! - see cache\net_%NAME%_join.log
    set /a FAILED+=1
    goto :eof
)
if not "!HEXIT!"=="0" (
    echo   [FAIL] %NAME%: the host exited with !HEXIT! - see cache\net_%NAME%_host.log
    set /a FAILED+=1
    goto :eof
)

rem ---- 1. 2 台の .rep が完全一致 ----
%DBG%\Runtime.exe --rep-diff "%HREP%" "%JREP%"
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] %NAME%: the two peers did NOT record the same tick sequence
    set /a FAILED+=1
    goto :eof
)
echo   peers agree: PASS

rem ---- 2. ローカル 2P 参照とも一致 ----
rem     ★ここが「ネット越しの入力がローカル 2P と同一」の機械証明。合成入力は
rem       (tick, lane) の純関数なので、host はレーン 0、joiner はレーン 1 を作るだけで
rem       合成結果がローカル実行と一致するはず — 一致しないなら、レーンの割り当てか
rem       入力遅延の先出し (priming) がずれている
%DBG%\Runtime.exe --rep-diff cache\net_local.rep "%HREP%"
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] %NAME%: the net run differs from the local 2-player reference
    set /a FAILED+=1
    goto :eof
)
echo   matches local 2P reference: PASS
goto :eof

rem ---------------------------------------------------------------------- :bg
rem %2 = 終了コードの置き場 / %3 = 実行するコマンド行 (リダイレクト込み)
rem ★%~3 は展開後に再パースされるので、文字列の中の > もリダイレクトとして効く
:bg
setlocal enabledelayedexpansion
set "BGCODE=%~2"
%~3
echo !ERRORLEVEL!> "!BGCODE!"
endlocal
exit /b 0
