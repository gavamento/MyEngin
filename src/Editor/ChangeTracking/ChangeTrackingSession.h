//====================================================================================
//                          ChangeTrackingSession.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の持ち主 (宣言)
//====================================================================================
#pragma once
#include <memory>
#include <string>

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/TrackClient.h"

namespace mye {

// エディタ 1 プロセス分の AI Change Tracking の持ち主 (SourceControlSession の形)。
// 窓ではなく状態: 窓が閉じていても接続と通知の受信は続き、開いたときに取り直す。
// **Shutdown は接続を閉じるだけでサービスを止めない** (Editor を閉じても記録が続く。architecture.md §3)
class ChangeTrackingSession {
public:
    ChangeTrackingSession() = default;
    ChangeTrackingSession(const ChangeTrackingSession&) = delete;
    ChangeTrackingSession& operator=(const ChangeTrackingSession&) = delete;

    // 起動時に 1 回。enabled が偽 (設定で無効 / 非対話起動) なら I/O の口を 1 回も呼ばず Disabled、
    // projectRoot が空なら NoProject。exeDir は mye_track.exe を探す場所 (Editor の exe の隣)
    void Start(const std::wstring& exeDir, const std::wstring& projectRoot, bool enabled);
    // 試験用: I/O を差し替えて始める (Start と同じ条件の判定)
    void StartWithIo(std::unique_ptr<TrackIo> io, const std::wstring& projectRoot, bool enabled);

    // 毎フレーム 1 回 (OnImGui の先頭)。待たない
    void Poll();
    void Shutdown();

    TrackState State() const { return client_.State(); }
    TrackClient& Client() { return client_; }
    ChangeModel& Model() { return model_; }
    const ChangeModel& Model() const { return model_; }

private:
    void Wire();

    // ★宣言順に意味がある: client_ のデストラクタが保留中の要求を失敗で返し、そのコールバックが model_ に触る。
    //   model_ を先に宣言して、後に壊れるようにする
    ChangeModel model_;
    TrackClient client_;
    uint32_t seenConnects_ = 0;
};

} // namespace mye
