@echo off
rem mcp_verify.bat [--update] [シナリオ名.ndjson]
rem   エンジン MCP サーバ (tools\mcp) の回帰検証。エディタも D3D も要らない。
rem
rem 中身は mcp_verify.ps1 (cargo test -> cargo build --release -> tests\mcp\*.ndjson)。
rem --update は期待 NDJSON の撮り直し。PowerShell のパラメータは 1 本ダッシュなのでここで綴りを変換する。
rem CI にも --selftest にも入れない (M87 の方針)。ローカルで MCP を触ったときに手で回す。
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set PSARGS=
if /i "%~1"=="--update" (
    set PSARGS=-Update
    shift
)
if not "%~1"=="" set PSARGS=!PSARGS! -Scenario "%~1"

pwsh -NoProfile -File tools\mcp_verify.ps1 !PSARGS!
rem ★if errorlevel 1 では見ない (SEH の負のコードがすり抜ける — M52f)
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo [mcp_verify] FAILED
    exit /b 1
)
exit /b 0
