@echo off
rem bisect_replay.bat — 決定論の退行を git bisect で二分探索するラッパ (M52a)
rem
rem 「いつからハッシュが割れ始めたか」を人手で探すのをやめるための道具。
rem 各コミットで Debug をビルドし、既知の good コミットで録った golden を照合する。
rem
rem 使い方:
rem   1. まだ壊れていないコミット (good) で golden を録る。これがそのまま「正解」になる:
rem        bin\x64\Debug\Editor.exe --replay-record cache\bisect.rep --replay-ticks 600
rem      ※ cache\ は git 管理外なので checkout を跨いでも残る
rem   2. git bisect start <bad> <good>
rem   3. git bisect run tools\bisect_replay.bat cache\bisect.rep [--parts-demo]
rem
rem exit 0 = good (600 tick ハッシュ一致) / 1 = bad (MISMATCH) / 125 = skip (ビルド不能)
rem
rem bad と判定されたコミットには <rep>.tick<N>.actual.dump が残るので、
rem bisect 終了後に `Editor.exe --hash-diff` でどのフィールドが割れたかまで辿れる
rem (期待側は good コミットで --hash-dump-tick <N> を付けて録り直す)。
rem
rem ★注意: シーンの内容やコンポーネント構成が変わったコミットも「bad」になる。
rem   golden はエンティティ構成が同じ範囲でしか意味を持たない
setlocal
cd /d "%~dp0.."

set "REP=%~1"
if "%REP%"=="" (
    echo [bisect] usage: bisect_replay.bat ^<golden.rep^> [extra editor args] & exit /b 125
)
shift

rem 残りの引数 (--parts-demo 等) をそのまま Editor へ渡す
set "EXTRA="
:collect
if "%~1"=="" goto collected
set "EXTRA=%EXTRA% %~1"
shift
goto collect
:collected

if not exist "%REP%" (
    echo [bisect] golden replay not found: %REP% & exit /b 125
)

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
    echo [bisect] MSBuild not found - skip & exit /b 125
)

"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo || (
    echo [bisect] build failed - skip this commit & exit /b 125
)

rem 直前のコミットのマーカーを掴まないように消してから照合する
if exist "%REP%.mismatch.txt" del /q "%REP%.mismatch.txt"

bin\x64\Debug\Editor.exe %EXTRA% --replay-verify "%REP%" || (
    echo [bisect] BAD: hash mismatch & exit /b 1
)
echo [bisect] GOOD: hashes match
exit /b 0
