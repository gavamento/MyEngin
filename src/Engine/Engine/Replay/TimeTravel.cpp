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
    //   ここだけ引数が欠けるとシークの自己検証が「割れていないのに割れた」と言い出す
    return HashWorld(refs.scene->GetWorld(),
                     {refs.particles, &refs.scene->Time(), &refs.scene->Persist()});
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
    seekTarget_ = 0;
    firstTick_ = 0;
    startHash_ = 0;
    simSinceSnapshot_ = 0;
    bytes_ = 0;
    entries_.clear();
    snapshots_.clear();
    lastSeek_ = SeekReport{};
}

void TimeTravel::Begin(const SimRefs& refs, uint64_t tick)
{
    entries_.clear();
    snapshots_.clear();
    bytes_ = 0;
    firstTick_ = tick;
    simSinceSnapshot_ = 0;
    scrubbing_ = false;
    seekPending_ = false;
    lastSeek_ = SeekReport{};
    startHash_ = HashOf(refs);
    enabled_ = true;
    if (!TakeSnapshot(refs, tick)) {
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
        // 分岐: シークで戻った後に走った tick は、記録済みの未来を置き換える。
        // Unity には無い挙動なので UI 側で明示すること
        entries_.resize(idx);
        DropSnapshotsAfter(ranTick);
        simSinceSnapshot_ = 0;
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

    if (simulated) {
        ++simSinceSnapshot_;
    }
    if (simSinceSnapshot_ >= config_.snapshotInterval) {
        simSinceSnapshot_ = 0;
        TakeSnapshot(refs, ranTick + 1); // 「次の tick が走る前」の状態
    }
}

void TimeTravel::RequestSeek(uint64_t tick)
{
    if (!enabled_) {
        return;
    }
    seekTarget_ = std::clamp(tick, firstTick_, EndTick());
    seekPending_ = true;
    scrubbing_ = true;
}

const TimeTravelEntry* TimeTravel::Entry(uint64_t t) const
{
    if (!HasTick(t)) {
        return nullptr;
    }
    return &entries_[static_cast<size_t>(t - firstTick_)];
}

uint64_t TimeTravel::HashAtTick(uint64_t t) const
{
    if (!enabled_ || t < firstTick_ || t > EndTick()) {
        return 0;
    }
    if (t == firstTick_) {
        return startHash_;
    }
    return entries_[static_cast<size_t>(t - firstTick_ - 1)].hashAfter;
}

const std::vector<std::byte>* TimeTravel::SnapshotAtOrBefore(uint64_t target,
                                                             uint64_t& outTick) const
{
    for (size_t i = snapshots_.size(); i > 0; --i) {
        const Snap& s = snapshots_[i - 1];
        if (s.tick <= target) {
            outTick = s.tick;
            return &s.blob;
        }
    }
    return nullptr;
}

bool TimeTravel::TakeSnapshot(const SimRefs& refs, uint64_t tick)
{
    Snap s;
    s.tick = tick;
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
    while (snapshots_.size() > 1
           && (snapshots_.size() > config_.maxSnapshots || bytes_ > config_.maxBytes)) {
        bytes_ -= snapshots_.front().blob.size();
        snapshots_.erase(snapshots_.begin());
    }
    // ★最古スナップショットより前へは戻れない = その入力を持っていても意味が無い。
    //   タイムラインの範囲もそこまで縮める (戻れない目盛りを見せない)
    if (snapshots_.empty()) {
        return;
    }
    const uint64_t oldest = snapshots_.front().tick;
    if (oldest <= firstTick_) {
        return;
    }
    const size_t drop = static_cast<size_t>(oldest - firstTick_);
    if (drop > entries_.size()) {
        return; // ありえないが、範囲外アクセスよりは据え置きの方が安全
    }
    startHash_ = entries_[drop - 1].hashAfter; // 新しい先頭 tick が走る前のハッシュ
    entries_.erase(entries_.begin(), entries_.begin() + static_cast<ptrdiff_t>(drop));
    firstTick_ = oldest;
}

void TimeTravel::DropSnapshotsAfter(uint64_t tick)
{
    // tick 番のスナップショットは「tick が走る前」なので残す (走り直しても前の状態は同じ)
    while (!snapshots_.empty() && snapshots_.back().tick > tick) {
        bytes_ -= snapshots_.back().blob.size();
        snapshots_.pop_back();
    }
}

} // namespace mye
