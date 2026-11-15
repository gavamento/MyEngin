//====================================================================================
//                          RestoreRunner.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元の実行 (ジャーナル → 適用 → 検証 → 巻き戻し → 起動時の回復。宣言)
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/RestoreFileIo.h"
#include "Editor/ChangeTracking/RestoreGate.h"
#include "Editor/ChangeTracking/RestoreJournal.h"
#include "Editor/ChangeTracking/RestorePlan.h"
#include "Editor/ChangeTracking/RestorePreview.h"
#include "Editor/ChangeTracking/RestoreWriteIo.h"
#include "Engine/Engine/HotReload/ReloadHub.h"

namespace mye {

class TrackClient;

// 復元の実行。ChangeTrackingSession が持ち、毎フレームの Poll で進める (AI Changes 窓を閉じても止まらない)。
// 書くのは、プレビューの計画 (RestorePlan.ops) にあるものだけ。**ファイルを書く口は RestoreWriteIo の 1 か所**。
// 正本: docs\ai-change-tracking\restore-safety.md §4.1、§5、§6

// BeginBatch 〜 EndBatch を呼ぶ口 (EditorApp が ReloadHub へ繋ぐ)。BeginBatch を呼んだら、どの経路でも EndBatch を 1 回呼ぶ
struct RestoreRunHooks {
    std::function<void()> beginBatch;
    std::function<void(const std::vector<BatchChange>&)> endBatch;
};

enum class RestoreOutcome : uint8_t {
    None,             // まだ終わっていない
    Committed,        // 全部書いて、読み直して確かめた
    Blocked,          // 書く前に止めた。利用者のファイルは 1 バイトも書いていない
    Cancelled,        // 書き始める前に利用者が取り消した
    RolledBack,       // 途中で失敗して、適用前の内容へ戻した
    RecoveryRequired, // 戻しきれないファイルが残った (回復の画面へ)
};

enum class RestoreStopReason : uint8_t {
    None,
    Gate,            // 実行の直前のゲートが止めた (blocks)
    PlanChanged,     // 計画が、プレビューのときと違う (内容・パスが変わった)
    PreviewFailed,   // 判定し直しの準備に失敗した
    FetchFailed,     // Before の内容を取れなかった (切断・時間切れ・欠け)
    BlobMismatch,    // 取った内容のハッシュが記録と違う
    JournalFailed,   // ジャーナルを書けなかった
    ContentChanged,  // 保存するとき・書く直前に、対象の今の内容が計画と違った
    PathChanged,     // 書く直前のパスの検査で拒否された
    WriteFailed,     // 書く・消す・名前を変えるのに失敗した
    VerifyFailed,    // 書いたあとの読み直しが合わない
    Exception,       // 例外
    UserCancelled,
    NotRecoverable,  // 回復の対象が無い
};

struct RestoreRunResult {
    RestoreOutcome outcome = RestoreOutcome::None;
    RestoreStopReason reason = RestoreStopReason::None;
    std::string restoreId;
    std::string detail;                 // 失敗したパスなど
    std::vector<RestoreBlock> blocks;   // reason == Gate のとき
    std::vector<std::string> unresolved; // RecoveryRequired: 触らずに残したパス
    int totalOps = 0;
    int completedOps = 0;
    bool fromRecovery = false;          // 起動時の回復の画面から始めた巻き戻し
    bool recordPending = false;         // restore_record をサービスへ送れていない (切断など。つながったら送り直す)
};

struct RestoreProgress {
    enum class Phase : uint8_t { Idle, Rechecking, Fetching, Saving, Applying, Verifying, RollingBack, Done };
    Phase phase = Phase::Idle;
    int step = 0;
    int total = 0;
    std::string path;
    bool canCancel = false; // 書き始める前だけ
};

// 回復の画面の 1 ファイル: 適用前・書こうとした内容・今の内容
struct RecoveryFileView {
    enum class Current : uint8_t { Pre, Intended, Other, Unreadable };
    std::string path;
    bool existed = false;
    std::string preHash;
    std::string intendedHash; // "" = 消す
    std::string currentHash;  // "" = 無い
    Current current = Current::Other;
};

class RestoreRunner {
public:
    struct StartRequest {
        RestoreScope scope = RestoreScope::File;
        nlohmann::json detail;                   // describe の結果 (判定し直しに使う)
        std::vector<TrackHunk> allHunks;
        std::vector<int64_t> selectedHunkIds;
        std::vector<RestoreOp> previewOps;       // プレビューの計画の ops。判定し直した計画がこれと同じでなければ書かない
        std::vector<std::string> targetPaths;
        // 今のゲートの判定 (止める理由の全部。空 = 通る)。引数 = 対象の Blob が欠けているか。実行の直前に呼ぶ
        std::function<std::vector<RestoreBlock>(bool blobMissing)> blocks;
    };

    RestoreRunner() = default;
    ~RestoreRunner();
    RestoreRunner(const RestoreRunner&) = delete;
    RestoreRunner& operator=(const RestoreRunner&) = delete;

    void Configure(const std::wstring& projectRoot, TrackClient* client);
    void SetHooks(RestoreRunHooks hooks) { hooks_ = std::move(hooks); }
    // 試験用: 読み取り・書き込みの口と、ID・時計を差し替える
    void SetIo(std::unique_ptr<RestoreFileIo> read, std::unique_ptr<RestoreWriteIo> write);
    void SetClock(std::function<int64_t()> nowMs) { nowMs_ = std::move(nowMs); }

    // 起動時 (対話起動のときだけ呼ぶ): prepared を捨て、古い committed を掃除し、回復待ちを集める。何も書き戻さない
    void ScanRecovery();

    // Editor の終了時 (ReloadHub がまだ生きているうちに) に 1 回。書き込みの途中なら BeginBatch の対になる EndBatch を通し、以後は何もしない。
    // ジャーナルは applying のまま残るので、次の起動で回復の画面に出る (途中で終了した復元と同じ扱い)
    void Shutdown();

    // 実行を始める。実行中・結果の確認待ち以外で、計画が Ready のときだけ true
    bool Start(StartRequest request);
    // 毎フレーム 1 回
    void Poll();
    // 書き始める前だけ効く
    void Cancel();

    bool Busy() const { return phase_ != RestoreProgress::Phase::Idle && phase_ != RestoreProgress::Phase::Done; }
    // BeginBatch 〜 EndBatch の間。ゲートの OpInFlight に入れて、ほかの書き込み (git・別の復元) を止める
    bool WriteSectionActive() const { return writeSection_; }
    bool RecoveryPending() const { return !recoveries_.empty(); }
    // 進行中、または回復待ちの復元がある (新しい復元を始められない)
    bool Pending() const { return Busy() || RecoveryPending(); }
    RestoreProgress Progress() const;
    bool HasResult() const { return phase_ == RestoreProgress::Phase::Done; }
    const RestoreRunResult& Result() const { return result_; }
    // 結果を見終えた (Idle へ戻す)
    void AcknowledgeResult();

    // ---- 回復 (起動時の検出と、利用者の選択) ----
    const std::vector<RestoreJournalStore::Entry>& Recoveries() const { return recoveries_; }
    // 3 つの内容 (適用前・書こうとした内容・今) を調べる。ファイルを読む (大きいものはハッシュだけ)。画面を開いたとき 1 回
    std::vector<RecoveryFileView> InspectRecovery(const std::string& id) const;
    // 適用前の内容へ戻す。今の内容が「自分が書いた内容」のファイルだけ戻し、違うものは触らない。blocks は実行の直前のゲート
    bool StartRecoveryRollback(const std::string& id, std::function<std::vector<RestoreBlock>(bool)> blocks);
    // 今のままにする (ファイルは触らない)。ジャーナルは残し、回復待ちから外す
    bool KeepCurrent(const std::string& id);
    // manifest が壊れたジャーナルを回復待ちから外す (中身は消さない)
    bool Dismiss(const std::string& id);

    // ---- 履歴 (サービスの restore_operations) ----
    void RequestHistory();
    const std::vector<nlohmann::json>& History() const { return history_; }
    bool HistoryPending() const { return historyPending_; }
    size_t PendingRecordCount() const { return records_.size(); }

    // ---- 復元の記録 (ライブのシーンの復元など、ジャーナルを作らない復元が使う) ----
    // restore_record の引数を送り先の順番待ちに積む (切断・時間切れなら、つながり直したら送り直す)
    void QueueRecord(nlohmann::json args);
    // 記録がサービスに受け取られたとき (画面の取り直しなど。ファイル・Hunk の復元の記録でも呼ぶ)
    void SetOnRecorded(std::function<void()> callback) { onRecorded_ = std::move(callback); }

private:
    struct Touched {
        std::string rel;
        std::wstring abs;
        bool existed = false;
        bool large = false;
        std::string hash; // 適用前
        uint64_t size = 0;
    };
    struct FetchState {
        size_t op = 0;
        uint64_t offset = 0;
        bool inFlight = false;
        bool large = false;
        bool hashing = false;
        std::future<std::string> hashJob;
    };
    struct Job {
        std::future<bool> done;
        std::future<std::string> hash;
        bool Valid() const { return done.valid() || hash.valid(); }
    };

    void StopBeforeWrite(RestoreOutcome outcome, RestoreStopReason reason, const std::string& detail);
    void Finish(RestoreOutcome outcome);
    void CloseWriteSection(const std::vector<BatchChange>& changes);
    std::vector<BatchChange> ChangesFor(bool committed) const;

    void TickRecheck();
    void TickFetch();
    void TickSave();
    void BeginApply();
    void TickApply();
    void TickVerify();
    void TickRollback();
    void BeginRollback(RestoreStopReason reason, const std::string& detail);

    enum class HashStatus : uint8_t { Done, Pending, Error };

    bool Gate(std::vector<RestoreBlock>& blocksOut, bool blobMissing);
    bool SaveManifest();
    std::wstring AbsOf(const std::string& rel) const;
    // 今の内容のハッシュ ("" = ファイルが無い)。大きいファイルは job の別のスレッドで計算し、終わるまで Pending
    HashStatus HashNow(const std::string& rel, Job& job, std::string& out);
    // 書き先の今の内容が expectedHash ("" = 無い) か (小さいファイルだけ全体を読む)
    bool PathStateIs(const std::string& rel, const std::string& expectedHash, bool large);

    void SendRecord(const JournalManifest& manifest, const char* state, const std::string& error);
    void SendExpect();
    void PumpRecords();
    void RefreshRecoveries();
    RestoreWriteIo& Write();
    RestoreFileIo& Read();
    RestoreJournalStore Store();
    int64_t NowMs() const { return nowMs_ ? nowMs_() : WallClockMs(); }

    std::wstring root_;
    TrackClient* client_ = nullptr;
    RestoreRunHooks hooks_;
    std::unique_ptr<RestoreFileIo> readIo_;
    std::unique_ptr<RestoreWriteIo> writeIo_;
    std::function<int64_t()> nowMs_;

    RestoreProgress::Phase phase_ = RestoreProgress::Phase::Idle;
    bool writeSection_ = false;
    bool recovery_ = false; // 起動時の回復の巻き戻し (計画の実行ではない)
    RestoreRunResult result_;
    StartRequest request_;
    RestorePreview recheck_;
    std::vector<RestoreOp> ops_;
    std::vector<Touched> touched_;
    JournalManifest manifest_;
    std::vector<std::string> fetched_;   // 操作ごとの取った内容 (8 MiB 以下のもの)
    std::vector<bool> staged_;           // 大きい内容をジャーナルの new\ へ積んだ操作
    std::vector<size_t> fetchOps_;       // BeforeBlob の操作の添字
    size_t fetchPos_ = 0;
    FetchState fetch_;
    nlohmann::json lastResponse_;
    bool hasResponse_ = false;
    uint64_t generation_ = 0;
    size_t savePos_ = 0;
    int saveStage_ = 0;
    Job saveJob_;
    std::vector<std::string> savedHashes_; // 保存したときに読んだ適用前のハッシュ
    bool applyStarted_ = false;
    size_t opPos_ = 0;
    size_t verifyPos_ = 0;
    Job verifyJob_;
    size_t rollbackPos_ = 0;
    Job rollbackJob_;
    int rollbackStage_ = 0; // 0 = 今の内容を調べる、1 = 戻している (大きいファイルの複写を待つ)
    std::vector<std::string> unresolved_;
    bool blobMissing_ = false;

    std::vector<RestoreJournalStore::Entry> recoveries_;
    std::vector<nlohmann::json> records_; // まだ送れていない restore_record の引数
    std::function<void()> onRecorded_;
    bool recordInFlight_ = false;
    int64_t recordRetryAtMs_ = 0;
    std::vector<nlohmann::json> history_;
    bool historyPending_ = false;
};

} // namespace mye
