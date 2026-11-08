//====================================================================================
//                          DisturbanceRender.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の sim 状態を描画への転送指示へ詰め替える実装
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceRender.h"

#include <algorithm>

#include "Engine/Engine/Disturbance/DisturbanceParams.h"
#include "Engine/Engine/Disturbance/DisturbanceState.h"
#include "Engine/Renderer/Passes/DisturbanceFieldPass.h"

// ★このファイルは sim の値を読んで並べ替えるだけ。計算は + - * / min max だけ (check_rules 規則 18)

namespace mye {

using namespace disturbance;

namespace {

const float* DataOrNull(const std::vector<float>& v)
{
    return v.empty() ? nullptr : v.data();
}

// スロットが場の設定と同じ形・寸法・マスの大きさか (違えば次の step で作り直される = 今は読まない)
bool SlotUsable(const DisturbanceSlot& s, const FieldParams& p)
{
    return s.shape == p.shape && s.cellsX == p.cellsX && s.cellsY == p.cellsY && s.cellsZ == p.cellsZ
           && s.channels == p.channels && s.cellSize == p.cellSize;
}

} // namespace

int32_t BuildDisturbanceFieldUpload(World& world, const DisturbanceState* state, DisturbanceFieldUpload& out)
{
    out = DisturbanceFieldUpload{};
    std::vector<FieldRow> rows;
    CollectFields(world, rows);
    const size_t considered = std::min<size_t>(rows.size(), kMaxDisturbanceFields);
    for (size_t i = 0; i < considered; ++i) {
        const FieldRow& r = rows[i];
        const FieldParams p = SanitizeField(r.entity, r.comp, r.pos);
        if (p.shape != kDisturbanceGroundLayer) {
            continue; // 立体は sub-07
        }
        DisturbanceLayerUpload& l = out.layers[out.layerCount];
        l.entityIndex = r.entity.index;
        l.entityGeneration = r.entity.generation;
        l.cellsX = p.cellsX;
        l.cellsZ = p.cellsZ;
        l.cellSize = p.cellSize;
        l.thickness = p.thickness;
        l.restDensity = p.restDensity;
        l.hasDensity = (p.channels & kDisturbanceChannelDensity) != 0;
        l.fogDensity = std::max(FiniteOr(r.comp.fogDensity, 0.0f), 0.0f);
        l.fogColor[0] = std::max(FiniteOr(r.comp.fogColor.x, 1.0f), 0.0f);
        l.fogColor[1] = std::max(FiniteOr(r.comp.fogColor.y, 1.0f), 0.0f);
        l.fogColor[2] = std::max(FiniteOr(r.comp.fogColor.z, 1.0f), 0.0f);
        l.fogBrightness = std::max(FiniteOr(r.comp.fogBrightness, 1.0f), 0.0f);

        const DisturbanceSlot* slot = nullptr;
        if (state != nullptr) {
            for (const DisturbanceSlot& s : state->Slots()) {
                if (s.entity == r.entity && SlotUsable(s, p)) {
                    slot = &s;
                    break;
                }
            }
        }
        if (slot != nullptr) {
            l.hasState = true;
            l.originCellX = slot->originCell[0];
            l.originCellZ = slot->originCell[2];
            l.baseY = slot->baseY;
            l.flowX = DataOrNull(slot->flowX);
            l.flowZ = DataOrNull(slot->flowZ);
            l.density = DataOrNull(slot->density);
            l.trail = DataOrNull(slot->trail);
            l.ripple = DataOrNull(slot->ripple);
        } else {
            // 最初の step が作る格子と同じ原点・基準の高さ (OriginCellOf は step と共有)
            l.hasState = false;
            l.originCellX = OriginCellOf(p.pos[0], p.cellSize, p.cellsX);
            l.originCellZ = OriginCellOf(p.pos[2], p.cellSize, p.cellsZ);
            l.baseY = p.pos[1];
        }
        ++out.layerCount;
    }
    out.serial = state != nullptr ? state->RenderSerial() : 0;
    return out.layerCount;
}

} // namespace mye
