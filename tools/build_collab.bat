@echo off
rem build_collab.bat <Config> -- MyeCollab.dll (Rust cdylib) と MyeCollabCli.exe を作る。
rem
rem MyEngine.sln の外にある (C# の build_managed.bat と同じ立場)。cargo は release を
rem 1 回だけ回し、成果物を bin\x64\Debug\ と bin\x64\Release\ の**両方**へ置く:
rem Rust 側に構成の区別を持ち込まない = Debug の Editor でも同じ DLL を読む。
rem 引数 <Config> は他の bat と呼び方を揃えるためだけに受ける (出力先は常に両方)。
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set CFG=%1
if "%CFG%"=="" set CFG=Debug

rem cargo の解決: PATH -> 無ければ rustup の既定インストール先。
rem ★rustup を入れた後に開き直していないシェルでは PATH に載っていない
rem   (環境変数はプロセス起動時に固定される)。M66a で実際に踏んだのでフォールバックを持つ
set CARGO=
for /f "delims=" %%i in ('where cargo 2^>nul') do if not defined CARGO set CARGO=%%i
if not defined CARGO if exist "%USERPROFILE%\.cargo\bin\cargo.exe" set CARGO=%USERPROFILE%\.cargo\bin\cargo.exe
if not defined CARGO (
    echo [build_collab] cargo not found on PATH nor in "%USERPROFILE%\.cargo\bin".
    echo [build_collab] install rustup stable from https://rustup.rs then reopen the shell.
    exit /b 1
)

echo === Building MyeCollab - Rust cdylib + CLI - requested config: %CFG% ===
"%CARGO%" build --release --manifest-path tools\collab\Cargo.toml
if !ERRORLEVEL! NEQ 0 (
    echo.
    echo [build_collab] BUILD FAILED
    exit /b 1
)

rem ★終了コードは if errorlevel 1 で見ない。SEH で落ちた負のコードが「1 以上か」の
rem   判定をすり抜ける (M52f)。数値比較の NEQ 0 で書く
set SRCDIR=tools\collab\target\release
for %%C in (Debug Release) do (
    if not exist "bin\x64\%%C" mkdir "bin\x64\%%C"
    copy /y "%SRCDIR%\mye_collab.dll" "bin\x64\%%C\MyeCollab.dll" >nul
    if !ERRORLEVEL! NEQ 0 (
        echo [build_collab] cannot copy MyeCollab.dll to bin\x64\%%C - is the editor running?
        exit /b 1
    )
    copy /y "%SRCDIR%\mye_collab_cli.exe" "bin\x64\%%C\MyeCollabCli.exe" >nul
    if !ERRORLEVEL! NEQ 0 (
        echo [build_collab] cannot copy MyeCollabCli.exe to bin\x64\%%C
        exit /b 1
    )
)

echo.
echo === MyeCollab.dll / MyeCollabCli.exe copied to bin\x64\Debug\ and bin\x64\Release\ ===
exit /b 0
