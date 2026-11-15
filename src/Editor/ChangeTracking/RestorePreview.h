//====================================================================================
//                          RestorePreview.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元のプレビューの準備 (内容を集めて計画を作る。書かない、宣言)
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/RestoreFileIo.h"
#include "Editor/ChangeTracking/RestorePlan.h"

namespace mye {

class TrackClient;

// 今のファイルを 1 回の Tick で読む量 (フレームを止めない。これより大きいファイルは数フレームに分けて読む)
constexpr size_t kRestoreReadChunkBytes = 1024u * 1024u;

// 復元のプレビュー 1 件の状態。AI Changes 窓の「復元…」を押すと、必要な内容 (describe と Hunk はモデルから、Before の Blob の有無は blob_get、
// 今のファイルは RestoreFileIo) を集めて RestorePlan を作る。**作業ツリーを書かない**: 使うのは RestoreFileIo の読み取りと、
// TrackClient の blob_get だけ。追跡に接続できていないときは I/O を 1 回も呼ばない
class RestorePreview {
public:
    enum class Phase : uint8_t {
        Closed,
        Preparing, // 今のファイルを読んでいる / Blob を確かめている
        Ready,     // 計画ができた (または、接続できていない・検査で拒否されたので、計画なしで止まった)
        Failed,    // 準備できなかった (サービスの応答の失敗など)
    };

    // 開く。detail = describe の結果、allHunks = モデルが持つ Revision の全 Hunk (File のとき見せる)、
    // selectedHunkIds = Hunk 単位のとき戻す Hunk の ID。client が要求を送れなければ I/O を呼ばずに Ready (計画なし) になる。
    // 準備中のプレビューがあれば捨てて開き直す
    void Open(RestoreScope scope, const nlohmann::json& detail, const std::vector<TrackHunk>& allHunks, const std::vector<int64_t>& selectedHunkIds,
              const std::wstring& projectRoot, TrackClient& client, RestoreFileIo& io);
    // 毎フレーム 1 回。今のファイルを 1 回につき kRestoreReadChunkBytes だけ読み進める
    void Tick(RestoreFileIo& io);
    void Close();

    Phase GetPhase() const { return phase_; }
    bool IsOpen() const { return phase_ != Phase::Closed; }
    // 計画 (Ready で、接続できていた・検査を通ったときだけ。無ければ偽)
    bool HasPlan() const { return hasPlan_; }
    const RestorePlan& Plan() const { return plan_; }
    const RestoreRevision& Revision() const { return revision_; }
    RestoreScope Scope() const { return scope_; }
    // 戻す対象の Hunk (Hunk 単位) / 見せる Hunk (ファイル全体)
    const std::vector<TrackHunk>& Hunks() const { return hunks_; }
    const NowState& Now() const { return now_; }
    const NowState& NowOld() const { return nowOld_; }
    // 復元が触るパス (ゲートの対象)
    const std::vector<std::string>& TargetPaths() const { return targetPaths_; }
    // Before の Blob が欠けている (blob_get が blob_missing を返した、または describe が欠けていると言っている)
    bool BlobMissing() const { return blobMissing_; }
    // 準備したときに追跡へ接続できていたか
    bool Connected() const { return connected_; }
    // Failed のとき
    const std::string& ErrorCode() const { return errorCode_; }
    const std::string& ErrorDetail() const { return errorDetail_; }
    // 差分 (Ready のとき): 戻すと変わる内容 (選んだ Hunk / 全 Hunk を逆向きに)、競合した Hunk の位置の今と記録した After
    const std::vector<DiffHunkInput>& ChangeView() const { return changeView_; }
    const std::vector<DiffHunkInput>& ConflictView() const { return conflictView_; }
    // 差分の入力を作り直したときに増える (DiffViewer へ渡し直す合図)
    uint64_t ViewSerial() const { return viewSerial_; }

private:
    struct ReadJob {
        std::wstring abs;
        bool oldPath = false; // 真なら nowOld_、偽なら now_ へ読む
        uint64_t offset = 0;
        bool done = false;
        // 8 MiB を超えるファイル全体の復元だけ: ハッシュを別のスレッドで計算して待つ (フレームを止めない)
        std::future<std::string> hashing;
        std::shared_ptr<std::atomic<bool>> cancel;
        uint64_t totalSize = 0;
    };

    NowState& TargetOf(const ReadJob& job) { return job.oldPath ? nowOld_ : now_; }
    void CancelHashes();

    void Finish();
    void StartReads(RestoreFileIo& io);
    void AdvanceRead(ReadJob& job, RestoreFileIo& io);
    void PollHash(ReadJob& job);
    void FailWith(const std::string& code, const std::string& detail);

    Phase phase_ = Phase::Closed;
    RestoreScope scope_ = RestoreScope::File;
    RestoreRevision revision_;
    std::vector<TrackHunk> hunks_;
    std::vector<TrackHunk> allHunks_;
    std::vector<std::string> targetPaths_;
    std::wstring projectRoot_;
    NowState now_;
    NowState nowOld_;
    PathIssue pathIssue_ = PathIssue::None;
    bool connected_ = false;
    bool hasPlan_ = false;
    bool blobMissing_ = false;
    bool blobPending_ = false;
    bool readsStarted_ = false; // blob_get の応答が、今のファイルの読み取りを始める前に届いても、まだ完了にしない
    RestorePlan plan_;
    std::vector<ReadJob> jobs_;
    std::string errorCode_;
    std::string errorDetail_;
    std::vector<DiffHunkInput> changeView_;
    std::vector<DiffHunkInput> conflictView_;
    uint64_t viewSerial_ = 0;
    // 閉じた・開き直したあとに届いた古い応答を捨てる
    uint64_t generation_ = 0;
};

} // namespace mye
