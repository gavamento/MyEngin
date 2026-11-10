//====================================================================================
//                          InstallLayout.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher が入れた配布版の置き場と印 (install.json)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/Update/EngineRelease.h"

namespace mye {

// 置き場 (M98、ADR-037):
//   <root>\Engine\<版>\           配布 zip の中身 (リポジトリと同じ形) + install.json
//   <root>\Engine\<版>.staging-*  展開中 / <版>.deleting-* 削除中
//   <root>\Launcher\MyEngineLauncher.exe
//   <root>\Downloads\*.part
// <root> は %LOCALAPPDATA%\MyEngine。検証では MYE_INSTALL_ROOT で差し替える
constexpr const wchar_t* kInstallRootEnv = L"MYE_INSTALL_ROOT";
constexpr const wchar_t* kInstallMarkerFile = L"install.json";
constexpr const wchar_t* kLauncherExeName = L"MyEngineLauncher.exe";
constexpr const wchar_t* kStagingInfix = L".staging-";
constexpr const wchar_t* kDeletingInfix = L".deleting-";

std::wstring InstallRoot();
std::wstring EnginesDir(const std::wstring& root);
std::wstring LauncherDir(const std::wstring& root);
std::wstring DownloadsDir(const std::wstring& root);

// 配布版のフォルダから見た exe の場所 (make_dist.ps1 の構成と同じ)
std::wstring InstalledBinDir(const std::wstring& installDir);
std::wstring InstalledEditorPath(const std::wstring& installDir);

struct InstallMarker
{
    std::string version; // 先頭の v を外した表記 (フォルダ名と同じ)
    std::string tag;
    std::string sha256; // 入れた zip の SHA-256
    std::string installedAt; // UTC の ISO 8601 (表示用)
};
bool ReadInstallMarker(const std::wstring& installDir, InstallMarker& out);
bool WriteInstallMarker(const std::wstring& installDir, const InstallMarker& marker);

struct InstalledEngine
{
    EngineVersion version;
    std::wstring dir;
    InstallMarker marker;
};
// install.json があり、フォルダ名と印の版が一致するものだけ。新しい順
std::vector<InstalledEngine> ScanInstalledEngines(const std::wstring& root);

// Launcher が最後に動いた場所 (<root>\launcher_location.json)。エディタの「更新して再起動」が Launcher を探すのに使う。
// Launcher は自分を勝手に複製しない (自分を写して居座る動きは振る舞い検知に疑われる、ADR-037 D7) ので、
// 利用者が置いた場所を記録して辿る
bool RecordLauncherLocation(const std::wstring& root, const std::wstring& launcherExe);
std::wstring RecordedLauncherPath(const std::wstring& root); // 記録が無い・ファイルが消えていれば空

// exe のフォルダ (<install>\bin\x64\Release) から、Launcher が入れた配布版のフォルダを返す。
// 印が無い (リポジトリのビルドや手で展開した zip) なら空
std::wstring FindManagedInstallDir(const std::wstring& exeDir);

} // namespace mye
