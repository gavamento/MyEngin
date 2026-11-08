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
#include "Engine/Renderer/Passes/DisturbanceVolumePass.h"

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
           && s.channels == p.channels && s.cellSize == p.cellSize && s.columnBaseY.empty() == !p.followTerrain;
}

// 列の基準高さ - baseY の上限 [m] (half で表せる範囲に収める)
constexpr float kMaxBaseOffset = 1000.0f;

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
            continue; // 立体は BuildDisturbanceVolumeUpload
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
        l.fogSunLight = r.comp.fogSunLight;
        l.fogLocalLights = r.comp.fogLocalLights;

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
            if (!slot->columnBaseY.empty()) {
                std::vector<float>& offset = out.baseOffsetStorage[out.layerCount];
                offset.resize(slot->columnBaseY.size());
                for (size_t k = 0; k < offset.size(); ++k) {
                    offset[k] = std::min(std::max(slot->columnBaseY[k] - slot->baseY, -kMaxBaseOffset), kMaxBaseOffset);
                }
                l.baseOffset = offset.data();
            }
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

int32_t BuildDisturbanceVolumeUpload(World& world, const DisturbanceState* state, DisturbanceVolumeUpload& out)
{
    out = DisturbanceVolumeUpload{};
    std::vector<FieldRow> rows;
    CollectFields(world, rows);
    const size_t considered = std::min<size_t>(rows.size(), kMaxDisturbanceFields);
    for (size_t i = 0; i < considered; ++i) {
        const FieldRow& r = rows[i];
        const FieldParams p = SanitizeField(r.entity, r.comp, r.pos);
        if (p.shape != kDisturbanceVolume) {
            continue;
        }
        DisturbanceVolumeEntryUpload& v = out.volumes[out.count];
        v.entityIndex = r.entity.index;
        v.entityGeneration = r.entity.generation;
        v.cellsX = p.cellsX;
        v.cellsY = p.cellsY;
        v.cellsZ = p.cellsZ;
        v.cellSize = p.cellSize;
        v.restDensity = p.restDensity;
        v.hasDensity = (p.channels & kDisturbanceChannelDensity) != 0;
        v.fogDensity = std::max(FiniteOr(r.comp.fogDensity, 0.0f), 0.0f);
        v.fogColor[0] = std::max(FiniteOr(r.comp.fogColor.x, 1.0f), 0.0f);
        v.fogColor[1] = std::max(FiniteOr(r.comp.fogColor.y, 1.0f), 0.0f);
        v.fogColor[2] = std::max(FiniteOr(r.comp.fogColor.z, 1.0f), 0.0f);
        v.fogBrightness = std::max(FiniteOr(r.comp.fogBrightness, 1.0f), 0.0f);
        v.fogSunLight = r.comp.fogSunLight;
        v.autoFroxel = r.comp.autoFroxel;

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
            v.hasState = true;
            for (int k = 0; k < 3; ++k) {
                v.originCell[k] = slot->originCell[k];
            }
            v.flowX = DataOrNull(slot->flowX);
            v.flowY = DataOrNull(slot->flowY);
            v.flowZ = DataOrNull(slot->flowZ);
            v.density = DataOrNull(slot->density);
        } else {
            // 最初の step が作る格子と同じ原点 (OriginCellOf は step と共有)
            v.hasState = false;
            v.originCell[0] = OriginCellOf(p.pos[0], p.cellSize, p.cellsX);
            v.originCell[1] = OriginCellOf(p.pos[1], p.cellSize, p.cellsY);
            v.originCell[2] = OriginCellOf(p.pos[2], p.cellSize, p.cellsZ);
        }
        ++out.count;
    }
    out.serial = state != nullptr ? state->RenderSerial() : 0;
    return out.count;
}

} // namespace mye
