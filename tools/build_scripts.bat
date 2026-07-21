@echo off
rem build_scripts.bat <Config> -- regenerate project files then build GameLogic.dll (C++ scripts).
rem Invoked by the editor's "Rebuild Scripts" button. On success the running editor hot-reloads.
setlocal
cd /d "%~dp0.."
set CFG=%1
if "%CFG%"=="" set CFG=Debug

echo === regen project files (pick up new scripts) ===
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\gen_project_files.ps1 || ( echo [build_scripts] gen_project_files failed & pause & exit /b 1 )

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" ( echo [build_scripts] MSBuild not found & pause & exit /b 1 )

echo === Building GameLogic (%CFG%) ===
"%MSBUILD%" build\GameLogic.vcxproj /p:Configuration=%CFG% /p:Platform=x64 /m /v:minimal /nologo || ( echo. & echo [build_scripts] BUILD FAILED -- fix errors above & pause & exit /b 1 )

echo.
echo === GameLogic built. The editor will hot-reload within ~0.5s. ===
exit /b 0
