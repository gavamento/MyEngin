//====================================================================================
//                          DisturbanceParams.h
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の場の設定の切り方と、ECS から集める行 (sim と描画で共有)
//====================================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Disturbance/DisturbanceState.h"

namespace mye {

class World;

namespace disturbance {

inline float FiniteOr(float v, float fallback)
{
    return std::isfinite(v) ? v : fallback;
}

// 非有限なら fallback、そうでなければ [lo, hi] へ切る (sim の ClampOr)
float ClampOr(float v, float lo, float hi, float fallback);

// 切った後の場の設定。sim も描画もこの値だけで動く (描画が別の切り方をすると絵と状態がずれる)
struct FieldParams {
    EntityID entity = kNullEntity;
    int32_t shape = kDisturbanceGroundLayer;
    int32_t cellsX = 0;
    int32_t cellsY = 1;
    int32_t cellsZ = 0;
    float cellSize = 0.25f;
    float thickness = 1.0f;
    uint32_t channels = 0;
    float restDensity = 1.0f;
    float densityKeep = 1.0f;
    float velocityKeep = 1.0f;
    float trailKeep = 1.0f;
    float rippleSpeed = 0.0f;
    float rippleKeep = 1.0f;
    int32_t pressureIterations = 0; // [0, kMaxPressureIterations]
    float windScale = 0.0f;         // [0, kMaxWindScale]
    float pos[3] = { 0.0f, 0.0f, 0.0f }; // 場のエンティティのワールド位置
};

FieldParams SanitizeField(EntityID e, const DisturbanceFieldComponent& c, const float pos[3]);

// 初期化の原点セル (最初の step と編集中の描画が同じ格子を指すための式)。pos は切った後のワールド位置
int32_t OriginCellOf(float worldPos, float cellSize, int32_t cells);

struct FieldRow {
    EntityID entity;
    DisturbanceFieldComponent comp;
    float pos[3];
};

// 場を持つ全エンティティを entity index の昇順で集める
void CollectFields(World& world, std::vector<FieldRow>& out);

} // namespace disturbance
} // namespace mye
