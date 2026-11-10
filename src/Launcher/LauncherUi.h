//====================================================================================
//                          LauncherUi.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          Launcher の画面 (最新版・インストール済みの版)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/Update/EngineRelease.h"
#include "Engine/Engine/Update/InstallLayout.h"
#include "Launcher/InstallService.h"

namespace mye {

struct LauncherOptions
{
    ReleaseSource source;
    bool includePrerelease = false;
    std::wstring root; // InstallRoot()
    // --update: エディタの「更新して再起動」から来た。最新版を入れたらエディタを開いて終わる
    bool updateMode = false;
    std::wstring launchProject; // --launch-project (updateMode でエディタに渡す)
};

struct LauncherState
{
    LauncherOptions options;
    LauncherJob job;
    bool checkDone = false;
    FetchReleaseResult latest;
    std::vector<InstalledEngine> installed;
    std::string message; // 直近の結果 (成功・失敗)
    bool messageIsError = false;
    std::wstring deleteTarget; // 非空で削除確認を開く
    std::string deleteLabel;
    bool updateFlowDone = false;
    bool inStartMenu = true; // LauncherBegin で調べる。false の間は登録ボタンを出す
};

// 起動直後に 1 回: インストール済みの走査と最新版の確認を始める
void LauncherBegin(LauncherState& st);
// 毎フレーム: ワーカーの結果を拾い、--update の流れを進める。false = 終了する
bool LauncherTick(LauncherState& st);
// 毎フレーム: 画面。false = 終了する
bool DrawLauncherUi(LauncherState& st);

} // namespace mye
