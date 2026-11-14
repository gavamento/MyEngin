@echo off
rem build_track.bat <Config> -- mye_track.exe (AI Change Tracking の追跡サービス、Rust) を作る。
rem
rem MyEngine.sln の外にある (build_collab.bat と同じ立場)。cargo は release を 1 回だけ回し、
rem 成果物を bin\x64\Debug\ と bin\x64\Release\ の**両方**へ置く: Editor は自分の exe の隣の
rem mye_track.exe を探すので、どちらの構成で起動しても同じものが見つかる。
rem 引数 <Config> は他の bat と呼び方を揃えるためだけに受ける (出力先は常に両方)。
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set CFG=%1
if "%CFG%"=="" set CFG=Debug

rem cargo の解決: PATH -> 無ければ rustup の既定インストール先 (build_collab.bat と同じ。M66a)
set CARGO=
for /f "delims=" %%i in ('where cargo 2^>nul') do if not defined CARGO set CARGO=%%i
if not defined CARGO if exist "%USERPROFILE%\.cargo\bin\cargo.exe" set CARGO=%USERPROFILE%\.cargo\bin\cargo.exe
if not defined CARGO (
    echo [build_track] cargo not found on PATH nor in "%USERPROFILE%\.cargo\bin".
    echo [build_track] install rustup stable from https://rustup.rs then reopen the shell.
    exit /b 1
)

echo === Building mye_track - Rust - requested config: %CFG% ===
"%CARGO%" build --release --manifest-path tools\track\Cargo.toml
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo [build_track] BUILD FAILED
    echo [build_track] mye_track needs Rust 1.89 or newer - run "rustc --version"
    exit /b 1
)

rem ★終了コードは if errorlevel 1 で見ない (SEH の負のコードがすり抜ける — M52f)。数値比較の NEQ 0 で書く
set SRCDIR=tools\track\target\release
for %%C in (Debug Release) do (
    if not exist "bin\x64\%%C" mkdir "bin\x64\%%C"
    copy /y "%SRCDIR%\mye_track.exe" "bin\x64\%%C\mye_track.exe" >nul
    if !ERRORLEVEL! NEQ 0 (
        echo [build_track] cannot copy mye_track.exe to bin\x64\%%C - is the service running?
        exit /b 1
    )
)

echo.
echo === mye_track.exe copied to bin\x64\Debug\ and bin\x64\Release\ ===
exit /b 0
