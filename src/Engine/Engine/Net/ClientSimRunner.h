//====================================================================================
//                          ClientSimRunner.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          クライアントの予測・ロールバック・再同期の駆動 (sim はフックで注入)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "Engine/Engine/Net/ClientSession.h"
#include "Engine/Engine/Net/NetRollback.h"
#include "Engine/Engine/Replay/SimSnapshot.h"

namespace mye {

class CrashRing;

// クライアントの sim を駆動する (M81d)。ClientSession が受け取った確定 tick を使い、
//   確定 tick があればそれで、無ければ予測 (他レーン = 確定済みの最新値、システム入力 = イベント無し) で
//   先行して回し、確定 tick が予測と食い違ったら直前のスナップショットへ戻して再シムする。
// P2P の予測ロールバック (EngineLoop の NetReconcile / NetResimFrom / NetCommitConfirmed) と同じ方式で、
// 違いは「確定の出どころがサーバ 1 本」「システム入力も tick ごとに予測 / 再シムする」「予測上限が 12」。
//
// sim はフックで注入する (EngineLoop は RunOneTick、selftest は HeadlessSim)。
// ★ここも sim 状態へ書くのは「確定入力 + 予測入力を runTick に渡す」ことと「スナップショットの復元」だけ。
//   時計は読まない (nowMs は呼び出し側が渡す)。

struct ClientSimHooks {
    // lanes (playerCount 本) と sys で 1 tick 回し、tick 末のワールドハッシュを返す。resim = 再シム
    std::function<uint64_t(const InputSnapshot* lanes, uint32_t playerCount,
                           const SystemInputTick& sys, bool resim)> runTick;
    // いまの sim のワールドハッシュ (スナップショット復元直後の照合)
    std::function<uint64_t()> worldHash;
    // 自レーンの入力源。tick ごとにちょうど 1 回だけ呼ばれる (tick = 未来の target)
    std::function<InputSnapshot(uint64_t tick)> liveInput;
    // 確定した tick を番号順に 1 回ずつ通知する (.rep の記録先)。予測で走った tick は来ない
    std::function<void(uint64_t tick, const NetConfirmedTick& confirmed, uint64_t hashAfter)> onCommitted;
    // 参加・再同期のスナップショットを復元し、ワールドハッシュの一致を確かめた直後 (ロールバックのリングを
    // 起こす前)。blob はこの呼び出しの間だけ有効。.rep の開始点 (開始スナップショット) を作る側が使う。null 可
    std::function<void(const SnapshotMeta& meta, const std::vector<std::byte>& blob)> onSnapshotApplied;
};

struct ClientSimRunnerConfig {
    uint32_t maxSpeculation = kNetMaxSpeculationClient; // D4: サーバ構成のクライアントは 12 tick
    uint32_t maxTicksPerUpdate = 16;                   // 1 回の Update で回す tick の上限
    // true: desync を検出したら止める (観察用)。既定は診断バンドルを出して再同期する
    bool haltOnDesync = false;
    // desync バンドルの出力先 (crashRing が null か空なら出さない)
    std::wstring crashRoot;
    CrashRing* crashRing = nullptr;
};

struct ClientSimRunnerStats {
    uint64_t ticksRun = 0;          // 通常 tick
    uint64_t ticksResimulated = 0;  // 再シムで回した tick
    uint64_t catchUpTicks = 0;      // 追いつきのために回した余分な tick
    uint64_t stalls = 0;            // 予測上限に達して止まった回数 (連続した停止は 1 回)
    double stallMs = 0.0;
    uint64_t desyncs = 0;
    uint64_t snapshotsApplied = 0;
    uint64_t resimsAcrossEvents = 0; // システムイベント (参加・離脱) を含む区間を再シムした回数
};

class ClientSimRunner {
public:
    // session / refs は呼び出し側が所有 (寿命は Runner より長いこと)
    void Attach(ClientSession* session, const SimRefs& refs, const ClientSimHooks& hooks,
                const ClientSimRunnerConfig& cfg);

    // 1 回の駆動 (描画フレームごと / selftest では 1ms ごと)。session.Poll も呼ぶ
    void Update(uint64_t nowMs);

    bool Failed() const { return failed_; }
    bool Halted() const { return halted_; }
    uint64_t TickIndex() const { return refs_.tickIndex != nullptr ? *refs_.tickIndex : 0; }
    uint64_t ConfirmedTick() const { return rb_.ConfirmedTick(); }
    const NetRollback& Rollback() const { return rb_; }
    const ClientSimRunnerStats& Stats() const { return stats_; }
    const std::wstring& LastBundleDir() const { return lastBundleDir_; }

private:
    bool BuildInputs(uint64_t tick, InputSnapshot* lanes, SystemInputTick& sys);
    void ApplySnapshot(uint64_t nowMs);
    bool RunOne(uint64_t nowMs);
    bool ResimFrom(uint64_t from);
    void Reconcile();
    void CommitConfirmed();
    void CheckDesync(uint64_t nowMs);

    ClientSession* session_ = nullptr;
    SimRefs refs_;
    ClientSimHooks hooks_;
    ClientSimRunnerConfig cfg_;
    NetRollback rb_;
    ClientSimRunnerStats stats_;
    bool begun_ = false;
    bool failed_ = false;
    bool halted_ = false;
    bool stalledNow_ = false;
    double acc_ = 0.0;
    uint64_t lastUpdateMs_ = 0;
    uint64_t desyncScan_ = 0;
    std::wstring lastBundleDir_;
};

} // namespace mye
