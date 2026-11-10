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

// ダウンロードフォルダなどから起動されたら、自分を <root>\Launcher\MyEngineLauncher.exe へ写す
// (そこにある版より自分が古ければ写さない)。createShortcut ならスタートメニューに
// 「MyEngine Launcher」を作る (検証用の --install-root では作らない)
void PlaceLauncherCopy(const std::wstring& root, bool createShortcut);

// 前回の自己更新が残した *.old.exe / *.new.exe を消す
void CleanupOldLauncherFiles(const std::wstring& root);

// <root>\Launcher\launcher.json に書いた、置いてある Launcher の版 (無ければ無効)
EngineVersion PlacedLauncherVersion(const std::wstring& root);

// release に自分より新しい Launcher (exe と .sha256) があるか
bool LauncherUpdateAvailable(const ReleaseInfo& release);

// 新しい Launcher を <root>\Launcher\MyEngineLauncher.new.exe へ落として SHA-256 を照合する
InstallOutcome DownloadLauncherUpdate(const ReleaseSource& source, const ReleaseInfo& release, const std::wstring& root,
                                      const InstallProgressFn& progress, const std::atomic<bool>* cancel);

// .new.exe を本体と入れ替える (動いている本体は .old.exe へ rename する。実行中でも rename はできる)。
// startNew なら入れ替えた Launcher を起動する。呼び出し側はその後に終了する
bool ApplyLauncherUpdate(const std::wstring& root, const std::string& newVersion, bool startNew, std::string& error);

// 配布版に同梱の Setup.ps1 (VC++ 再頒布・.NET 8・Git を winget で入れる) を別のコンソール窓で -NoLaunch 実行する。
// C++ Build Tools を入れるかの y/N を聞くので、隠さずに見せる
bool RunSetupScript(const std::wstring& installDir, std::string& error);

} // namespace mye
