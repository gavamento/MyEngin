#include "Engine/Engine/Replay/TimeTravel.h"

#include <algorithm>

#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"

namespace mye {

uint64_t TimeTravel::HashOf(const SimRefs& refs)
{
    if (refs.scene == nullptr) {
        return 0;
    }
    // ★record/verify が撮っているのと**同じ 3 出口の同じ引数**で撮ること。
    //   ここだけ引数が欠けるとシークの自己検証が「割れていないのに割れた」と言い出す。
    // ★M70c で **refs.acoustic の渡し忘れを修正**した (M65a が TickRunner 側にだけ
    //   配線して、ここと EngineLoop の 4 か所が 4 引数のまま残っていた)。音響の節は
    //   内容ゲート = 波が 1 本も無ければ畳まないので、波の出ないシーンでは同じ値が
    //   出ていて誰も気づけなかった — 波のあるシーンでだけ「クラッシュ .rep が全 tick
    //   MISMATCH」「タイムトラベルの自己検証が波スロット表を見ない」形で出る
    return HashWorld(refs.scene->GetWorld(),
                     {refs.particles, &refs.scene->Time(), &refs.scene->Persist(), refs.xpbd, refs.acoustic,
                      &refs.scene->UI()});
}

void TimeTravel::SetEnabled(bool on)
{
    want_ = on;
    if (!on) {
        Clear();
    }
}

void TimeTravel::Clear()
{
    enabled_ = false;
    scrubbing_ = false;
    seekPending_ = false;
    switchPending_ = false;
    scrubbedSinceLastTick_ = false;
    seekTarget_ = 0;
    switchTarget_ = 0;
    firstTick_ = 0;
    startHash_ = 0;
    simSinceSnapshot_ = 0;
    bytes_ = 0;
    entries_.clear();
    snapshots_.clear();
    branches_.clear();
    nextBranchId_ = 1;
    branchSeq_ = 0;
    lastSeek_ = SeekReport{};
}

void TimeTravel::Begin(const SimRefs& refs, uint64_t tick)
{
    entries_.clear();
    snapshots_.clear();
    branches_.clear();
    nextBranchId_ = 1;
    branchSeq_ = 0;
    bytes_ = 0;
    firstTick_ = tick;
    simSinceSnapshot_ = 0;
    scrubbing_ = false;
    seekPending_ = false;
    switchPending_ = false;
    scrubbedSinceLastTick_ = false;
    lastSeek_ = SeekReport{};
    startHash_ = HashOf(refs);
    enabled_ = true;
    if (!TakeSnapshot(refs, tick, startHash_, false)) {
        // 1 枚目が撮れない = そもそも巻き戻せない。黙って空リングを持つより無効化する
        MYE_LOG_ERROR("[timetravel] could not capture the first snapshot - disabled");
        Clear();
        want_ = false;
        return;
    }
    MYE_LOG_INFO("[timetravel] ring started at tick %llu (interval %llu, max %zu snapshots)",
                 static_cast<unsigned long long>(tick),
                 static_cast<unsigned long long>(config_.snapshotInterval),
                 config_.maxSnapshots);
}

void TimeTravel::OnTickEnd(const SimRefs& refs, uint64_t ranTick, const InputSnapshot* inputs,
                           uint32_t playerCount, bool simulated, uint64_t hashAfter)
{
    if (!enabled_) {
        return;
    }
    if (ranTick < firstTick_ || ranTick > EndTick()) {
        // リングの範囲外で tick が走った = 追跡不能 (誰かが tickIndex を飛ばした)。
        // 嘘のタイムラインを見せるくらいなら止める
        MYE_LOG_WARN("[timetravel] tick %llu is outside the ring [%llu, %llu) - stopped",
                     static_cast<unsigned long long>(ranTick),
                     static_cast<unsigned long long>(firstTick_),
                     static_cast<unsigned long long>(EndTick()));
        Clear();
        want_ = false;
        return;
    }
    const size_t idx = static_cast<size_t>(ranTick - firstTick_);
    if (idx < entries_.size()) {
        // 縮退経路: Fork を通らずにリングの途中で tick が走った。記録済みの未来は
        // 捨てずに分岐へ移す (M52e までは resize で捨てていた)。走る前の状態は撮れて
        // いないので、編集があったとしてもここでは拾えない = 正規経路は Fork
        const uint32_t id = SplitSuffix(kLiveLane, ranTick);
        MYE_LOG_WARN("[timetravel] tick %llu ran without Fork - the recorded future moved to "
                     "branch B%u",
                     static_cast<unsigned long long>(ranTick), id);
        simSinceSnapshot_ = 0;
        EnforceBranchLimit(id);
    }
    TimeTravelEntry e;
    if (inputs != nullptr) {
        const uint32_t n = (playerCount < kMaxPlayers) ? playerCount : kMaxPlayers;
        for (uint32_t p = 0; p < n; ++p) {
            e.inputs[p] = inputs[p];
        }
    }
    e.simulated = simulated;
    e.hashAfter = hashAfter;
    entries_.push_back(e);
    scrubbedSinceLastTick_ = false;

    if (simulated) {
        ++simSinceSnapshot_;
    }
    if (simSinceSnapshot_ >= config_.snapshotInterval) {
        simSinceSnapshot_ = 0;
        TakeSnapshot(refs, ranTick + 1, hashAfter, false); // 「次の tick が走る前」の状態
    }
    CollapseIdentical(ranTick);
}

// ---------------------------------------------------------------- 分岐 (M72a)

bool TimeTravel::NeedsBoundaryCheck(uint64_t tick) const
{
    if (!enabled_) {
        return false;
    }
    if (tick < EndTick()) {
        return true; // 記録済みの未来がある = 走らせる前に分岐へ移す
    }
    if (scrubbedSinceLastTick_) {
        return true; // シーク直後 (末尾へ戻った場合を含む)。編集されているかもしれない
    }
    // 直前がポーズ tick なら、そのあいだに Inspector が世界を触っている可能性がある
    return !entries_.empty() && !entries_.back().simulated;
}

uint32_t TimeTravel::Fork(const SimRefs& refs, uint64_t tick)
{
    if (!enabled_ || tick < firstTick_ || tick > EndTick()) {
        return 0;
    }
    const uint64_t liveHash = HashOf(refs);
    const uint64_t recordedHash = HashAtTick(tick);
    const bool hasFuture = tick < EndTick();
    const bool edited = liveHash != recordedHash;
    if (!hasFuture && !edited) {
        return 0;
    }
    uint32_t id = 0;
    size_t movedTicks = 0;
    if (hasFuture) {
        movedTicks = entries_.size() - static_cast<size_t>(tick - firstTick_);
        id = SplitSuffix(kLiveLane, tick);
    } else {
        // 未来は無いが世界が編集された: この tick の古い (編集前の) スナップショットは
        // もう「この tick が走る前の状態」ではない。残すと後で戻ったときに編集が消える
        while (!snapshots_.empty() && snapshots_.back().tick >= tick) {
            bytes_ -= snapshots_.back().blob.size();
            snapshots_.pop_back();
        }
    }
    // ★編集後の状態を pinned で撮り直す。未来を移しただけのときも撮る —
    //   分岐のゴーストを焼いた後にライブへ戻る起点になる (M72d)
    if (!TakeSnapshot(refs, tick, liveHash, true)) {
        MYE_LOG_WARN("[timetravel] fork at tick %llu: could not re-capture the live state",
                     static_cast<unsigned long long>(tick));
    }
    simSinceSnapshot_ = 0;
    EnforceBranchLimit(id);
    MYE_LOG_INFO("[timetravel] fork at tick %llu -> %s%u (%zu ticks moved)%s",
                 static_cast<unsigned long long>(tick), id != 0 ? "branch B" : "no branch, id ", id,
                 movedTicks, edited ? " - the live state was edited, re-captured" : "");
    return id;
}

void TimeTravel::RequestSwitch(uint32_t branchId)
{
    if (!enabled_ || FindBranch(branchId) == nullptr) {
        return;
    }
    switchTarget_ = branchId;
    switchPending_ = true;
    scrubbing_ = true;
    scrubbedSinceLastTick_ = true;
}

bool TimeTravel::SwitchToBranch(uint32_t branchId, uint64_t currentTick, uint64_t& outSeekTarget)
{
    if (!enabled_ || FindBranch(branchId) == nullptr) {
        return false;
    }
    // X から根 (ライブ) までの経路。上から順に継ぎ足す
    std::vector<uint32_t> chain;
    for (uint32_t cur = branchId; cur != kLiveLane;) {
        const TimeTravelBranch* b = FindBranch(cur);
        if (b == nullptr || chain.size() > branches_.size()) {
            MYE_LOG_ERROR("[timetravel] branch B%u has a broken parent chain", branchId);
            return false;
        }
        chain.push_back(cur);
        cur = b->parent;
    }
    std::reverse(chain.begin(), chain.end());
    const uint64_t f0 = FindBranch(chain.front())->forkTick;
    if (f0 < firstTick_ || f0 > EndTick()) {
        MYE_LOG_ERROR("[timetravel] branch B%u forks outside the live lane", branchId);
        return false;
    }
    // いまのライブの suffix は分岐として残す (分岐点の pinned スナップショットごと)
    const uint32_t demoted = SplitSuffix(kLiveLane, f0);
    for (size_t i = 0; i < chain.size(); ++i) {
        if (i + 1 < chain.size()) {
            // 次の要素が枝分かれする点より先は、この要素自身の続きなので分岐として切り離す
            SplitSuffix(chain[i], FindBranch(chain[i + 1])->forkTick);
        }
        Graft(chain[i]);
    }
    outSeekTarget = std::min(currentTick, EndTick());
    EnforceBranchLimit(kLiveLane);
    MYE_LOG_INFO("[timetravel] switched to branch B%u (fork %llu, live is now [%llu, %llu), the old "
                 "future is %s%u)",
                 branchId, static_cast<unsigned long long>(f0),
                 static_cast<unsigned long long>(firstTick_),
                 static_cast<unsigned long long>(EndTick()),
                 demoted != 0 ? "branch B" : "gone (nothing to keep), id ", demoted);
    return true;
}

void TimeTravel::DeleteBranch(uint32_t branchId)
{
    std::vector<uint32_t> victims;
    victims.push_back(branchId);
    for (size_t i = 0; i < victims.size(); ++i) {
        for (const TimeTravelBranch& b : branches_) {
            if (b.parent == victims[i] && b.id != kLiveLane) {
                victims.push_back(b.id);
            }
        }
    }
    for (uint32_t v : victims) {
        auto it = std::find_if(branches_.begin(), branches_.end(),
                               [v](const TimeTravelBranch& b) { return b.id == v; });
        if (it == branches_.end()) {
            continue;
        }
        for (const TimeTravelSnap& s : it->snapshots) {
            bytes_ -= s.blob.size();
        }
        branches_.erase(it);
        if (switchPending_ && switchTarget_ == v) {
            switchPending_ = false;
        }
    }
}

const TimeTravelBranch* TimeTravel::FindBranch(uint32_t id) const
{
    for (const TimeTravelBranch& b : branches_) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

TimeTravelBranch* TimeTravel::FindBranchMut(uint32_t id)
{
    for (TimeTravelBranch& b : branches_) {
        if (b.id == id) {
            return &b;
        }
    }
    return nullptr;
}

bool TimeTravel::HasUnbakedGhost() const
{
    for (const TimeTravelBranch& b : branches_) {
        if (!b.ghostBaked) {
            return true;
        }
    }
    return false;
}

TimeTravel::LaneRef TimeTravel::Lane(uint32_t lane)
{
    LaneRef r;
    if (lane == kLiveLane) {
        r.id = kLiveLane;
        r.parent = kLiveLane;
        r.forkTick = firstTick_;
        r.startHash = startHash_;
        r.entries = &entries_;
        r.snapshots = &snapshots_;
        r.valid = enabled_;
        return r;
    }
    TimeTravelBranch* b = FindBranchMut(lane);
    if (b == nullptr) {
        return r;
    }
    r.id = b->id;
    r.parent = b->parent;
    r.forkTick = b->forkTick;
    r.startHash = b->startHash;
    r.entries = &b->entries;
    r.snapshots = &b->snapshots;
    r.valid = enabled_;
    return r;
}

TimeTravel::LaneRef TimeTravel::Lane(uint32_t lane) const
{
    // 読み取り専用の経路でも同じ束を使う (呼び出し側は書かない)
    return const_cast<TimeTravel*>(this)->Lane(lane);
}

bool TimeTravel::HasLane(uint32_t lane) const
{
    return Lane(lane).valid;
}

uint64_t TimeTravel::EndTickOn(uint32_t lane) const
{
    const LaneRef L = Lane(lane);
    return L.valid ? L.End() : 0;
}

uint64_t TimeTravel::ForkTickOn(uint32_t lane) const
{
    const LaneRef L = Lane(lane);
    return L.valid ? L.forkTick : 0;
}

const TimeTravelEntry* TimeTravel::EntryOn(uint32_t lane, uint64_t t) const
{
    LaneRef L = Lane(lane);
    size_t guard = 0;
    while (L.valid) {
        if (t >= L.forkTick) {
            if (t < L.End()) {
                return &(*L.entries)[static_cast<size_t>(t - L.forkTick)];
            }
            return nullptr;
        }
        if (L.id == kLiveLane || ++guard > branches_.size()) {
            return nullptr;
        }
        L = Lane(L.parent);
    }
    return nullptr;
}

const TimeTravelEntry* TimeTravel::Entry(uint64_t t) const
{
    return EntryOn(kLiveLane, t);
}

uint64_t TimeTravel::HashAtTickOn(uint32_t lane, uint64_t t) const
{
    LaneRef L = Lane(lane);
    size_t guard = 0;
    while (L.valid) {
        if (t >= L.forkTick && t <= L.End()) {
            // ★このレーンにその tick のスナップショットがあれば、その状態が正
            //   (編集点では entry の hashAfter と食い違うのが正しい)
            const auto& snaps = *L.snapshots;
            auto it = std::lower_bound(snaps.begin(), snaps.end(), t,
                                       [](const TimeTravelSnap& s, uint64_t v) { return s.tick < v; });
            if (it != snaps.end() && it->tick == t) {
                return it->stateHash;
            }
            if (t == L.forkTick) {
                return L.startHash;
            }
            return (*L.entries)[static_cast<size_t>(t - L.forkTick - 1)].hashAfter;
        }
        if (t >= L.forkTick || L.id == kLiveLane || ++guard > branches_.size()) {
            return 0;
        }
        L = Lane(L.parent);
    }
    return 0;
}

const std::vector<std::byte>* TimeTravel::SnapshotAtOrBeforeOn(uint32_t lane, uint64_t target,
                                                               uint64_t& outTick) const
{
    LaneRef L = Lane(lane);
    uint64_t limit = UINT64_MAX; // 親レーンでは forkTick 未満の枚だけが候補 (排他的上限)
    size_t guard = 0;
    while (L.valid) {
        const auto& snaps = *L.snapshots;
        for (size_t i = snaps.size(); i > 0; --i) {
            const TimeTravelSnap& s = snaps[i - 1];
            if (s.tick <= target && s.tick < limit) {
                outTick = s.tick;
                return &s.blob;
            }
        }
        if (L.id == kLiveLane || ++guard > branches_.size()) {
            return nullptr;
        }
        limit = std::min(limit, L.forkTick);
        L = Lane(L.parent);
    }
    return nullptr;
}

DivergenceReport TimeTravel::FirstDivergence(uint32_t laneA, uint32_t laneB) const
{
    DivergenceReport r;
    if (!HasLane(laneA) || !HasLane(laneB)) {
        return r;
    }
    const uint64_t begin = std::max(ForkTickOn(laneA), ForkTickOn(laneB));
    const uint64_t end = std::min(EndTickOn(laneA), EndTickOn(laneB));
    if (begin > end) {
        return r;
    }
    r.comparable = true;
    r.commonBegin = begin;
    r.commonEnd = end;
    for (uint64_t t = begin; t <= end; ++t) {
        if (HashAtTickOn(laneA, t) != HashAtTickOn(laneB, t)) {
            r.diverged = true;
            r.firstTick = t;
            break;
        }
    }
    return r;
}

// ---------------------------------------------------------------- 内部

bool TimeTravel::TakeSnapshot(const SimRefs& refs, uint64_t tick, uint64_t stateHash, bool pinned)
{
    TimeTravelSnap s;
    s.tick = tick;
    s.stateHash = stateHash;
    s.pinned = pinned;
    if (!CaptureSimSnapshot(refs, s.blob)) {
        MYE_LOG_ERROR("[timetravel] snapshot capture failed at tick %llu",
                      static_cast<unsigned long long>(tick));
        return false;
    }
    bytes_ += s.blob.size();
    snapshots_.push_back(std::move(s));
    Evict();
    return true;
}

void TimeTravel::Evict()
{
    // (1) 予算超過は、まず分岐側の pinned でないスナップショットを古い分岐から落とす。
    //     entry 列は残すので「分岐点から再シムして辿り着く」能力は失わない
    while (bytes_ > config_.maxBytes) {
        TimeTravelBranch* victim = nullptr;
        for (TimeTravelBranch& b : branches_) {
            const bool hasLoose = std::any_of(b.snapshots.begin(), b.snapshots.end(),
                                              [](const TimeTravelSnap& s) { return !s.pinned; });
            if (hasLoose && (victim == nullptr || b.createdSeq < victim->createdSeq)) {
                victim = &b;
            }
        }
        if (victim == nullptr) {
            break;
        }
        for (size_t i = victim->snapshots.size(); i > 0; --i) {
            if (!victim->snapshots[i - 1].pinned) {
                bytes_ -= victim->snapshots[i - 1].blob.size();
                victim->snapshots.erase(victim->snapshots.begin() + static_cast<ptrdiff_t>(i - 1));
                break;
            }
        }
    }
    // (2) それでも超過 / 枚数超過なら、ライブの古い方から
    while (snapshots_.size() > 1
           && (snapshots_.size() > config_.maxSnapshots || bytes_ > config_.maxBytes)) {
        bytes_ -= snapshots_.front().blob.size();
        snapshots_.erase(snapshots_.begin());
    }
    // ★最古スナップショットより前へは戻れない = その入力を持っていても意味が無い。
    //   タイムラインの範囲もそこまで縮める (戻れない目盛りを見せない)
    if (!snapshots_.empty()) {
        const uint64_t oldest = snapshots_.front().tick;
        if (oldest > firstTick_) {
            const size_t drop = static_cast<size_t>(oldest - firstTick_);
            if (drop <= entries_.size()) { // ありえないが、範囲外アクセスよりは据え置きの方が安全
                startHash_ = entries_[drop - 1].hashAfter; // 新しい先頭 tick が走る前のハッシュ
                entries_.erase(entries_.begin(), entries_.begin() + static_cast<ptrdiff_t>(drop));
                firstTick_ = oldest;
            }
        }
    }
    PruneUnreachable();
}

void TimeTravel::EnforceBranchLimit(uint32_t protect)
{
    // 本数上限: 最古の葉から消す (親を先に消すと子の prefix が宙に浮く)。
    // ★SwitchToBranch の途中では呼ばない — 継ぎ足し待ちの要素が葉として消える。
    //   protect = いま作ったばかりの分岐 (捨てた未来そのもの) は候補から外す
    while (branches_.size() > config_.maxBranches) {
        const TimeTravelBranch* oldestLeaf = nullptr;
        for (const TimeTravelBranch& b : branches_) {
            if (b.id != protect && IsLeaf(b.id)
                && (oldestLeaf == nullptr || b.createdSeq < oldestLeaf->createdSeq)) {
                oldestLeaf = &b;
            }
        }
        if (oldestLeaf == nullptr) {
            break;
        }
        MYE_LOG_INFO("[timetravel] branch B%u dropped (more than %zu branches)", oldestLeaf->id,
                     config_.maxBranches);
        DeleteBranch(oldestLeaf->id);
    }
}

void TimeTravel::PruneUnreachable()
{
    // ライブの最古 tick より前で分岐したものは、もう戻れない = 部分木ごと消す
    for (;;) {
        uint32_t victim = 0;
        for (const TimeTravelBranch& b : branches_) {
            if (b.parent == kLiveLane && b.forkTick < firstTick_) {
                victim = b.id;
                break;
            }
        }
        if (victim == 0) {
            return;
        }
        MYE_LOG_INFO("[timetravel] branch B%u dropped (its fork tick left the ring)", victim);
        DeleteBranch(victim);
    }
}

uint32_t TimeTravel::SplitSuffix(uint32_t lane, uint64_t at)
{
    LaneRef L = Lane(lane);
    if (!L.valid || at < L.forkTick || at > L.End()) {
        return 0;
    }
    const size_t idx = static_cast<size_t>(at - L.forkTick);
    auto& snaps = *L.snapshots;
    auto snapIt = std::lower_bound(snaps.begin(), snaps.end(), at,
                                   [](const TimeTravelSnap& s, uint64_t v) { return s.tick < v; });
    if (idx >= L.entries->size() && snapIt == snaps.end()) {
        return 0;
    }
    TimeTravelBranch b;
    b.id = nextBranchId_++;
    b.parent = lane;
    b.forkTick = at;
    b.startHash = HashAtTickOn(lane, at); // 移す前に読む (at のスナップショットがあればその値)
    b.createdSeq = branchSeq_++;
    if (idx < L.entries->size()) {
        b.entries.assign(std::make_move_iterator(L.entries->begin() + static_cast<ptrdiff_t>(idx)),
                         std::make_move_iterator(L.entries->end()));
        L.entries->resize(idx);
    }
    if (snapIt != snaps.end()) {
        b.snapshots.assign(std::make_move_iterator(snapIt), std::make_move_iterator(snaps.end()));
        snaps.erase(snapIt, snaps.end());
    }
    if (!b.snapshots.empty() && b.snapshots.front().tick == at) {
        b.snapshots.front().pinned = true; // 分岐の再シム起点。予算追い出しで落とさない
    }
    // lane の子で at より先に枝分かれしていたものは、prefix の所有者が新分岐へ移る
    for (TimeTravelBranch& c : branches_) {
        if (c.parent == lane && c.forkTick > at) {
            c.parent = b.id;
        }
    }
    const uint32_t id = b.id;
    branches_.push_back(std::move(b)); // ★ここで L の参照は無効になる (以後触らない)
    return id;
}

void TimeTravel::Graft(uint32_t branchId)
{
    TimeTravelBranch* b = FindBranchMut(branchId);
    if (b == nullptr) {
        return;
    }
    if (b->forkTick != EndTick()) {
        MYE_LOG_ERROR("[timetravel] cannot graft B%u at %llu onto a live lane ending at %llu",
                      branchId, static_cast<unsigned long long>(b->forkTick),
                      static_cast<unsigned long long>(EndTick()));
        return;
    }
    if (entries_.empty() && snapshots_.empty()) {
        startHash_ = b->startHash; // ライブが空 (ありえないが) なら開始ハッシュも継ぐ
    }
    entries_.insert(entries_.end(), std::make_move_iterator(b->entries.begin()),
                    std::make_move_iterator(b->entries.end()));
    snapshots_.insert(snapshots_.end(), std::make_move_iterator(b->snapshots.begin()),
                      std::make_move_iterator(b->snapshots.end()));
    for (TimeTravelBranch& c : branches_) {
        if (c.parent == branchId) {
            c.parent = kLiveLane;
        }
    }
    branches_.erase(std::find_if(branches_.begin(), branches_.end(),
                                 [branchId](const TimeTravelBranch& x) { return x.id == branchId; }));
    simSinceSnapshot_ = 0;
}

void TimeTravel::CollapseIdentical(uint64_t ranTick)
{
    // ライブが分岐の終端へ追いついた瞬間に 1 回だけ比べる。同じ入力で同じ未来を
    // なぞっただけの分岐 (Step / 再開を繰り返したときに量産される) は残す価値が無い
    for (;;) {
        uint32_t victim = 0;
        for (const TimeTravelBranch& b : branches_) {
            if (b.parent != kLiveLane || !IsLeaf(b.id) || b.EndTick() != ranTick + 1
                || b.forkTick > ranTick) {
                continue;
            }
            const DivergenceReport d = FirstDivergence(kLiveLane, b.id);
            if (d.comparable && !d.diverged) {
                victim = b.id;
                break;
            }
        }
        if (victim == 0) {
            return;
        }
        MYE_LOG_INFO("[timetravel] branch B%u collapsed: identical to the live lane", victim);
        DeleteBranch(victim);
    }
}

bool TimeTravel::IsLeaf(uint32_t id) const
{
    for (const TimeTravelBranch& b : branches_) {
        if (b.parent == id) {
            return false;
        }
    }
    return true;
}

void TimeTravel::RequestSeek(uint64_t tick)
{
    if (!enabled_) {
        return;
    }
    seekTarget_ = std::clamp(tick, firstTick_, EndTick());
    seekPending_ = true;
    scrubbing_ = true;
    scrubbedSinceLastTick_ = true;
}

} // namespace mye
