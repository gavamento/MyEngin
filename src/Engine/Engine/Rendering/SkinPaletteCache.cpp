//====================================================================================
//                          SkinPaletteCache.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          スキンのボーンパレットのビュー間キャッシュの実装
//====================================================================================
#include "Engine/Engine/Rendering/SkinPaletteCache.h"

namespace mye {
namespace {

// 使われなくなったエントリを手放すまでの Render 回数。掃除の周期も兼ねる
constexpr uint64_t kSweepViews = 512;

} // namespace

bool SkinPaletteCache::Reusable(const Entry& e, const Request& req)
{
    if (!e.valid || !e.cacheable || !req.cacheable || e.entity != req.entity || e.model != req.model) {
        return false;
    }
    // 入力が同じなら、いつ作ったパレットでも今作り直したものと同じ
    if (e.alpha == req.alpha && SameRenderPoseInputs(e.pose, *req.sm)) {
        return true;
    }
    if (req.interval <= 1) {
        return false;
    }
    // 間引き: 窓の途中 (更新 tick でない tick) では、同じ窓で作ったパレットをそのまま使う (補間しない)。
    // 同じ tick に作ったものは除く: tick が進まない編集中にポーズが変わったときは、作り直して見た目へ反映する
    const int64_t window = UroWindowStart(req.simTick, req.entity.index, req.interval);
    return window < static_cast<int64_t>(req.simTick) && e.window == window && e.interval == req.interval
           && e.evalTick != req.simTick;
}

SkinPaletteCache::Entry* SkinPaletteCache::Find(const Request& req)
{
    if (req.entity.index >= slotByIndex_.size() || slotByIndex_[req.entity.index] < 0) {
        return nullptr;
    }
    Entry& e = entries_[static_cast<size_t>(slotByIndex_[req.entity.index])];
    if (!Reusable(e, req)) {
        return nullptr;
    }
    e.lastUsedView = viewCounter_;
    return &e;
}

SkinPaletteCache::Entry* SkinPaletteCache::Claim(const Request& req)
{
    if (req.entity.index >= slotByIndex_.size()) {
        slotByIndex_.resize(static_cast<size_t>(req.entity.index) + 1, -1);
    }
    int32_t& slot = slotByIndex_[req.entity.index];
    if (slot < 0) {
        if (!freeSlots_.empty()) {
            slot = freeSlots_.back();
            freeSlots_.pop_back();
        } else {
            entries_.emplace_back();
            slot = static_cast<int32_t>(entries_.size() - 1);
        }
        ++liveCount_;
    }
    Entry& e = entries_[static_cast<size_t>(slot)];
    e.entity = req.entity;
    e.model = req.model;
    e.pose = *req.sm;
    e.alpha = req.alpha;
    e.interval = req.interval;
    e.window = UroWindowStart(req.simTick, req.entity.index, req.interval);
    e.evalTick = req.simTick;
    e.cacheable = req.cacheable;
    e.valid = true;
    e.lastUsedView = viewCounter_;
    return &e;
}

void SkinPaletteCache::BeginFrame()
{
    ++viewCounter_;
    if (viewCounter_ % kSweepViews != 0) {
        return;
    }
    for (size_t index = 0; index < slotByIndex_.size(); ++index) {
        const int32_t slot = slotByIndex_[index];
        if (slot < 0 || entries_[static_cast<size_t>(slot)].lastUsedView + kSweepViews > viewCounter_) {
            continue;
        }
        Entry& e = entries_[static_cast<size_t>(slot)];
        e.valid = false;
        std::vector<DirectX::XMFLOAT4X4>().swap(e.palette);
        slotByIndex_[index] = -1;
        freeSlots_.push_back(slot);
        --liveCount_;
    }
}

void SkinPaletteCache::Clear()
{
    entries_.clear();
    slotByIndex_.clear();
    freeSlots_.clear();
    liveCount_ = 0;
}

} // namespace mye
