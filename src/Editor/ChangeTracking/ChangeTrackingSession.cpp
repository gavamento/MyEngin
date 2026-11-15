//====================================================================================
//                          ChangeTrackingSession.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の持ち主 (接続・購読・モデル)
//====================================================================================
#include "Editor/ChangeTracking/ChangeTrackingSession.h"

#include <filesystem>
#include <optional>
#include <utility>

#include <Windows.h>

#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace mye {

bool HashFileForSavedNotice(const std::wstring& absPath, std::string& hexOut)
{
    std::error_code ec;
    const std::filesystem::path path(absPath);
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return false; // 消えた・フォルダ (フォルダの移動は個々のファイルを知らせない)
    }
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kTrackSavedHashMaxBytes) {
        return false;
    }
    return Sha256FileHex(absPath, hexOut);
}

void ChangeTrackingSession::Start(const std::wstring& exeDir, const std::wstring& projectRoot, bool enabled)
{
    // 無効・プロジェクト無しのときは、I/O の実体も作らない
    StartWithIo(enabled && !projectRoot.empty() ? CreateWin32TrackIo(exeDir) : nullptr, projectRoot, enabled);
}

void ChangeTrackingSession::StartWithIo(std::unique_ptr<TrackIo> io, const std::wstring& projectRoot, bool enabled)
{
    Shutdown();
    seenConnects_ = 0;
    projectRoot_ = projectRoot;
    pendingSaved_.clear();
    restore_.Configure(projectRoot, &client_);
    Wire();
    if (!enabled) {
        client_.SetIdle(TrackState::Disabled);
        return;
    }
    if (projectRoot.empty()) {
        client_.SetIdle(TrackState::NoProject);
        return;
    }
    client_.Start(std::move(io), projectRoot, static_cast<uint32_t>(GetCurrentProcessId()));
}

void ChangeTrackingSession::Wire()
{
    model_.Attach(&client_);
    review_.Attach(&client_, &model_);
    review_.Reset();
    overlay_.Attach(&client_);
    overlay_.Reset();
    client_.SetNotifyHandler([this](const nlohmann::json& event) {
        model_.OnNotification(event);
        overlay_.OnNotification(event);
    });
    client_.SetStateHandler([this](TrackState state) {
        if (state == TrackState::Ready && client_.GetStats().connects != seenConnects_) {
            // 新しい接続が通った (DbDegraded から戻っただけのときは、購読も取り直しもそのまま)
            seenConnects_ = client_.GetStats().connects;
            model_.OnConnected();
            overlay_.OnConnected();
            // 通知を受ける。失敗しても (古いサービスなど) 表示は取り直しで動く。通知なしで困るのは更新の遅れだけ
            client_.Request(trackop::kSubscribe,
                            { { "events", { trackevent::kChangeRecorded, trackevent::kAttributionUpdated,
                                            trackevent::kCoverageGap, trackevent::kSessionChanged,
                                            trackevent::kReviewChanged, trackevent::kShuttingDown } } },
                            [](const nlohmann::json& response) {
                                if (!response.value("ok", false)) {
                                    MYE_LOG_INFO("[track] subscribe に失敗: %s", response.dump().c_str());
                                }
                            });
        } else if (!client_.CanRequest()) {
            model_.OnDisconnected();
            overlay_.OnDisconnected();
        }
    });
}

void ChangeTrackingSession::Poll()
{
    client_.Poll();
    restore_.Poll();
    model_.Tick();
    overlay_.Tick();
    DrainSaved();
}

namespace {

// 大きさと最終更新時刻 (読めなければ nullopt)。保存の知らせの前後で外の書き込みが混ざっていないかの確認に使う
std::optional<std::pair<uint64_t, int64_t>> StatForSavedNotice(const std::wstring& absPath)
{
    std::error_code ec;
    const std::filesystem::path path(absPath);
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::nullopt;
    }
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return std::nullopt;
    }
    return std::make_pair(size, static_cast<int64_t>(time.time_since_epoch().count()));
}

// 末尾の区切りを落として、1 つだけ付ける (プロジェクトの中かの前方一致用)
std::wstring RootPrefixKey(const std::wstring& projectRoot)
{
    std::wstring key = NormalizePathKey(projectRoot);
    while (!key.empty() && key.back() == L'\\') {
        key.pop_back();
    }
    return key + L'\\';
}

} // namespace

void ChangeTrackingSession::NoteSaved(const std::wstring& absPath)
{
    // 無効・非対話起動・切断中・DB 停止中は、順番待ちにも積まない (ハッシュは計算しない)
    if (!client_.Ready() || projectRoot_.empty() || absPath.empty()) {
        return;
    }
    const std::wstring key = NormalizePathKey(absPath);
    const std::wstring prefix = RootPrefixKey(projectRoot_);
    if (key.size() <= prefix.size() || key.compare(0, prefix.size(), prefix) != 0) {
        return; // プロジェクトの外
    }
    // .mye の下 (サービス自身のデータ) は追跡の対象外
    constexpr std::wstring_view kInternalDir = L".mye\\";
    if (key.compare(prefix.size(), kInternalDir.size(), kInternalDir) == 0) {
        return;
    }
    if (pendingSaved_.size() >= kTrackSavedMaxPending && pendingSaved_.find(key) == pendingSaved_.end()) {
        return;
    }
    SavedEntry entry;
    entry.absPath = absPath;
    if (const auto stat = StatForSavedNotice(absPath)) {
        entry.stamp = FileStamp{ stat->first, stat->second };
    }
    pendingSaved_[key] = std::move(entry);
}

void ChangeTrackingSession::DrainSaved()
{
    if (pendingSaved_.empty()) {
        return;
    }
    if (!client_.Ready()) {
        pendingSaved_.clear(); // 切れた間の保存は、あとから知らせても照合できない
        return;
    }
    for (int budget = kTrackSavedPerPoll; budget > 0 && !pendingSaved_.empty(); --budget) {
        const auto first = pendingSaved_.begin();
        const SavedEntry entry = first->second;
        pendingSaved_.erase(first);
        const std::wstring& path = entry.absPath;
        // 知らされた時点と、ハッシュの直前・直後で、大きさと更新時刻が同じであること。
        // 違えばその間に別の書き込みがあった (外の変更を Editor の利用者の保存として確定しない)
        auto sameAsNoted = [&] {
            const auto stat = StatForSavedNotice(path);
            if (stat.has_value() != entry.stamp.has_value()) {
                return false;
            }
            return !stat || (stat->first == entry.stamp->size && stat->second == entry.stamp->writeTime);
        };
        if (!sameAsNoted()) {
            continue;
        }
        std::string hash;
        if (!hasher_ || !hasher_(path, hash) || hash.empty()) {
            continue;
        }
        if (!sameAsNoted()) {
            continue;
        }
        client_.Request(trackop::kEditorSaved, { { "path", WideToUtf8(path) }, { "hash", hash } },
                        [](const nlohmann::json& response) {
                            if (!response.value("ok", false)) {
                                MYE_LOG_INFO("[track] editor_saved が通らなかった: %s", response.dump().c_str());
                            }
                        });
    }
}

void ChangeTrackingSession::Shutdown()
{
    client_.Stop(); // 接続を閉じるだけ。サービスへ shutdown は送らない
}

} // namespace mye
