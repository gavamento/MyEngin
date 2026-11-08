//====================================================================================
//                          DisturbanceDebugDraw.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) のデバッグ描画 (流れの矢印と濃さの十字)
//====================================================================================
#include <algorithm>
#include <cmath>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Disturbance/DisturbanceState.h"
#include "Engine/Engine/Rendering/DebugDraw.h"

// ★出力レーン: sim の状態は読むだけ。計算は + - * / sqrt min max だけ (check_rules 規則 18)

namespace mye {

namespace {

constexpr int32_t kDebugCellStride = 4;      // 何マスおきに描くか
constexpr float kArrowSecondsPerMeter = 0.3f; // 矢印の長さ = 速さ[m/s] × この値 [m]
constexpr float kMinDrawnSpeed = 0.05f;      // これ未満の流れの矢印は描かない [m/s]
constexpr float kCrossHalfSize = 0.08f;      // 濃さの十字の半分の長さ [m]
constexpr float kDrawHeightAboveBase = 0.05f; // 板の下端からの高さ [m]
constexpr uint32_t kArrowColor = 0x40E0FFFFu;  // 0xRRGGBBAA。シアン

uint32_t GrayColor(float level)
{
    const float clamped = std::min(std::max(level, 0.0f), 1.0f);
    const uint32_t v = static_cast<uint32_t>(clamped * 255.0f);
    return (v << 24) | (v << 16) | (v << 8) | 0xFFu;
}

} // namespace

void DisturbanceAppendDebugLines(World& world, const DisturbanceState& state, std::vector<DebugLineCmd>& out)
{
    for (const DisturbanceSlot& s : state.Slots()) {
        const auto* comp = world.GetComponent<DisturbanceFieldComponent>(s.entity);
        if (comp == nullptr || !comp->drawDebug) {
            continue;
        }
        const bool hasDensity = (s.channels & kDisturbanceChannelDensity) != 0;
        const float y = s.baseY + kDrawHeightAboveBase;
        for (int32_t z = kDebugCellStride / 2; z < s.cellsZ; z += kDebugCellStride) {
            for (int32_t x = kDebugCellStride / 2; x < s.cellsX; x += kDebugCellStride) {
                const size_t i = static_cast<size_t>(x) + static_cast<size_t>(s.cellsX) * static_cast<size_t>(z);
                const float cx = (static_cast<float>(s.originCell[0] + x) + 0.5f) * s.cellSize;
                const float cz = (static_cast<float>(s.originCell[2] + z) + 0.5f) * s.cellSize;
                if (hasDensity) {
                    const uint32_t c = GrayColor(s.density[i]);
                    out.push_back({ cx - kCrossHalfSize, y, cz, cx + kCrossHalfSize, y, cz, c });
                    out.push_back({ cx, y, cz - kCrossHalfSize, cx, y, cz + kCrossHalfSize, c });
                }
                const float vx = s.flowX[i];
                const float vz = s.flowZ[i];
                const float speed = std::sqrt(vx * vx + vz * vz);
                if (!(speed >= kMinDrawnSpeed)) {
                    continue;
                }
                const float len = speed * kArrowSecondsPerMeter;
                const float dx = vx / speed;
                const float dz = vz / speed;
                const float tipX = cx + dx * len;
                const float tipZ = cz + dz * len;
                // 矢じりは先端から後ろへ、軸に垂直な方向へ開く
                const float head = std::min(len * 0.35f, 0.15f);
                const float backX = tipX - dx * head;
                const float backZ = tipZ - dz * head;
                const float sideX = -dz * head * 0.5f;
                const float sideZ = dx * head * 0.5f;
                out.push_back({ cx, y, cz, tipX, y, tipZ, kArrowColor });
                out.push_back({ tipX, y, tipZ, backX + sideX, y, backZ + sideZ, kArrowColor });
                out.push_back({ tipX, y, tipZ, backX - sideX, y, backZ - sideZ, kArrowColor });
            }
        }
    }
}

} // namespace mye
