//====================================================================================
//                          LauncherSelf.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher 自身の配置・自己更新・依存の導入 (Setup.ps1)
//====================================================================================
#pragma once
#include <atomic>
#include <string>

#include "Engine/Engine/Update/EngineRelease.h"
#include "Launcher/InstallService.h"

namespace mye {

// 動いている MyEngineLauncher.exe の絶対パス
std::wstring RunningLauncherPath();

// 利用者が押したときだけ: 自分を <root>\Launcher\MyEngineLauncher.exe へ写し (置いてある方が新しければ写さない)、
// スタートメニューに「MyEngine Launcher」を作る。起動時に自動ではやらない — 自分を写して居座る動きは
// 振る舞い検知に疑われる (v0.86 で Behavior:Win32/DefenseEvasion.A!ml に止められた、ADR-037 D7)
bool AddLauncherToStartMenu(const std::wstring& root, std::string& error);
bool IsLauncherInStartMenu();

// launcherExe の隣に残った前回の自己更新の *.old.exe / *.new.exe を消す
void CleanupOldLauncherFiles(const std::wstring& launcherExe);

// <root>\Launcher\launcher.json に書いた、置き場の Launcher の版 (無ければ無効)
EngineVersion PlacedLauncherVersion(const std::wstring& root);

// release に自分より新しい Launcher (exe と .sha256) があるか
bool LauncherUpdateAvailable(const ReleaseInfo& release);

// 新しい Launcher を targetExe の隣 (MyEngineLauncher.new.exe) へ落とし、印とスキャンを通して SHA-256 を照合する
InstallOutcome DownloadLauncherUpdate(const ReleaseSource& source, const ReleaseInfo& release,
                                      const std::wstring& targetExe, const InstallProgressFn& progress,
                                      const std::atomic<bool>* cancel);

// .new.exe を targetExe と入れ替える (動いている本体は .old.exe へ rename する。実行中でも rename はできる)。
// 置き場の Launcher なら launcher.json の版も書き換える。startNew なら入れ替えた Launcher を起動する
bool ApplyLauncherUpdate(const std::wstring& targetExe, const std::wstring& root, const std::string& newVersion,
                         bool startNew, std::string& error);

// 配布版に同梱の Setup.ps1 (VC++ 再頒布・.NET 8・Git を winget で入れる) を別のコンソール窓で -NoLaunch 実行する。
// C++ Build Tools を入れるかの y/N を聞くので、隠さずに見せる
bool RunSetupScript(const std::wstring& installDir, std::string& error);

} // namespace mye
