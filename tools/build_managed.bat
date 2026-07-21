@echo off
rem build_managed.bat <Config> -- build the managed C# scripting host (MyeScripting.dll).
rem Outputs to bin\x64\<Config>\ next to Editor.exe / Runtime.exe (loaded by CoreCLR at startup).
rem Separate from MyEngine.sln (native) so the documented MSBuild command stays restore-free.
setlocal
cd /d "%~dp0.."
set CFG=%1
if "%CFG%"=="" set CFG=Debug

where dotnet >nul 2>nul || ( echo [build_managed] dotnet SDK not found on PATH & exit /b 1 )

echo === Building MyeScripting (%CFG%) — C# scripting host + Roslyn ===
dotnet build src\Scripting\MyeScripting.csproj -c %CFG% -v minimal || ( echo. & echo [build_managed] BUILD FAILED & exit /b 1 )

echo.
echo === MyeScripting.dll built to bin\x64\%CFG%\ ===
exit /b 0
