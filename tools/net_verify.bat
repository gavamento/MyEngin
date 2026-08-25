@echo off
rem net_verify.bat — UDP + 遅延ロックステップ + 予測ロールバックの実地検証 (M52h / M52i)
rem   同一マシンで host / joiner の 2 プロセスを実際に起動し、両方に .rep を録らせて
rem     1. 2 本の .rep が**完全一致**すること (= 本当に同じ tick 列を回した証明)
rem     2. それが「--local-players 2 --synth-input のローカル 1 プロセス実行」の .rep とも
rem        一致すること (= ネット越しの入力が、ローカル 2P と 1 バイトも変わらないこと)
rem   を機械検証する。判定は `fc /b` ではなく `--rep-diff` (M52h) — 割れたときに
rem   「どの tick の どのレーンの どのフィールドか」まで 1 行で出る。
rem
rem   使い方:  tools\net_verify.bat [ticks]     (既定 300)
rem
rem   ケース A: Debug ↔ Debug   / ロス 0%%  / 遅延 3 / ロールバック on
rem   ケース B: Debug ↔ Release / ロス 20%% / 遅延 3 / ロールバック on
rem   ケース C: Debug ↔ Release / ロス 30%% / 遅延 1 / ロールバック on  ← 巻き戻し多発帯
rem   ケース D: Debug ↔ Debug   / ロス 20%% / 遅延 3 / ロールバック off (M52h の回帰)
rem   ケース E: desync 注入 (--net-poke-tick) → 検出 + バンドル + 診断チェーン
rem
rem   ★A-D の本質は同じ 1 つの主張:「**いつ tick が回るか**がどれだけ揺れても、
rem     **tick が何を消費するか**は確定入力だけで決まる」。ロールバックは前者を
rem     大きく揺らす仕掛けなので、それでも .rep がローカル 2P 参照とバイト一致するなら、
rem     予測・巻き戻し・再シムが sim を 1 バイトも汚していないことの証明になる。
rem     C は遅延 1 + ロス 30%% で**予測が外れ続ける**帯を狙って踏ませている。
rem   ★D を残しているのは「ロールバックが原因か」を切り分ける口を検証側にも持つため。
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
del /q cache\net_*.rep cache\net_*.log cache\net_*.code cache\net_*.dump 2>nul

set COMMON=--local-demo --synth-input --warp --no-audio --replay-ticks %TICKS%
set FAILED=0

echo === reference: local 2-player recording (Debug, %TICKS% ticks) ===
%DBG%\Runtime.exe --local-demo --local-players 2 --synth-input --warp --no-audio --replay-ticks %TICKS% --replay-record cache\net_local.rep > cache\net_local.log 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] the local reference recording failed - see cache\net_local.log
    exit /b 1
)
rem ★findstr は「見つかったら 0」なので EQU 0 = FAIL 側 (以降の同型検査も同じ)
findstr /c:"NO C++ scripts are registered" cache\net_local.log >nul
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] the local reference ran without C++ scripts - see cache\net_local.log
    exit /b 1
)

call :case A 7801 0  "--net-delay 3" "%DBG%\Runtime.exe" "%DBG%\Runtime.exe"
call :case B 7802 20 "--net-delay 3" "%DBG%\Runtime.exe" "%REL%\Runtime.exe"
call :case C 7803 30 "--net-delay 1" "%DBG%\Runtime.exe" "%REL%\Runtime.exe"
call :case D 7804 20 "--net-delay 3 --net-no-rollback" "%DBG%\Runtime.exe" "%DBG%\Runtime.exe"
call :desync 7805

echo.
if not %FAILED%==0 (
    echo [FAIL] net lockstep: %FAILED% case^(s^) failed
    exit /b 1
)
echo [PASS] net lockstep (4 cases x [host==joiner / == local 2P reference] + desync detection)
exit /b 0

rem -------------------------------------------------------------------- :case
rem %1 = ケース名 / %2 = ポート / %3 = ロス%% / %4 = 追加引数 / %5 = host exe / %6 = joiner exe
:case
set NAME=%~1
set PORT=%~2
set LOSS=%~3
set XARGS=%~4
set HOSTEXE=%~5
set JOINEXE=%~6
set HREP=cache\net_%NAME%_host.rep
set JREP=cache\net_%NAME%_join.rep
set HCODE=cache\net_%NAME%_host.code
echo.
echo === case %NAME%: host=%HOSTEXE% join=%JOINEXE% port=%PORT% loss=%LOSS%%% %XARGS% ===
del /q "%HREP%" "%JREP%" "%HCODE%" 2>nul

start "mye net host" /b cmd /c call "%~f0" __bg "%HCODE%" "%HOSTEXE% %COMMON% %XARGS% --net-loss %LOSS% --net-host %PORT% --replay-record %HREP% > cache\net_%NAME%_host.log 2>&1"
%JOINEXE% %COMMON% %XARGS% --net-loss %LOSS% --net-join 127.0.0.1:%PORT% --replay-record %JREP% > cache\net_%NAME%_join.log 2>&1
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

rem ---- 0. どちらかが「C++ スクリプト 0 本の世界」で走っていないか ----
rem     ★DllReloader のプローブ衝突 (M52h 追補で修理) の再発検出。exit code 判定より
rem       前に置くのは、接続拒否で joiner が非 0 のとき根本原因を名指しするため。
rem       両者が**同時に** 0 本になるとハッシュが偶然一致して下の 3 段判定は緑のまま
rem       通ってしまう — この findstr が唯一の検出線
findstr /c:"NO C++ scripts are registered" cache\net_%NAME%_host.log cache\net_%NAME%_join.log >nul
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] %NAME%: a peer started without C++ scripts ^(DLL shadow-copy race^) - see the logs
    set /a FAILED+=1
    goto :eof
)

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
rem       入力遅延の先出し (priming) がずれている。**ロールバックが走っていても
rem       ここが一致する**ことが M52i の中心的な主張
%DBG%\Runtime.exe --rep-diff cache\net_local.rep "%HREP%"
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] %NAME%: the net run differs from the local 2-player reference
    set /a FAILED+=1
    goto :eof
)
echo   matches local 2P reference: PASS

rem ---- 3. ロールバックが本当に働いたか (ケース A-C) ----
rem     ★「一致した」だけでは**ロールバックが 1 度も起きていない**可能性が残る。
rem       それでは M52i を何も検証していないので、実際に巻き戻した記録を要求する
if "%XARGS%"=="%XARGS:no-rollback=%" (
    findstr /c:"[net] rollback:" cache\net_%NAME%_host.log cache\net_%NAME%_join.log
    findstr /c:"predicted tick(s)" cache\net_%NAME%_host.log cache\net_%NAME%_join.log >nul
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAIL] %NAME%: neither peer reported rollback statistics
        set /a FAILED+=1
    )
)
goto :eof

rem ------------------------------------------------------------------ :desync
rem %1 = ポート。片側にだけ --net-poke-tick を渡して**意図的に**ワールドを壊し、
rem   ・両者が desync を検出して exit 4 で止まること
rem   ・両者が同じ checkpoint のバンドルを吐くこと (フォルダ名はレーン番号で分かれる)
rem   ・--rep-diff が実際に壊した tick を名指しすること
rem   ・その tick のダンプ 2 本を --hash-diff にかけると**壊したフィールド 1 本**が出ること
rem   を通しで確認する (M52a の診断チェーンと M52i の検出器の統合実証)。
rem ★ロスは 0%% にする。ハッシュ主張のパケットが落ちると検出 checkpoint が後ろへ
rem   ずれてフォルダ名が読めなくなる — ここで見たいのはロス耐性ではない
:desync
set PORT=%~1
set POKE=60
set CHECK=64
set CRASH=%DBG%\crash
set HCODE=cache\net_E_host.code
echo.
echo === case E: desync injection (--net-poke-tick %POKE%, expect detection at tick %CHECK%) ===
if exist "%CRASH%\desync_%CHECK%_p0" rd /s /q "%CRASH%\desync_%CHECK%_p0"
if exist "%CRASH%\desync_%CHECK%_p1" rd /s /q "%CRASH%\desync_%CHECK%_p1"
del /q "%HCODE%" 2>nul

start "mye net host" /b cmd /c call "%~f0" __bg "%HCODE%" "%DBG%\Runtime.exe %COMMON% --net-delay 3 --net-host %PORT% > cache\net_E_host.log 2>&1"
%DBG%\Runtime.exe %COMMON% --net-delay 3 --net-join 127.0.0.1:%PORT% --net-poke-tick %POKE% > cache\net_E_join.log 2>&1
set JCODE=!ERRORLEVEL!
set WAIT=0
:waitdesync
if exist "%HCODE%" goto :desyncdone
set /a WAIT+=1
if !WAIT! GTR 240 (
    echo   [FAIL] E: the host did not exit within 120 s
    set /a FAILED+=1
    goto :eof
)
ping -n 2 127.0.0.1 >nul
goto :waitdesync
:desyncdone
set /p HEXIT=<"%HCODE%"

rem スクリプト 0 本検査 (:case の 0. と同型)。ここで 0 本だと「desync を正しく検出した」
rem ように見えて実は別の世界を壊しただけ、になり得る
findstr /c:"NO C++ scripts are registered" cache\net_E_host.log cache\net_E_join.log >nul
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] E: a peer started without C++ scripts ^(DLL shadow-copy race^) - see the logs
    set /a FAILED+=1
    goto :eof
)

rem exit 4 = desync (1 = 通常の失敗 / 2 = 落とし損ね と区別してある)
if not "!JCODE!"=="4" (
    echo   [FAIL] E: the joiner exited with !JCODE!, expected 4 ^(desync^)
    set /a FAILED+=1
    goto :eof
)
if not "!HEXIT!"=="4" (
    echo   [FAIL] E: the host exited with !HEXIT!, expected 4 ^(desync^)
    set /a FAILED+=1
    goto :eof
)
echo   both peers halted on the desync: PASS

if not exist "%CRASH%\desync_%CHECK%_p0\local.rep" (
    echo   [FAIL] E: no bundle at %CRASH%\desync_%CHECK%_p0
    set /a FAILED+=1
    goto :eof
)
if not exist "%CRASH%\desync_%CHECK%_p1\local.rep" (
    echo   [FAIL] E: no bundle at %CRASH%\desync_%CHECK%_p1
    set /a FAILED+=1
    goto :eof
)
echo   both bundles written (same checkpoint, one folder per lane): PASS

rem ---- 診断チェーン 1: --rep-diff が壊した tick を名指しする ----
%DBG%\Runtime.exe --rep-diff "%CRASH%\desync_%CHECK%_p0\local.rep" "%CRASH%\desync_%CHECK%_p1\local.rep" > cache\net_E_repdiff.log 2>&1
findstr /c:"tick %POKE%: world hash differs" cache\net_E_repdiff.log >nul
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] E: --rep-diff did not point at tick %POKE% - see cache\net_E_repdiff.log
    set /a FAILED+=1
    goto :eof
)
echo   --rep-diff names tick %POKE%: PASS

rem ---- 診断チェーン 2: --hash-diff が壊したフィールドを名指しする ----
rem     ★壊した側の .rep は --net-poke-tick を渡さないと再現しない (変異は tick 本体の
rem       中で起きるので、記録ハッシュにも載っている)。ここは「同じ入力を与えれば
rem       同じ世界になる」という決定論そのものを 2 回使っている
%DBG%\Runtime.exe --warp --no-audio --replay-verify "%CRASH%\desync_%CHECK%_p0\local.rep" --hash-dump-tick %POKE% --hash-dump cache\net_E_p0.dump > cache\net_E_vp0.log 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] E: replaying the clean peer .rep failed - see cache\net_E_vp0.log
    set /a FAILED+=1
    goto :eof
)
%DBG%\Runtime.exe --warp --no-audio --replay-verify "%CRASH%\desync_%CHECK%_p1\local.rep" --net-poke-tick %POKE% --hash-dump-tick %POKE% --hash-dump cache\net_E_p1.dump > cache\net_E_vp1.log 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] E: replaying the poked peer .rep failed - see cache\net_E_vp1.log
    set /a FAILED+=1
    goto :eof
)
%DBG%\Runtime.exe --hash-diff cache\net_E_p0.dump cache\net_E_p1.dump > cache\net_E_hashdiff.log 2>&1
findstr /c:"LocalTransform.position" cache\net_E_hashdiff.log >nul
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] E: --hash-diff did not name the corrupted field - see cache\net_E_hashdiff.log
    set /a FAILED+=1
    goto :eof
)
findstr /c:"1 field(s) differ" cache\net_E_hashdiff.log >nul
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] E: --hash-diff reported more than the single corrupted field
    set /a FAILED+=1
    goto :eof
)
echo   --hash-diff names LocalTransform.position (1 field): PASS
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
