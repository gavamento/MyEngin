//====================================================================================
//                          ClientSimRunner.cpp
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          クライアントの予測・ロールバック・再同期の駆動の実装
//====================================================================================
#include "Engine/Engine/Net/ClientSimRunner.h"

#include <algorithm>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Replay/CrashRing.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr double kTickMs = 1000.0 / 60.0;

} // namespace

void ClientSimRunner::Attach(ClientSession* session, const SimRefs& refs, const ClientSimHooks& hooks,
                             const ClientSimRunnerConfig& cfg)
{
    session_ = session;
    refs_ = refs;
    hooks_ = hooks;
    cfg_ = cfg;
    stats_ = ClientSimRunnerStats{};
    begun_ = failed_ = halted_ = stalledNow_ = false;
    acc_ = 0.0;
    lastUpdateMs_ = 0;
    desyncScan_ = 0;
    lastBundleDir_.clear();
    rb_.Clear();
}

// 他レーンは確定が無ければ予測、自レーンは確定が無ければ自分で決めた値 (サーバが代替入力にしていたら
// 確定が届いたときの食い違いで巻き戻る)。戻り値 = 予測を含むか
bool ClientSimRunner::BuildInputs(uint64_t tick, InputSnapshot* lanes, SystemInputTick& sys)
{
    const uint32_t pc = (std::min)(session_->PlayerCount(), kMaxPlayers);
    if (const NetConfirmedTick* c = session_->Confirmed(tick)) {
        for (uint32_t p = 0; p < pc; ++p) {
            lanes[p] = c->inputs[p];
        }
        sys = c->sys;
        return false;
    }
    for (uint32_t p = 0; p < pc; ++p) {
        if (p == session_->Lane()) {
            InputSnapshot in = {};
            session_->LocalInput(tick, in); // まだ無ければゼロ入力 (参加直後の inputDelay 分)
            lanes[p] = in;
        } else {
            lanes[p] = session_->PredictLane(tick, p);
        }
    }
    sys = SystemInputTick{}; // システム入力の予測 = イベント無し
    return true;
}

void ClientSimRunner::ApplySnapshot(uint64_t nowMs)
{
    const std::vector<std::byte>& blob = session_->SnapshotBlob();
    const SnapshotMeta& meta = session_->SnapshotMetaOf();
    bool match = RestoreSimSnapshot(refs_, blob.data(), blob.size());
    // 復元した tick が meta と同じで、ワールドハッシュも一致して初めて参加を成立させる
    match = match && TickIndex() == meta.tick && hooks_.worldHash() == meta.worldHash;
    if (match && hooks_.onSnapshotApplied) {
        hooks_.onSnapshotApplied(meta, blob); // OnSnapshotApplied(true) が blob を手放す前
    }
    session_->OnSnapshotApplied(match, nowMs);
    if (!match) {
        return;
    }
    ++stats_.snapshotsApplied;
    begun_ = rb_.Begin(refs_, meta.tick, cfg_.maxSpeculation);
    if (!begun_) {
        MYE_LOG_ERROR("[client] could not start the rollback ring at tick %llu",
                      static_cast<unsigned long long>(meta.tick));
        failed_ = true;
        return;
    }
    if (cfg_.crashRing != nullptr) {
        cfg_.crashRing->Begin(refs_, meta.tick);
    }
    desyncScan_ = (meta.tick + kNetHashCheckpoint - 1) / kNetHashCheckpoint * kNetHashCheckpoint;
    acc_ = 0.0;
    lastUpdateMs_ = nowMs;
    stalledNow_ = false;
}

bool ClientSimRunner::RunOne(uint64_t nowMs)
{
    const uint64_t t = TickIndex();
    const uint32_t pc = (std::min)(session_->PlayerCount(), kMaxPlayers);
    // ★自レーンの未来入力 (t + inputDelay) は tick ごとにちょうど 1 回確定させて送る
    const uint64_t target = t + session_->InputDelay();
    InputSnapshot stored = {};
    if (!session_->LocalInput(target, stored)) {
        session_->SubmitLocalInput(target, hooks_.liveInput(target), nowMs);
    }
    InputSnapshot lanes[kMaxPlayers] = {};
    SystemInputTick sys = {};
    const bool predicted = BuildInputs(t, lanes, sys);
    if (cfg_.crashRing != nullptr) {
        cfg_.crashRing->OnTickBegin(t, lanes, pc, &sys);
    }
    const uint64_t hash = hooks_.runTick(lanes, pc, sys, false);
    if (cfg_.crashRing != nullptr) {
        cfg_.crashRing->OnTickEnd(refs_, t, hash);
    }
    rb_.OnTickEnd(refs_, t, lanes, pc, hash, predicted, true, &sys);
    if (!rb_.Active()) {
        MYE_LOG_ERROR("[client] rollback ring failed at tick %llu", static_cast<unsigned long long>(t));
        failed_ = true;
        return false;
    }
    return true;
}

// from の直前まで巻き戻し、今の tick まで確定入力 (無ければ予測) で走り直す
bool ClientSimRunner::ResimFrom(uint64_t from)
{
    const uint64_t resume = TickIndex();
    if (from >= resume) {
        return true;
    }
    const uint32_t pc = (std::min)(session_->PlayerCount(), kMaxPlayers);
    const std::vector<std::byte>* blob = rb_.SnapshotBefore(from);
    if (blob == nullptr || !RestoreSimSnapshot(refs_, blob->data(), blob->size())) {
        MYE_LOG_ERROR("[client] rollback: no restorable snapshot for tick %llu",
                      static_cast<unsigned long long>(from));
        return false;
    }
    if (cfg_.crashRing != nullptr) {
        cfg_.crashRing->Rewind(from);
    }
    // 区間にシステムイベントがあれば記録する: 再シムが tick ごとの確定イベントで差し替わって走ることの実走の証跡
    uint32_t eventTicks = 0;
    uint32_t eventCount = 0;
    for (uint64_t t = from; t < resume; ++t) {
        if (const NetConfirmedTick* c = session_->Confirmed(t)) {
            eventTicks += c->sys.eventCount > 0 ? 1u : 0u;
            eventCount += c->sys.eventCount;
        }
    }
    if (eventCount > 0) {
        ++stats_.resimsAcrossEvents;
        MYE_LOG_INFO("[client] rollback to tick %llu re-simulates %llu tick(s) across %u system event(s) in %u tick(s) "
                     "(join / leave / rejoin applied from the confirmed record)",
                     static_cast<unsigned long long>(from), static_cast<unsigned long long>(resume - from), eventCount,
                     eventTicks);
    }
    for (uint64_t t = from; t < resume; ++t) {
        InputSnapshot lanes[kMaxPlayers] = {};
        SystemInputTick sys = {};
        const bool predicted = BuildInputs(t, lanes, sys);
        if (cfg_.crashRing != nullptr) {
            cfg_.crashRing->OnTickBegin(t, lanes, pc, &sys);
        }
        const uint64_t hash = hooks_.runTick(lanes, pc, sys, true);
        if (cfg_.crashRing != nullptr) {
            cfg_.crashRing->OnTickEnd(refs_, t, hash);
        }
        rb_.OnTickEnd(refs_, t, lanes, pc, hash, predicted, true, &sys);
        ++stats_.ticksResimulated;
    }
    return rb_.Active();
}

void ClientSimRunner::CommitConfirmed()
{
    uint64_t confirmed = rb_.ConfirmedTick();
    const uint64_t now = TickIndex();
    while (confirmed < now && session_->HasConfirmed(confirmed)) {
        const NetSpecTick* e = rb_.Entry(confirmed);
        if (e == nullptr || e->predicted) {
            break; // まだ覆りうる (次の Reconcile で片付く)
        }
        const NetConfirmedTick* c = session_->Confirmed(confirmed);
        rb_.NoteCommitted(confirmed, e->hashAfter);
        // ★リングの保護下限を進めるのは確定した tick まで (投機 tick で進めると、後から届いた本物が捨てられる)
        session_->OnTickConsumed(confirmed);
        if (hooks_.onCommitted) {
            hooks_.onCommitted(confirmed, *c, e->hashAfter);
        }
        if (confirmed % kNetHashCheckpoint == 0) {
            session_->SetLocalConfirmed(confirmed, e->hashAfter);
        }
        ++confirmed;
    }
    rb_.SetConfirmedTick(confirmed);
}

// 届いた確定 tick と投機記録を突き合わせ、外れていたら巻き戻す。確定した tick を commit する
void ClientSimRunner::Reconcile()
{
    if (!rb_.Active()) {
        return;
    }
    const uint32_t pc = (std::min)(session_->PlayerCount(), kMaxPlayers);
    const uint64_t now = TickIndex();
    uint64_t bad = ~0ull;
    for (uint64_t t = rb_.ConfirmedTick(); t < now; ++t) {
        if (!session_->HasConfirmed(t)) {
            break; // まだ確定していない = ここから先は判定できない
        }
        const NetSpecTick* e = rb_.Entry(t);
        if (e == nullptr) {
            break;
        }
        if (!e->predicted) {
            continue;
        }
        const NetConfirmedTick* c = session_->Confirmed(t);
        if (rb_.InputsMatch(t, c->inputs, pc) && rb_.SystemMatch(t, c->sys)) {
            rb_.MarkConfirmed(t); // 予測が当たった
            continue;
        }
        bad = t;
        break;
    }
    if (bad != ~0ull) {
        const uint64_t depth = now - bad;
        if (!ResimFrom(bad)) {
            failed_ = true;
            return;
        }
        rb_.NoteRollback(depth);
    }
    CommitConfirmed();
}

void ClientSimRunner::CheckDesync(uint64_t nowMs)
{
    if (!rb_.Active() || halted_) {
        return;
    }
    // 確定済みの checkpoint を古い順に突き合わせる (サーバの主張が落ちたものは追い越されたら諦める)
    uint64_t serverHash = 0;
    uint64_t mine = 0;
    while (desyncScan_ < rb_.ConfirmedTick()) {
        if (!session_->ServerCheckpointHash(desyncScan_, serverHash)) {
            if (session_->ServerFrontier() > desyncScan_ + kNetHashCheckpoint * 4) {
                desyncScan_ += kNetHashCheckpoint;
                continue;
            }
            return; // まだ届いていないだけ
        }
        if (rb_.CommittedHash(desyncScan_, mine) && mine != serverHash) {
            break;
        }
        desyncScan_ += kNetHashCheckpoint;
    }
    if (desyncScan_ >= rb_.ConfirmedTick()) {
        return;
    }
    const uint64_t tick = desyncScan_;
    ++stats_.desyncs;
    MYE_LOG_ERROR("[client] DESYNC at tick %llu: local %016llX / server %016llX",
                  static_cast<unsigned long long>(tick), static_cast<unsigned long long>(mine),
                  static_cast<unsigned long long>(serverHash));
    if (cfg_.crashRing != nullptr && !cfg_.crashRoot.empty()) {
        NetDesyncReport rep;
        rep.tick = tick;
        rep.nowTick = TickIndex();
        rep.localHash = mine;
        rep.peerHash = serverHash;
        rep.localPlayer = session_->Lane();
        rep.role = static_cast<int>(NetRole::Client);
        HashDump dump;
        HashWorldDump(refs_.scene->GetWorld(), refs_.HashSources(), TickIndex(), dump);
        if (WriteNetDesyncBundle(cfg_.crashRoot, rep, *cfg_.crashRing, dump, lastBundleDir_)) {
            MYE_LOG_ERROR("[client]   bundle: %s", WideToUtf8(lastBundleDir_).c_str());
        }
    }
    if (cfg_.haltOnDesync) {
        halted_ = true; // desync 後の世界は別物。止めて報告する
        return;
    }
    session_->RequestResync(NetResyncReason::Desync, nowMs);
}

void ClientSimRunner::Update(uint64_t nowMs)
{
    if (session_ == nullptr || failed_ || halted_) {
        return;
    }
    session_->Poll(nowMs);
    if (session_->State() == ClientState::SnapshotReady) {
        ApplySnapshot(nowMs);
    }
    if (!session_->Running() || !begun_) {
        acc_ = 0.0;
        lastUpdateMs_ = nowMs;
        return;
    }
    const double dt = static_cast<double>(nowMs - lastUpdateMs_);
    lastUpdateMs_ = nowMs;

    Reconcile();
    CheckDesync(nowMs);
    if (failed_ || halted_ || !session_->Running()) {
        return;
    }

    acc_ += dt * session_->SpeedFactor();
    uint32_t ran = 0;
    while (ran < cfg_.maxTicksPerUpdate) {
        const uint64_t t = TickIndex();
        const bool gateOpen = t < rb_.ConfirmedTick() || t - rb_.ConfirmedTick() < rb_.MaxSpeculation();
        if (gateOpen) {
            stalledNow_ = false;
        }
        const bool byAccumulator = acc_ >= kTickMs;
        const bool byCatchUp = !byAccumulator && session_->CatchUpPending() > 0;
        if (!byAccumulator && !byCatchUp) {
            break;
        }
        // 予測上限: 確定済みの先頭から maxSpeculation 本までしか先へ行かない (超えたら止まる)
        if (!gateOpen) {
            if (!stalledNow_) {
                stalledNow_ = true;
                ++stats_.stalls;
            }
            stats_.stallMs += dt;
            acc_ = (std::min)(acc_, kTickMs); // 止まっていた間の時間を溜めない (復帰で早送りしない)
            break;
        }
        if (!RunOne(nowMs)) {
            return;
        }
        if (byAccumulator) {
            acc_ -= kTickMs;
            ++stats_.ticksRun;
        } else {
            ++stats_.catchUpTicks;
            session_->OnCatchUpTickRan(nowMs);
        }
        ++ran;
    }
    // 回した tick のぶんだけ確定が進んだかもしれない (予測が無かった tick は即確定)
    CommitConfirmed();
}

} // namespace mye
