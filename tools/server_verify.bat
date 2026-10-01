@echo off
rem server_verify.bat - 専用サーバ (Server.exe) + Runtime クライアントの実プロセス検証 (M81e)
rem   Server.exe と Runtime.exe を実際に UDP で繋いで起動し、各プロセスに .rep を録らせて
rem     (a) サーバ .rep と各クライアント .rep が、tick の重なる区間で**全 tick 一致**すること
rem         (クライアントの .rep は参加 tick のスナップショットを埋め込んで始まる = サーバ .rep の一部)
rem     (b) サーバ .rep を Debug / Release の Server.exe --replay-verify でオフライン再生して全 tick 一致
rem     (c) 同じ .rep を Runtime.exe --replay-verify (窓あり) でも一致 = 再生結果は role に依存しない
rem   を機械検証する。比較は --rep-diff --rep-diff-overlap (割れたら tick・レーン・項目名まで出る)。
rem
rem   使い方:  tools\server_verify.bat [ticks] [cases]
rem             ticks = クライアントが記録する確定 tick 数 (既定 600)
rem             cases = 回すケースの文字 (既定 ABCD。例: B だけ)
rem
rem   ケース A: Debug サーバ + Debug x2 / ロス 0%%
rem   ケース B: Debug サーバ + Debug / Release 混在 x3 / ロス 20%% / 途中参加 1 / 切断 -> 再接続 1
rem             (Debug と Release の GameLogic.dll は必ず別バイトなので --allow-game-mismatch を使うのはこのケースだけ)
rem   ケース C: desync 注入 (クライアントの sim を参加の 100 tick 後に 1 フィールド壊す) ->
rem             診断バンドル + 再同期 -> 再同期後の区間はサーバと一致、壊れた区間は壊した tick で割れる
rem   ケース D: Release サーバ + Release x4 / ロス 0%% -> サーバの tick 時間をログに出す (R3、4 ms 目標)
rem
rem   ★ネット越しの値が sim へ入るのは確定入力 (レーン入力 + SystemInputTick) だけ。クライアントの予測・巻き戻し・
rem     再同期が働いていても、確定した tick の .rep はサーバと 1 バイトも違わない、が中心の主張。
rem   ★CI では回さない (UDP + 複数プロセス + 実時間。net_verify.bat と同じ理由)。論理の回帰は Editor.exe --selftest の
rem     Server/client net self test (1 プロセス・偽トランスポート) が CI 側で押さえている。
rem   ★ビルドはしない。replay_verify.bat で焼いた Debug/Release の exe をそのまま使う。
setlocal enabledelayedexpansion
cd /d "%~dp0.."

rem ---- 自己呼び出しによるバックグラウンド実行 (下の :bg。net_verify.bat と同じ流儀) ----
if "%~1"=="__bg" goto :bg

set TICKS=600
if not "%~1"=="" set TICKS=%~1
set CASES=ABCD
if not "%~2"=="" set CASES=%~2

set DBG=bin\x64\Debug
set REL=bin\x64\Release
for %%E in (Runtime.exe Server.exe) do (
    if not exist %DBG%\%%E (echo [server_verify] %DBG%\%%E not found - build Debug first & exit /b 1)
    if not exist %REL%\%%E (echo [server_verify] %REL%\%%E not found - build Release first & exit /b 1)
)

rem --local-demo のシーンはコードから毎回組む。保存済みが残っているとロード経路に落ちてコード側の正解と食い違う
if exist cache\local_players.scene.json del /q cache\local_players.scene.json
del /q cache\sv_*.rep cache\sv_*.log cache\sv_*.code 2>nul

rem サーバとクライアントで一致が必要な起動オプション (configBits): 合成入力を両方に渡す
set SRV_ARGS=--local-demo --synth-input --max-players 4 --net-delay 3
set CLI_ARGS=--local-demo --synth-input --warp --no-audio
set FAILED=0

if not "!CASES:A=!"=="!CASES!" call :case_A
if not "!CASES:B=!"=="!CASES!" call :case_B
if not "!CASES:C=!"=="!CASES!" call :case_C
if not "!CASES:D=!"=="!CASES!" call :case_D

echo.
if not %FAILED%==0 (
    echo [FAIL] server_verify: %FAILED% check^(s^) failed - logs: cache\sv_*.log
    exit /b 1
)
echo [PASS] server_verify ^(cases !CASES!: A 2 clients / B 3 mixed Debug-Release clients, 20%% loss, late join, drop + rejoin / C desync injection + resync / D 4 Release clients^) - logs: cache\sv_*.log
exit /b 0

rem -------------------------------------------------------------------- 部品
rem %1 = 終了コードの置き場 / %2 = 実行するコマンド行 (リダイレクト込み)
:launch
start "mye sv" /b cmd /c call "%~f0" __bg "%~1" "%~2"
goto :eof

rem %1 = ファイル / %2 = 最大秒数。見つかれば WAITOK=1
:waitfile
set WAITOK=0
set /a W=0
:waitfile_loop
if exist "%~1" (
    set WAITOK=1
    goto :eof
)
set /a W+=1
if !W! GTR %~2 goto :eof
ping -n 2 127.0.0.1 >nul
goto :waitfile_loop

rem %1 = ログ / %2 = 探す文字列 / %3 = 最大秒数。見つかれば WAITOK=1
:waitlog
set WAITOK=0
set /a W=0
:waitlog_loop
findstr /c:%2 "%~1" >nul 2>&1
if !ERRORLEVEL! EQU 0 (
    set WAITOK=1
    goto :eof
)
set /a W+=1
if !W! GTR %~3 goto :eof
ping -n 2 127.0.0.1 >nul
goto :waitlog_loop

rem %1 = 秒
:sleep
set /a SLP=%~1+1
ping -n !SLP! 127.0.0.1 >nul
goto :eof

rem %1 = ケース名 / %2 = 表示名 / %3 = 終了コードのファイル / %4 = 待つ秒数 / %5 = 期待する終了コード (既定 0)
rem ★表示名に ( ) を入れない: if ( ... ) ブロックの中の echo に展開され、ブロックが途中で閉じて壊れる
:expect_exit
call :waitfile "%~3" %~4
if not "!WAITOK!"=="1" (
    echo   [FAIL] %~1: %~2 did not exit within %~4 s
    set /a FAILED+=1
    goto :eof
)
set /p XCODE=<"%~3"
set WANT=%~5
if "!WANT!"=="" set WANT=0
if not "!XCODE!"=="!WANT!" (
    echo   [FAIL] %~1: %~2 exited with !XCODE!, expected !WANT! - see cache\sv_%~1_*.log
    set /a FAILED+=1
    goto :eof
)
echo   %~2 exited with !XCODE!: ok
goto :eof

rem %1 = ケース名。そのケースのログのどれかが「C++ スクリプト 0 本の世界」で走っていないか
:check_scripts
findstr /c:"NO C++ scripts are registered" cache\sv_%~1_*.log >nul 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] %~1: a process started without C++ scripts ^(DLL shadow-copy race^) - see the logs
    set /a FAILED+=1
)
goto :eof

rem %1 = ケース名 / %2 = クライアント名 (c1 など)。そのクライアントが参加し終える (joined: の行) まで待つ
:wait_joined
call :waitlog cache\sv_%~1_%~2.log "joined: lane=" 240
if not "!WAITOK!"=="1" (
    echo   [FAIL] %~1: %~2 did not join the server within 240 s - see cache\sv_%~1_%~2.log
    set /a FAILED+=1
)
goto :eof

rem %1 = ケース名 / %2 = クライアント名 / %3 = サーバ .rep / %4 = クライアント .rep。重なり区間が一致すること
:check_agree
%DBG%\Server.exe --rep-diff "%~3" "%~4" --rep-diff-overlap 100 > cache\sv_%~1_diff_%~2.log 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] %~1: the server and %~2 did NOT record the same tick sequence - see cache\sv_%~1_diff_%~2.log
    findstr /c:"[rep-diff]" cache\sv_%~1_diff_%~2.log
    set /a FAILED+=1
    goto :eof
)
for /f "tokens=*" %%L in ('findstr /c:"[rep-diff]" cache\sv_%~1_diff_%~2.log') do echo   server == %~2: PASS  %%L
goto :eof

rem %1 = ケース名 / %2 = サーバ .rep。Debug / Release の Server.exe と Runtime.exe (窓あり) で再生して全 tick 一致
:check_replays
set REP=%~2
%DBG%\Server.exe --local-demo --replay-verify "%REP%" > cache\sv_%~1_verify_dbg_server.log 2>&1
call :check_verify %~1 "Debug Server.exe" cache\sv_%~1_verify_dbg_server.log !ERRORLEVEL!
%REL%\Server.exe --local-demo --replay-verify "%REP%" > cache\sv_%~1_verify_rel_server.log 2>&1
call :check_verify %~1 "Release Server.exe" cache\sv_%~1_verify_rel_server.log !ERRORLEVEL!
%DBG%\Runtime.exe --local-demo --warp --no-audio --replay-verify "%REP%" > cache\sv_%~1_verify_dbg_runtime.log 2>&1
call :check_verify %~1 "Debug Runtime.exe" cache\sv_%~1_verify_dbg_runtime.log !ERRORLEVEL!
%REL%\Runtime.exe --local-demo --warp --no-audio --replay-verify "%REP%" > cache\sv_%~1_verify_rel_runtime.log 2>&1
call :check_verify %~1 "Release Runtime.exe" cache\sv_%~1_verify_rel_runtime.log !ERRORLEVEL!
goto :eof

rem %1 = ケース名 / %2 = 表示名 / %3 = ログ / %4 = 終了コード
:check_verify
if not "%~4"=="0" (
    echo   [FAIL] %~1: %~2 --replay-verify of the server .rep failed ^(exit %~4^) - see %~3
    set /a FAILED+=1
    goto :eof
)
findstr /c:"PASS" "%~3" >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] %~1: %~2 --replay-verify did not report PASS - see %~3
    set /a FAILED+=1
    goto :eof
)
echo   replay by %~2: PASS
goto :eof

rem %1 = ケース名 / %2 = 表示名 / %3 = ログ。巻き戻しと参加・離脱をまたいだ再シムの記録を見せる
:show_client_stats
for /f "tokens=*" %%L in ('findstr /c:"[client] rollback:" "%~3"') do echo   %~2 %%L
goto :eof

rem %1 = ログ / %2 = 変数名。"joined: lane=L playerId=N snapshot" の N を取る (最初の 1 行)
:get_player_id
set "%~2="
for /f "tokens=3 delims==" %%a in ('findstr /c:"joined: lane=" "%~1"') do (
    if not defined %~2 for /f %%b in ("%%a") do set "%~2=%%b"
)
goto :eof

rem -------------------------------------------------------------------- ケース A
:case_A
set PORT=7821
echo.
echo === case A: Debug server + 2 Debug clients, loss 0%%, %TICKS% ticks, port %PORT% ===
call :launch "cache\sv_A_s.code" "%DBG%\Server.exe %SRV_ARGS% --port %PORT% --exit-when-empty --server-timeout 400 --replay-record cache\sv_A_server.rep > cache\sv_A_server.log 2>&1"
call :waitlog cache\sv_A_server.log "started: 4 lane" 120
if not "!WAITOK!"=="1" (
    echo   [FAIL] A: the server did not start - see cache\sv_A_server.log
    set /a FAILED+=1
    exit /b 0
)
rem client 1 は後から来る client 2 より十分長く居る (全員が出ていくと --exit-when-empty でサーバが終わるため)
set /a AT=%TICKS%*5
call :launch "cache\sv_A_1.code" "%DBG%\Runtime.exe %CLI_ARGS% --net-connect 127.0.0.1:%PORT% --player-session-id p1 --replay-ticks !AT! --replay-record cache\sv_A_c1.rep > cache\sv_A_c1.log 2>&1"
call :wait_joined A c1
call :launch "cache\sv_A_2.code" "%DBG%\Runtime.exe %CLI_ARGS% --net-connect 127.0.0.1:%PORT% --player-session-id p2 --replay-ticks %TICKS% --replay-record cache\sv_A_c2.rep > cache\sv_A_c2.log 2>&1"
call :expect_exit A "client 1" cache\sv_A_1.code 300
call :expect_exit A "client 2" cache\sv_A_2.code 300
call :expect_exit A "server" cache\sv_A_s.code 60
call :check_scripts A
call :check_agree A c1 cache\sv_A_server.rep cache\sv_A_c1.rep
call :check_agree A c2 cache\sv_A_server.rep cache\sv_A_c2.rep
call :show_client_stats A c1 cache\sv_A_c1.log
call :show_client_stats A c2 cache\sv_A_c2.log
call :check_replays A cache\sv_A_server.rep
exit /b 0

rem -------------------------------------------------------------------- ケース B
:case_B
set PORT=7822
set MIX=--allow-game-mismatch --net-loss 20
echo.
echo === case B: Debug server + Debug/Release/Release clients, loss 20%%, late join, drop + rejoin, port %PORT% ===
call :launch "cache\sv_B_s.code" "%DBG%\Server.exe %SRV_ARGS% %MIX% --port %PORT% --exit-when-empty --server-timeout 500 --replay-record cache\sv_B_server.rep > cache\sv_B_server.log 2>&1"
call :waitlog cache\sv_B_server.log "started: 4 lane" 120
if not "!WAITOK!"=="1" (
    echo   [FAIL] B: the server did not start - see cache\sv_B_server.log
    set /a FAILED+=1
    exit /b 0
)
rem client 1 (Debug) は最後まで居続ける。client 2 (Release) は参加の 240 tick 後に Bye 無しで消え、同じ playerId で戻る。
rem ★前のクライアントが参加し終えてから次を起動する: Debug は起動が遅く、先に参加した Release だけで全員が出ていくと
rem   --exit-when-empty でサーバが終わってしまう
call :launch "cache\sv_B_1.code" "%DBG%\Runtime.exe %CLI_ARGS% %MIX% --net-connect 127.0.0.1:%PORT% --player-session-id p1 --replay-ticks 3600 --replay-record cache\sv_B_c1.rep > cache\sv_B_c1.log 2>&1"
call :wait_joined B c1
call :launch "cache\sv_B_2.code" "%REL%\Runtime.exe %CLI_ARGS% %MIX% --net-connect 127.0.0.1:%PORT% --player-session-id p2 --net-drop-after 240 --replay-ticks 3600 --replay-record cache\sv_B_c2.rep > cache\sv_B_c2.log 2>&1"
rem client 3 (Release) は途中参加: client 2 が参加して 3 秒後
call :wait_joined B c2
call :sleep 3
call :launch "cache\sv_B_3.code" "%REL%\Runtime.exe %CLI_ARGS% %MIX% --net-connect 127.0.0.1:%PORT% --player-session-id p3 --replay-ticks %TICKS% --replay-record cache\sv_B_c3.rep > cache\sv_B_c3.log 2>&1"
rem client 2 が消えたら、ログの playerId を引いて再接続 (同じ player session ID・同じレーンへ戻るはず)
call :expect_exit B "client 2 dropped" cache\sv_B_2.code 300
call :get_player_id cache\sv_B_c2.log P2ID
if "!P2ID!"=="" (
    echo   [FAIL] B: no "joined: ... playerId=" line in the log of client 2 - see cache\sv_B_c2.log
    set /a FAILED+=1
    exit /b 0
)
echo   client 2 had playerId !P2ID!; reconnecting it
call :launch "cache\sv_B_2b.code" "%REL%\Runtime.exe %CLI_ARGS% %MIX% --net-connect 127.0.0.1:%PORT% --player-session-id p2 --net-player-id !P2ID! --replay-ticks %TICKS% --replay-record cache\sv_B_c2b.rep > cache\sv_B_c2b.log 2>&1"
call :expect_exit B "client 2 reconnected" cache\sv_B_2b.code 300
call :expect_exit B "client 3 late joiner" cache\sv_B_3.code 300
call :expect_exit B "client 1" cache\sv_B_1.code 400
call :expect_exit B "server" cache\sv_B_s.code 60
call :check_scripts B
call :check_agree B c1 cache\sv_B_server.rep cache\sv_B_c1.rep
call :check_agree B c2 cache\sv_B_server.rep cache\sv_B_c2.rep
call :check_agree B c2b cache\sv_B_server.rep cache\sv_B_c2b.rep
call :check_agree B c3 cache\sv_B_server.rep cache\sv_B_c3.rep
rem サーバ側の記録: 参加 3 / 再接続 1 / 離脱 >= 1
findstr /c:"joins 3, rejoins 1" cache\sv_B_server.log >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] B: the server did not report joins 3 / rejoins 1 - see cache\sv_B_server.log
    set /a FAILED+=1
) else (
    echo   server: joins 3 / rejoins 1: ok
)
rem 再シムがシステムイベント (参加・離脱) をまたいだ記録 (どのクライアントでもよい)
set EVTRESIM=0
findstr /c:"across" cache\sv_B_c1.log cache\sv_B_c2.log cache\sv_B_c2b.log cache\sv_B_c3.log | findstr /c:"system event" > cache\sv_B_resim_events.log
for %%F in (cache\sv_B_resim_events.log) do if %%~zF GTR 0 set EVTRESIM=1
if "!EVTRESIM!"=="1" (
    echo   rollbacks re-simulated across join/leave events:
    type cache\sv_B_resim_events.log
) else (
    echo   [WARN] B: no rollback re-simulated across a system event in this run ^(depends on timing^)
)
call :show_client_stats B c1 cache\sv_B_c1.log
call :show_client_stats B c2 cache\sv_B_c2.log
call :show_client_stats B c2b cache\sv_B_c2b.log
call :show_client_stats B c3 cache\sv_B_c3.log
call :check_replays B cache\sv_B_server.rep
exit /b 0

rem -------------------------------------------------------------------- ケース C
:case_C
set PORT=7823
echo.
echo === case C: desync injection (client 2 is corrupted 100 ticks after joining) + resync, port %PORT% ===
call :launch "cache\sv_C_s.code" "%DBG%\Server.exe %SRV_ARGS% --port %PORT% --exit-when-empty --server-timeout 400 --replay-record cache\sv_C_server.rep > cache\sv_C_server.log 2>&1"
call :waitlog cache\sv_C_server.log "started: 4 lane" 120
if not "!WAITOK!"=="1" (
    echo   [FAIL] C: the server did not start - see cache\sv_C_server.log
    set /a FAILED+=1
    exit /b 0
)
set /a CT=%TICKS%*5
call :launch "cache\sv_C_1.code" "%DBG%\Runtime.exe %CLI_ARGS% --net-connect 127.0.0.1:%PORT% --player-session-id p1 --replay-ticks !CT! --replay-record cache\sv_C_c1.rep > cache\sv_C_c1.log 2>&1"
call :wait_joined C c1
call :launch "cache\sv_C_2.code" "%DBG%\Runtime.exe %CLI_ARGS% --net-connect 127.0.0.1:%PORT% --player-session-id p2 --net-poke-after 100 --replay-ticks %TICKS% --replay-record cache\sv_C_c2.rep > cache\sv_C_c2.log 2>&1"
call :expect_exit C "client 2 corrupted" cache\sv_C_2.code 300
call :expect_exit C "client 1" cache\sv_C_1.code 300
call :expect_exit C "server" cache\sv_C_s.code 60
call :check_scripts C
findstr /c:"DESYNC at tick" cache\sv_C_c2.log >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [FAIL] C: client 2 did not detect the injected desync - see cache\sv_C_c2.log
    set /a FAILED+=1
) else (
    echo   client 2 detected the desync:
    findstr /c:"DESYNC at tick" /c:"bundle:" cache\sv_C_c2.log
)
findstr /c:"DESYNC" cache\sv_C_c1.log >nul 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] C: the healthy client 1 reported a desync - see cache\sv_C_c1.log
    set /a FAILED+=1
)
rem 壊した tick (ログの "corrupted on purpose at tick N") を取る
set POKETICK=
for /f "tokens=2 delims=:" %%a in ('findstr /c:"corrupted on purpose at tick" cache\sv_C_c2.log') do if not defined POKETICK for /f "tokens=*" %%b in ("%%a") do for %%w in (%%b) do set POKETICK=%%w
if "!POKETICK!"=="" (
    echo   [FAIL] C: no poke tick in the log of client 2
    set /a FAILED+=1
    exit /b 0
)
rem 1. 壊す前の区間 (client 2 の最初の .rep) は、壊した tick でサーバと割れる (= 注入が本当に効いた証拠)
%DBG%\Server.exe --rep-diff cache\sv_C_server.rep cache\sv_C_c2.rep --rep-diff-overlap 50 > cache\sv_C_diff_c2_before.log 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   [FAIL] C: the corrupted client's first .rep still equals the server's - the injection did nothing
    set /a FAILED+=1
) else (
    findstr /c:"tick !POKETICK!: world hash differs" cache\sv_C_diff_c2_before.log >nul 2>&1
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAIL] C: --rep-diff did not name tick !POKETICK! - see cache\sv_C_diff_c2_before.log
        set /a FAILED+=1
    ) else (
        echo   before the resync the corrupted client differs from the server at tick !POKETICK!: PASS
    )
)
rem 2. 再同期後の区間 (.rs1.rep) と、健全な client 1 は全 tick 一致
if not exist cache\sv_C_c2.rs1.rep (
    echo   [FAIL] C: no .rep after the resync ^(cache\sv_C_c2.rs1.rep^)
    set /a FAILED+=1
) else (
    call :check_agree C c2_after_resync cache\sv_C_server.rep cache\sv_C_c2.rs1.rep
)
call :check_agree C c1 cache\sv_C_server.rep cache\sv_C_c1.rep
findstr /c:"client desync reports 1" cache\sv_C_server.log >nul 2>&1
if !ERRORLEVEL! NEQ 0 (
    echo   [WARN] C: the server did not log exactly one client desync report ^(timing^) - see cache\sv_C_server.log
)
call :show_client_stats C c1 cache\sv_C_c1.log
call :show_client_stats C c2 cache\sv_C_c2.log
call :check_replays C cache\sv_C_server.rep
exit /b 0

rem -------------------------------------------------------------------- ケース D
:case_D
set PORT=7824
echo.
echo === case D: Release server + 4 Release clients, loss 0%%, port %PORT% ^(R3: server tick time^) ===
call :launch "cache\sv_D_s.code" "%REL%\Server.exe %SRV_ARGS% --port %PORT% --exit-when-empty --server-timeout 400 --replay-record cache\sv_D_server.rep > cache\sv_D_server.log 2>&1"
call :waitlog cache\sv_D_server.log "started: 4 lane" 120
if not "!WAITOK!"=="1" (
    echo   [FAIL] D: the server did not start - see cache\sv_D_server.log
    set /a FAILED+=1
    exit /b 0
)
rem 4 台とも Release (起動時間がそろう)。全員が重なる区間を長く取るため、記録する tick を多めにする
set /a DT=%TICKS%*3
for %%K in (1 2 3 4) do (
    call :launch "cache\sv_D_%%K.code" "%REL%\Runtime.exe %CLI_ARGS% --net-connect 127.0.0.1:%PORT% --player-session-id p%%K --replay-ticks !DT! --replay-record cache\sv_D_c%%K.rep > cache\sv_D_c%%K.log 2>&1"
    call :sleep 2
)
for %%K in (1 2 3 4) do call :expect_exit D "client %%K" cache\sv_D_%%K.code 300
call :expect_exit D "server" cache\sv_D_s.code 60
call :check_scripts D
for %%K in (1 2 3 4) do call :check_agree D c%%K cache\sv_D_server.rep cache\sv_D_c%%K.rep
echo   R3 - Release server tick time with 4 clients:
findstr /c:"tick time:" cache\sv_D_server.log
findstr /c:"exceeds the" cache\sv_D_server.log
call :check_replays D cache\sv_D_server.rep
exit /b 0

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
