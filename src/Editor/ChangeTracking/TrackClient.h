//====================================================================================
//                          TrackClient.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          追跡サービスの pipe クライアント (スレッドなし・待たない)
//====================================================================================
#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/TrackIo.h"

namespace mye {

// 追跡サービスとの接続の状態。Disabled / NoProject / NoService / ProtoMismatch / WrongProject は
// 自分では抜けない (Start し直すまで I/O をしない)。DbDegraded と Disconnected は再接続を試す
enum class TrackState {
    Disabled,      // 設定で無効 / 非対話起動
    NoProject,     // プロジェクトを開いていない
    Connecting,    // 接続中 (hello の応答待ちを含む)
    Ready,
    NoService,     // mye_track.exe が見つからない
    ProtoMismatch, // プロトコルの版が違う
    WrongProject,  // 別のプロジェクトのサービスに繋がった
    DbDegraded,    // サービスが DB へ書けない
    Disconnected,  // 切れた。再接続待ち
};

const char* TrackStateName(TrackState state);

// 要求の時間切れ。読み取りの op は 30 秒、記録・その他は 5 秒 (ipc-contract.md §6)。AI の編集を待たせないための値
constexpr int64_t kTrackReadTimeoutMs = 30000;
constexpr int64_t kTrackRecordTimeoutMs = 5000;
int64_t TrackOpTimeoutMs(const std::string& op);

// 切断・失敗のあと、次に接続を試すまでの間隔
constexpr int64_t kTrackReconnectIntervalMs = 3000;
// サービスを起動したあと、pipe が現れるのを見に行く間隔と、その猶予
constexpr int64_t kTrackAfterLaunchRetryMs = 500;
constexpr int64_t kTrackLaunchGraceMs = 10000;
// 空きインスタンスが無い (ERROR_PIPE_BUSY) ときに、待たずに次のフレームで開き直す回数
constexpr int kTrackBusyRetryLimit = 5;

// 1 回の Poll() が処理する量の上限。残りは次のフレーム
constexpr int kTrackMaxLinesPerPoll = 256;
constexpr int kTrackMaxPumpsPerPoll = 4;
constexpr std::chrono::microseconds kTrackPollBudget{ 2000 };
// 溜まった未処理の入力がこれを超えたら、pipe から読むのを止める (サービス側が書き止まる)
constexpr size_t kTrackRxHighWater = 4 * 1024 * 1024;
// 1 回の Poll() が TrackIo を呼ぶ回数の上限 (時計を除く)
constexpr int kTrackMaxIoCallsPerPoll = 8;

// 名前付き pipe のクライアント。**スレッドを作らず、Poll() は待たない**。
// 応答は id で対にする (順不同)。id の無い行は通知としてハンドラへ配る。
// 正本: docs\ai-change-tracking\ipc-contract.md
class TrackClient {
public:
    using ResponseFn = std::function<void(const nlohmann::json&)>; // 応答 1 件。失敗は合成した {"ok":false,"error":{...}}
    using NotifyFn = std::function<void(const nlohmann::json&)>;
    using StateFn = std::function<void(TrackState)>;

    struct Stats {
        uint64_t lines = 0;          // 解釈した行の累計
        uint32_t lastPollLines = 0;  // 直近の Poll() で解釈した行
        uint64_t badLines = 0;       // JSON でない・形が合わない行
        uint64_t lateResponses = 0;  // 時間切れのあとに届いた応答
        uint64_t timeouts = 0;
        uint32_t connects = 0;       // hello が通った回数
    };

    TrackClient() = default;
    ~TrackClient();
    TrackClient(const TrackClient&) = delete;
    TrackClient& operator=(const TrackClient&) = delete;

    // 接続を始める。以後 Poll() が接続・再接続を進める。io の呼び出しは Poll() からだけ
    void Start(std::unique_ptr<TrackIo> io, const std::wstring& projectRoot, uint32_t pid);
    // I/O をしない待機状態 (Disabled / NoProject) にする
    void SetIdle(TrackState state);
    // 接続を閉じる (サービスは止めない)。保留中の要求は失敗で返す
    void Stop();

    // 毎フレーム 1 回。待たない
    void Poll();

    // 要求を投げる。onDone は必ず 1 回、Poll() か Request() の中から同じスレッドで呼ばれる (応答・失敗・時間切れ)。
    // 戻り値は採番した id (0 = 送れなかった。そのとき onDone は呼ばれ済み)
    uint64_t Request(const char* op, const nlohmann::json& args, ResponseFn onDone);

    // 1 行を解釈して配る。Poll() が使う本体で、試験が行を流し込むために公開している (CollabClient の前例)
    void DispatchLine(const std::string& line);

    // status の結果 (db.state) を受けて、DbDegraded と Ready を行き来する。接続は保つ
    void SetDbDegraded(bool degraded);

    void SetNotifyHandler(NotifyFn fn) { onNotify_ = std::move(fn); }
    // 状態が変わるたびに呼ばれる (Ready になったとき、モデルが取り直しを始める)
    void SetStateHandler(StateFn fn) { onState_ = std::move(fn); }

    TrackState State() const { return state_; }
    bool Ready() const { return state_ == TrackState::Ready; }
    // 要求を送れるか (hello が通っている。DbDegraded を含む)
    bool CanRequest() const { return helloOk_; }
    size_t PendingCount() const { return pending_.size(); }
    // 直近の切断・失敗の理由 (ログと画面の補足用)
    const std::string& Reason() const { return reason_; }
    const Stats& GetStats() const { return stats_; }
    int64_t NowMs() const { return io_ ? io_->NowMs() : 0; }

    // 合成した失敗の応答
    static nlohmann::json MakeFailure(const char* code, const std::string& detail);

private:
    struct Pending {
        ResponseFn fn;
        int64_t deadlineMs = 0;
        std::string op;
    };

    void TryConnect(int64_t now);
    void OnHello(const nlohmann::json& response);
    // 接続を閉じて保留中の要求を失敗で返す (状態は変えない)
    void CloseConnection(const char* failCode);
    // 切れた: 再接続を待つ
    void Disconnect(const std::string& reason);
    // 再接続しない状態で閉じる
    void FinalClose(TrackState state, const std::string& reason);
    void SetState(TrackState state);
    void ExpireTimedOut(int64_t now);
    bool HasCompleteLine() const;

    std::unique_ptr<TrackIo> io_;
    std::wstring projectRoot_;
    std::string pipeName_;
    std::string projectId_;
    uint32_t pid_ = 0;

    TrackState state_ = TrackState::Disabled;
    std::string reason_;
    bool connected_ = false;  // pipe が開いている
    bool helloOk_ = false;    // hello が通った
    bool peerClosed_ = false; // 相手が閉じた (溜まった行を配り終えてから切る)
    bool everReady_ = false;  // 接続が一度でも Ready になった (切れたとき Disconnected にする)
    bool launchRequested_ = false;
    bool badTokenRetried_ = false;
    int busyCount_ = 0;
    int64_t nextAttemptMs_ = 0;
    int64_t launchedAtMs_ = 0;

    uint64_t nextId_ = 1;
    uint64_t helloId_ = 0;
    // map: 走査順が id 昇順 (送信順) になり、同時に期限切れになったときのコールバック順が決まる
    std::map<uint64_t, Pending> pending_;

    std::string rx_;
    size_t rxPos_ = 0;
    Stats stats_;
    NotifyFn onNotify_;
    StateFn onState_;
};

} // namespace mye
