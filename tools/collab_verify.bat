@echo off
rem collab_verify.bat [--update] [シナリオ名.ndjson]
rem   Source Control (MyeCollab) の回帰検証。エディタも D3D も要らない。
rem
rem 中身は collab_verify.ps1。ローカルでも CI でも**この bat をそのまま呼ぶ**
rem (CI 専用の検証ロジックを書かないため)。
rem 先に tools\build_collab.bat で MyeCollabCli.exe を作っておくこと。
rem
rem --update は期待 NDJSON の撮り直し (shot_verify.bat と同じ綴り)。PowerShell の
rem パラメータは 1 本ダッシュなのでここで綴りを変換する
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set PSARGS=
if /i "%~1"=="--update" (
    set PSARGS=-Update
    shift
)
if not "%~1"=="" set PSARGS=!PSARGS! -Scenario "%~1"

pwsh -NoProfile -File tools\collab_verify.ps1 !PSARGS!
rem ★if errorlevel 1 では見ない (SEH の負のコードがすり抜ける — M52f)
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo [collab_verify] FAILED
    exit /b 1
)
exit /b 0
