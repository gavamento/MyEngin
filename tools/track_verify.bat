@echo off
rem track_verify.bat [--update] [シナリオ名.ndjson]
rem   AI Change Tracking の追跡サービス (mye_track.exe) の回帰検証。エディタも D3D も要らない。
rem
rem 中身は track_verify.ps1。ローカルでも CI でも**この bat をそのまま呼ぶ**。
rem 先に tools\build_track.bat で mye_track.exe を作っておくこと (tools\track\target\release を使う)。
rem
rem --update は期待 NDJSON の撮り直し。PowerShell のパラメータは 1 本ダッシュなのでここで綴りを変換する
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set PSARGS=
if /i "%~1"=="--update" (
    set PSARGS=-Update
    shift
)
if not "%~1"=="" set PSARGS=!PSARGS! -Scenario "%~1"

pwsh -NoProfile -File tools\track_verify.ps1 !PSARGS!
rem ★if errorlevel 1 では見ない (SEH の負のコードがすり抜ける — M52f)
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo [track_verify] FAILED
    exit /b 1
)
exit /b 0
