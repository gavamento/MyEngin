//====================================================================================
//                          DisturbanceParams.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の場の設定の切り方と、ECS から集める行の実装
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceParams.h"

#include <algorithm>

#include "Engine/Core/Ecs/World.h"

// ★決定論 (check_rules 規則 18): + - * / sqrt floor ceil min max だけ。CRT の超越関数は使わない

namespace mye::disturbance {

float ClampOr(float v, float lo, float hi, float fallback)
{
    return std::min(std::max(FiniteOr(v, fallback), lo), hi);
}

FieldParams SanitizeField(EntityID e, const DisturbanceFieldComponent& c, const float pos[3])
{
    FieldParams p;
    p.entity = e;
    p.shape = c.shape == kDisturbanceVolume ? kDisturbanceVolume : kDisturbanceGroundLayer;
    p.cellsX = std::clamp(c.cellsX, kMinDisturbanceSide, kMaxDisturbanceSide);
    p.cellsZ = std::clamp(c.cellsZ, kMinDisturbanceSide, kMaxDisturbanceSide);
    p.cellsY = p.shape == kDisturbanceGroundLayer ? 1 : std::clamp(c.cellsY, kMinDisturbanceSide, kMaxDisturbanceSide);
    // 総マス数の上限 (2D は 512 x 512 = 262144 で上限ちょうど。Y を持つ形に備えて Z を詰める)
    while (static_cast<int64_t>(p.cellsX) * p.cellsY * p.cellsZ > kMaxDisturbanceCells && p.cellsZ > kMinDisturbanceSide) {
        --p.cellsZ;
    }
    p.cellSize = ClampOr(c.cellSize, 0.05f, 8.0f, 0.25f);
    p.thickness = ClampOr(c.thickness, 0.05f, 20.0f, 1.0f);
    // Volume は濃さだけ (跡・波紋・嵩のビットは無視する)
    p.channels = c.channels & (p.shape == kDisturbanceVolume ? kDisturbanceChannelDensity : kDisturbanceChannelMask);
    p.restDensity = ClampOr(c.restDensity, 0.0f, 1.0f, 1.0f);
    p.densityKeep = ClampOr(c.densityKeep, 0.0f, 1.0f, 1.0f);
    p.velocityKeep = ClampOr(c.velocityKeep, 0.0f, 1.0f, 1.0f);
    p.trailKeep = ClampOr(c.trailKeep, 0.0f, 1.0f, 1.0f);
    p.rippleSpeed = ClampOr(c.rippleSpeed, 0.0f, 1.0e6f, 0.0f);
    p.rippleKeep = ClampOr(c.rippleKeep, 0.0f, 1.0f, 1.0f);
    p.pressureIterations = std::clamp(c.pressureIterations, 0, kMaxPressureIterations);
    p.windScale = ClampOr(c.windScale, 0.0f, kMaxWindScale, 1.0f);
    p.buoyancy = ClampOr(c.buoyancy, 0.0f, kMaxBuoyancy, 0.0f);
    p.massGravity = ClampOr(c.massGravity, 0.0f, kMaxMassGravity, 0.0f);
    p.ignoreColliders = c.ignoreColliders;
    p.followTarget = c.followTarget;
    // ずらしは軸のどれかがこの値以上ずれた step で起きる。格子の半分を超えると中心に戻せないので半分で切る
    const int32_t smallestSide = p.shape == kDisturbanceVolume ? std::min({ p.cellsX, p.cellsY, p.cellsZ }) : std::min(p.cellsX, p.cellsZ);
    p.followMargin = std::clamp(c.followMargin, 1, std::max(1, smallestSide / 2));
    p.followTerrain = c.followTerrain && p.shape == kDisturbanceGroundLayer;
    for (int i = 0; i < 3; ++i) {
        p.pos[i] = FiniteOr(pos[i], 0.0f);
    }
    return p;
}

int32_t OriginCellOf(float worldPos, float cellSize, int32_t cells)
{
    // int32 へ落とす前に範囲を切る (巨大・非有限の座標で未定義動作を踏まない)
    constexpr float kLimit = 1.0e8f;
    const float rounded = std::floor(worldPos / cellSize + 0.5f);
    return static_cast<int32_t>(std::min(std::max(FiniteOr(rounded, 0.0f), -kLimit), kLimit)) - cells / 2;
}

void CollectFields(World& world, std::vector<FieldRow>& out)
{
    const ComponentTypeId req[] = { DisturbanceFieldComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ti = arch.FindTypeIndex(DisturbanceFieldComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            FieldRow r{};
            r.entity = arch.EntityAt(row);
            r.comp = *static_cast<const DisturbanceFieldComponent*>(arch.GetPtr(ti, row));
            if (wi >= 0) {
                const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
                r.pos[0] = wm->value._41;
                r.pos[1] = wm->value._42;
                r.pos[2] = wm->value._43;
            }
            out.push_back(r);
        }
    });
    // アーキタイプの並びに依らない明示キー (規則 7)
    std::sort(out.begin(), out.end(), [](const FieldRow& a, const FieldRow& b) { return a.entity.index < b.entity.index; });
}

} // namespace mye::disturbance
