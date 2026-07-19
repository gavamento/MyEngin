@echo off
rem replay_verify.bat — Debug/Release 一貫性の自動検証 (engine_spec.md 11.3)
rem   1. 両構成をビルド
rem   2. Debug でゴールデンリプレイを記録
rem   3. Debug と Release の両方でハッシュ照合
rem   4. 静的規則チェック
rem 全て成功で exit 0、いずれか失敗で exit 1
setlocal
cd /d "%~dp0.."

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
    echo [replay_verify] MSBuild not found & exit /b 1
)

echo === build Debug ===
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo || exit /b 1
echo === build Release ===
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo || exit /b 1

set REP=cache\golden.rep
set TICKS=600
if not "%~1"=="" set TICKS=%~1

echo === record golden replay (Debug, %TICKS% ticks) ===
bin\x64\Debug\Editor.exe --replay-record %REP% --replay-ticks %TICKS% || exit /b 1

echo === verify in Debug ===
bin\x64\Debug\Editor.exe --replay-verify %REP% || (echo [FAIL] Debug verify & exit /b 1)

echo === verify in Release ===
bin\x64\Release\Editor.exe --replay-verify %REP% || (echo [FAIL] Release verify & exit /b 1)

echo === static rule check ===
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\check_rules.ps1 || (echo [FAIL] rule check & exit /b 1)

echo.
echo [PASS] replay consistency (Debug/Release) + rule check
exit /b 0
