//====================================================================================
//                          DisturbanceState.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の sim 状態 (消去・配送待ち・スナップショットの出し入れ)
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceState.h"

#include <cmath>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/ByteIo.h"

namespace mye {

namespace {

bool AllFinite(const float* v, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

// 1 つのスロットが持つべき配列の長さ (有効なチャネルの分だけ確保する規約)
size_t ExpectedLength(const DisturbanceSlot& s, uint32_t channelBit)
{
    return (s.channels & channelBit) != 0 ? s.CellCount() : 0;
}

bool SlotShapeValid(const DisturbanceSlot& s)
{
    if (s.shape != kDisturbanceGroundLayer || s.cellsY != 1) {
        return false;
    }
    if (s.cellsX < kMinDisturbanceSide || s.cellsX > kMaxDisturbanceSide || s.cellsZ < kMinDisturbanceSide
        || s.cellsZ > kMaxDisturbanceSide) {
        return false;
    }
    if (static_cast<int64_t>(s.cellsX) * s.cellsZ > kMaxDisturbanceCells || (s.channels & ~kDisturbanceChannelMask) != 0) {
        return false;
    }
    return std::isfinite(s.cellSize) && s.cellSize > 0.0f && std::isfinite(s.baseY);
}

} // namespace

void DisturbanceState::Clear()
{
    slots_.clear();
    queue_.clear();
    warnedFields_.clear();
    rippleScratch_.clear();
    queueFullWarned_ = false;
    ++renderSerial_; // 中身が変わったので描画の転送を省かせない (通番は巻き戻さない)
}

bool DisturbanceState::QueueImpulse(const FieldImpulse& impulse)
{
    // 不正値を状態へ入れない (NaN が格子に混ざると全マスへ広がる)
    if (!AllFinite(impulse.pos, 3) || !AllFinite(impulse.dir, 3) || !std::isfinite(impulse.radius)
        || !std::isfinite(impulse.speed) || !std::isfinite(impulse.coneCos) || !std::isfinite(impulse.density)
        || !std::isfinite(impulse.trail) || !std::isfinite(impulse.ripple) || impulse.shape > kFieldImpulseCone) {
        MYE_LOG_WARN("[disturbance] impulse rejected: non-finite value or unknown shape");
        return false;
    }
    if (queue_.size() >= kMaxDisturbanceQueue) {
        if (!queueFullWarned_) {
            queueFullWarned_ = true;
            MYE_LOG_WARN("[disturbance] impulse queue is full (%u); impulses are dropped until the next step",
                         kMaxDisturbanceQueue);
        }
        return false;
    }
    queue_.push_back(impulse);
    return true;
}

void DisturbanceState::Write(ByteWriter& w) const
{
    w.Count(slots_.size());
    for (const DisturbanceSlot& s : slots_) {
        w.U32(s.entity.index);
        w.U32(s.entity.generation);
        w.I32(s.shape);
        w.I32(s.cellsX);
        w.I32(s.cellsY);
        w.I32(s.cellsZ);
        w.U32(s.channels);
        w.F32(s.cellSize);
        for (int32_t o : s.originCell) {
            w.I32(o);
        }
        w.F32(s.baseY);
        w.PodVector(s.flowX);
        w.PodVector(s.flowZ);
        w.PodVector(s.density);
        w.PodVector(s.trail);
        w.PodVector(s.ripple);
        w.PodVector(s.ripplePrev);
    }
    w.PodVector(queue_);
}

bool DisturbanceState::Read(ByteReader& r, DisturbanceState& out)
{
    DisturbanceState tmp;
    const size_t slotCount = r.Count(1);
    if (!r.Ok() || slotCount > kMaxDisturbanceFields) {
        return false;
    }
    tmp.slots_.resize(slotCount);
    for (DisturbanceSlot& s : tmp.slots_) {
        s.entity.index = r.U32();
        s.entity.generation = r.U32();
        s.shape = r.I32();
        s.cellsX = r.I32();
        s.cellsY = r.I32();
        s.cellsZ = r.I32();
        s.channels = r.U32();
        s.cellSize = r.F32();
        for (int32_t& o : s.originCell) {
            o = r.I32();
        }
        s.baseY = r.F32();
        // 寸法を検証してから配列を読む (壊れた blob の巨大な長さを確保しない)
        if (!r.Ok() || !SlotShapeValid(s)) {
            return false;
        }
        s.flowX = r.PodVector<float>();
        s.flowZ = r.PodVector<float>();
        s.density = r.PodVector<float>();
        s.trail = r.PodVector<float>();
        s.ripple = r.PodVector<float>();
        s.ripplePrev = r.PodVector<float>();
        const size_t n = s.CellCount();
        if (!r.Ok() || s.flowX.size() != n || s.flowZ.size() != n
            || s.density.size() != ExpectedLength(s, kDisturbanceChannelDensity)
            || s.trail.size() != ExpectedLength(s, kDisturbanceChannelTrail)
            || s.ripple.size() != ExpectedLength(s, kDisturbanceChannelRipple)
            || s.ripplePrev.size() != ExpectedLength(s, kDisturbanceChannelRipple)) {
            return false;
        }
    }
    tmp.queue_ = r.PodVector<FieldImpulse>();
    if (!r.Ok() || tmp.queue_.size() > kMaxDisturbanceQueue) {
        return false;
    }
    // 描画の通番は状態ではないので引き継いで進める。0 へ戻すと、直前に転送した通番とたまたま一致して
    // 中身の違う場の転送を省いてしまう
    tmp.renderSerial_ = out.renderSerial_ + 1;
    out = std::move(tmp);
    return true;
}

} // namespace mye
