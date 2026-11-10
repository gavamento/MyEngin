//====================================================================================
//                          UpdateWindow.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          エディタの更新確認・更新窓・Help メニュー・About
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "Engine/Engine/Update/EngineRelease.h"

namespace mye {

class ToastCenter;

// 1 日 1 回の自動確認をするか (enabled かつ前回から 24 時間以上。時計が戻っていても確認する)
bool ShouldAutoCheckUpdate(bool enabled, int64_t lastCheckUnix, int64_t nowUnix);

// 更新に使う Launcher: 置き場 (<root>\Launcher) を優先し、無ければこのエディタの隣。見つからなければ空
std::wstring FindLauncherForUpdate(const std::wstring& installRoot, const std::wstring& exeDir);

// 更新の確認 (M98d)。GitHub Releases の最新版を別スレッドで見に行き、新しければ知らせる。
// 「更新して再起動」は Launcher (--update --launch-project) に任せる: 動いている exe は自分を
// 上書きできず、版ごとの置き場 (InstallLayout) を扱えるのは Launcher だけなので
class UpdateWindow
{
public:
    bool open = false;

    UpdateWindow() = default;
    UpdateWindow(const UpdateWindow&) = delete;
    UpdateWindow& operator=(const UpdateWindow&) = delete;
    ~UpdateWindow();

    // 起動時の自動確認 (設定が ON で前回から 24 時間以上たっていれば)
    void StartupCheck();
    // メニューバーの中で呼ぶ
    void DrawHelpMenu();
    // 毎フレーム。true = 「更新して再起動」が押された (呼び出し側が未保存の確認をしてから LaunchUpdater)
    bool OnImGui(ToastCenter& toasts);

    // Launcher を --update で起動する。成功したらエディタは終了してよい
    static bool LaunchUpdater(const std::wstring& projectRoot, std::string& error);

private:
    void StartCheck(bool manual);
    void PollResult(ToastCenter& toasts);
    void DrawUpdateWindow();
    void DrawAbout();

    std::thread worker_;
    std::atomic<bool> busy_{ false };
    std::atomic<bool> cancel_{ false };
    std::mutex mutex_;
    bool hasPending_ = false;
    FetchReleaseResult pending_;

    bool checked_ = false;
    bool manual_ = false;
    FetchReleaseResult latest_;
    bool aboutOpenRequest_ = false;
    bool requestUpdate_ = false;
    bool settingsLoaded_ = false;
    bool autoCheck_ = true;
    std::wstring managedDir_; // Launcher が入れた版の置き場 (空 = ソースからのビルド)
    bool managedChecked_ = false;
};

} // namespace mye
