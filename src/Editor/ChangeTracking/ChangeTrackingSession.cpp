//====================================================================================
//                          ChangeTrackingSession.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の持ち主 (接続・購読・モデル)
//====================================================================================
#include "Editor/ChangeTracking/ChangeTrackingSession.h"

#include <Windows.h>

#include "Editor/ChangeTracking/TrackProtocol.h"
#include "Engine/Core/Diagnostics/Log.h"

namespace mye {

void ChangeTrackingSession::Start(const std::wstring& exeDir, const std::wstring& projectRoot, bool enabled)
{
    // 無効・プロジェクト無しのときは、I/O の実体も作らない
    StartWithIo(enabled && !projectRoot.empty() ? CreateWin32TrackIo(exeDir) : nullptr, projectRoot, enabled);
}

void ChangeTrackingSession::StartWithIo(std::unique_ptr<TrackIo> io, const std::wstring& projectRoot, bool enabled)
{
    Shutdown();
    seenConnects_ = 0;
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
}

void ChangeTrackingSession::Shutdown()
{
    client_.Stop(); // 接続を閉じるだけ。サービスへ shutdown は送らない
}

} // namespace mye
