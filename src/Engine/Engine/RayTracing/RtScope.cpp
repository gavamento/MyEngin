//====================================================================================
//                          RtScope.cpp
//  MyEngin/ 秋田蓮音                                                       09/30/2026
//                                          RT の適用範囲の判定の実装
//====================================================================================
#include "Engine/Engine/RayTracing/RtScope.h"

#include <bit>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/Tags.h"

namespace mye {

namespace {

// 個別設定の 1 欄をレーンへ反映する。既に決まっていれば触らない (近い方が勝つ)
void ApplyExplicit(RtScopeLane& lane, bool& decided, int32_t mode, EntityID owner)
{
    if (decided || (mode != kRtScopeOn && mode != kRtScopeOff)) {
        return; // 継承 (と範囲外の値) は素通り
    }
    lane.on = (mode == kRtScopeOn);
    lane.source = RtScopeSource::Explicit;
    lane.from = owner;
    decided = true;
}

// 個別設定で決まらなかったレーンをタグ規則で決める
void ApplyTagRules(RtScopeLane& lane, uint64_t tagMask, uint64_t onMask, uint64_t offMask)
{
    if (const uint64_t hitOff = tagMask & offMask; hitOff != 0) {
        lane.on = false;
        lane.source = RtScopeSource::Tag;
        lane.tagIndex = std::countr_zero(hitOff);
    } else if (const uint64_t hitOn = tagMask & onMask; hitOn != 0) {
        lane.on = true;
        lane.source = RtScopeSource::Tag;
        lane.tagIndex = std::countr_zero(hitOn);
    }
}

} // namespace

RtScope ResolveRtScope(World& world, EntityID e, const RtTagRules& rules)
{
    RtScope scope;
    bool sceneDecided = false;
    bool receiverDecided = false;
    uint64_t tagMask = 0;
    // 祖先を 1 回だけ辿り、個別設定とタグの OR を同時に集める
    for (EntityID cur = e; !cur.IsNull(); cur = world.GetParent(cur)) {
        if (const auto* rt = world.GetComponent<RayTracingComponent>(cur)) {
            ApplyExplicit(scope.inScene, sceneDecided, rt->inScene, cur);
            ApplyExplicit(scope.receiver, receiverDecided, rt->receiver, cur);
        }
        tagMask |= Tags::OwnMask(world, cur);
    }
    if (!sceneDecided) {
        ApplyTagRules(scope.inScene, tagMask, rules.sceneOn, rules.sceneOff);
    }
    if (!receiverDecided) {
        ApplyTagRules(scope.receiver, tagMask, rules.receiverOn, rules.receiverOff);
    }
    return scope;
}

} // namespace mye
