@echo off
rem build_managed.bat <Config> -- build the managed C# scripting host (MyeScripting.dll).
rem Outputs to bin\x64\<Config>\ next to Editor.exe / Runtime.exe (loaded by CoreCLR at startup).
rem Separate from MyEngine.sln (native) so the documented MSBuild command stays restore-free.
setlocal
cd /d "%~dp0.."
set CFG=%1
if "%CFG%"=="" set CFG=Debug

where dotnet >nul 2>nul || ( echo [build_managed] dotnet SDK not found on PATH & exit /b 1 )

rem M52b: CI 固有の引数は環境変数で注入する (bat 本体はローカルと CI で同一)。
rem   MYE_DOTNET_ARGS … dotnet build へ後置 (CI では "/p:TreatWarningsAsErrors=true")
rem   ※ C# は TreatWarningsAsErrors、C++ は TreatWarningAsError と綴りが違う
echo === Building MyeScripting (%CFG%) — C# scripting host + Roslyn ===
dotnet build src\Scripting\MyeScripting.csproj -c %CFG% -v minimal %MYE_DOTNET_ARGS% || ( echo. & echo [build_managed] BUILD FAILED & exit /b 1 )

echo.
echo === MyeScripting.dll built to bin\x64\%CFG%\ ===
exit /b 0
