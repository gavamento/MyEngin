@echo off
setlocal
cd /d "%~dp0.."
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "WATCHER_VS=%%i"
if not defined WATCHER_VS exit /b 1
call "%WATCHER_VS%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
if not exist cache mkdir cache
cl /nologo /std:c++20 /Zc:preprocessor /EHsc /utf-8 /W4 /WX /fp:strict /Od /Isrc tools\WatcherRulesSelfTest.cpp /Focache\WatcherRulesSelfTestDebug.obj /Fecache\WatcherRulesSelfTestDebug.exe || exit /b 1
cache\WatcherRulesSelfTestDebug.exe || exit /b 1
cl /nologo /std:c++20 /Zc:preprocessor /EHsc /utf-8 /W4 /WX /fp:strict /O2 /Isrc tools\WatcherRulesSelfTest.cpp /Focache\WatcherRulesSelfTestRelease.obj /Fecache\WatcherRulesSelfTestRelease.exe || exit /b 1
cache\WatcherRulesSelfTestRelease.exe || exit /b 1
exit /b 0
