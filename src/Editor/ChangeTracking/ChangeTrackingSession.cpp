//====================================================================================
//                          ChangeTrackingSession.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の持ち主 (接続・購読・モデル)
//====================================================================================
#include "Editor/ChangeTracking/ChangeTrackingSession.h"

#include <filesystem>

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
    client_.SetNotifyHandler([this](const nlohmann::json& event) { model_.OnNotification(event); });
    client_.SetStateHandler([this](TrackState state) {
        if (state == TrackState::Ready && client_.GetStats().connects != seenConnects_) {
            // 新しい接続が通った (DbDegraded から戻っただけのときは、購読も取り直しもそのまま)
            seenConnects_ = client_.GetStats().connects;
            model_.OnConnected();
            // 通知を受ける。失敗しても (古いサービスなど) 表示は取り直しで動く。通知なしで困るのは更新の遅れだけ
            client_.Request(trackop::kSubscribe,
                            { { "events", { trackevent::kChangeRecorded, trackevent::kAttributionUpdated,
                                            trackevent::kCoverageGap, trackevent::kSessionChanged,
                                            trackevent::kShuttingDown } } },
                            [](const nlohmann::json& response) {
                                if (!response.value("ok", false)) {
                                    MYE_LOG_INFO("[track] subscribe に失敗: %s", response.dump().c_str());
                                }
                            });
        } else if (!client_.CanRequest()) {
            model_.OnDisconnected();
        }
    });
}

void ChangeTrackingSession::Poll()
{
    client_.Poll();
    model_.Tick();
    DrainSaved();
}

namespace {

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
    pendingSaved_[key] = absPath;
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
        const std::wstring path = first->second;
        pendingSaved_.erase(first);
        std::string hash;
        if (!hasher_ || !hasher_(path, hash) || hash.empty()) {
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
