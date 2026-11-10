//====================================================================================
//                          InstallService.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          配布版のインストール・削除と、それを回すワーカー
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Engine/Engine/Update/EngineRelease.h"
#include "Engine/Engine/Update/InstallLayout.h"

namespace mye {

enum class InstallStage : uint8_t
{
    Idle,
    Checking,
    Downloading,
    Verifying,
    Extracting,
    Finishing,
    Deleting,
};

using InstallProgressFn = std::function<void(InstallStage stage, uint64_t done, uint64_t total)>;

struct InstallOutcome
{
    bool ok = false;
    bool cancelled = false;
    bool alreadyInstalled = false;
    std::string error;
    std::wstring installDir;
    std::string version; // 表示用 (v を外した表記)
};

// release を <root>\Engine\<版>\ へ入れる。順序: SHA-256 の取得 → ダウンロード → 照合 →
// staging へ展開 → install.json → rename。どの段で失敗しても既存の版と <版>\ には触れず、
// 書きかけ (.part / staging) は消す
InstallOutcome InstallRelease(const ReleaseSource& source, const ReleaseInfo& release, const std::wstring& root,
                              const InstallProgressFn& progress, const std::atomic<bool>* cancel);

// 先に <版>.deleting-* へ rename してから消す。rename できない (エディタが動いている) ときは何も消さない
InstallOutcome UninstallEngine(const std::wstring& installDir, const InstallProgressFn& progress);

// 前回の Launcher が落ちて残した staging / deleting / .part を消す。
// 他の Launcher が動いていないとき (単一起動の印を取れたとき) だけ呼ぶ
void CleanupStaleInstallWork(const std::wstring& root);

// エディタを起動する。projectDir が空なら Hub、あれば --project で開く
bool LaunchInstalledEditor(const std::wstring& installDir, const std::wstring& projectDir, std::string& error);

// 1 本のワーカーで確認・インストール・削除のどれか 1 つを回す (画面を止めないため)。
// 結果は Take* で 1 回だけ受け取る
class LauncherJob
{
public:
    enum class Kind : uint8_t
    {
        None,
        Check,
        Install,
        Uninstall,
    };

    LauncherJob() = default;
    LauncherJob(const LauncherJob&) = delete;
    LauncherJob& operator=(const LauncherJob&) = delete;
    ~LauncherJob();

    bool Busy() const { return running_.load(); }
    Kind CurrentKind() const;
    void StartCheck(const ReleaseSource& source, bool includePrerelease);
    void StartInstall(const ReleaseSource& source, const ReleaseInfo& release, const std::wstring& root);
    void StartUninstall(const std::wstring& installDir);
    void Cancel() { cancel_.store(true); }

    void Progress(InstallStage& stage, uint64_t& done, uint64_t& total) const;
    bool TakeCheckResult(FetchReleaseResult& out);
    bool TakeInstallResult(InstallOutcome& out); // インストールと削除の両方

private:
    void Launch(Kind kind, std::function<void()> body);
    void Join();
    void SetProgress(InstallStage stage, uint64_t done, uint64_t total);

    std::thread thread_;
    std::atomic<bool> running_{ false };
    std::atomic<bool> cancel_{ false };
    mutable std::mutex mutex_;
    Kind kind_ = Kind::None;
    InstallStage stage_ = InstallStage::Idle;
    uint64_t done_ = 0;
    uint64_t total_ = 0;
    bool hasCheck_ = false;
    FetchReleaseResult check_;
    bool hasInstall_ = false;
    InstallOutcome install_;
};

} // namespace mye
