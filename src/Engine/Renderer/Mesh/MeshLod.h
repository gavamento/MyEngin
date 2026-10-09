//====================================================================================
//                          MeshLod.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          メッシュ LOD の段の選択 (screen-size とヒステリシス)
//====================================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

// 段を切り替える境目の幅 (しきい値の割合)。上げる側は 1+h 倍、下げる側は 1-h 倍で判定する
inline constexpr float kLodHysteresis = 0.1f;

struct LodSelectParams {
    float lodBias = 1.0f;     // > 1 で詳細な段を長く使う (Unity の QualitySettings.lodBias と同じ向き)
    int32_t forcedLod = -1;   // -1 = 自動 / 0.. = その段 (無ければ最も粗い段)
};

// 外接球が画面の高さに占める割合。projScaleY = proj._22 (透視は 1/tan(fovY/2)、正射影は 2/高さ)
inline float LodScreenSize(float radius, float distance, float projScaleY, bool orthographic)
{
    if (orthographic) {
        return radius * projScaleY;
    }
    return radius * projScaleY / std::max(distance, 1e-4f);
}

// ワールド AABB の外接球が画面の高さに占める割合。LOD の段選択と URO (アニメ間引き) が共有する
inline float BoxScreenSize(const DirectX::XMFLOAT3& lo, const DirectX::XMFLOAT3& hi,
                           const DirectX::XMFLOAT3& cameraPos, float projScaleY, bool orthographic)
{
    const float cx = (lo.x + hi.x) * 0.5f - cameraPos.x;
    const float cy = (lo.y + hi.y) * 0.5f - cameraPos.y;
    const float cz = (lo.z + hi.z) * 0.5f - cameraPos.z;
    const float dx = hi.x - lo.x;
    const float dy = hi.y - lo.y;
    const float dz = hi.z - lo.z;
    const float radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    return LodScreenSize(radius, std::sqrt(cx * cx + cy * cy + cz * cz), projScaleY, orthographic);
}

// 描く段を決める純関数。lods = Mesh::lods (LOD0 を含む)。prevLod < 0 = 履歴なし (しきい値だけで決める)。
// 段 k (>= 1) は screenSize * lodBias が lods[k].screenSize を下回ると選ばれる
inline uint32_t SelectLod(const std::vector<MeshLodLevel>& lods, float screenSize, int32_t prevLod,
                          const LodSelectParams& params)
{
    const uint32_t count = static_cast<uint32_t>(lods.size());
    if (count <= 1) {
        return 0;
    }
    if (params.forcedLod >= 0) {
        return std::min(static_cast<uint32_t>(params.forcedLod), count - 1);
    }
    const float size = screenSize * params.lodBias;
    uint32_t stage = 0;
    if (prevLod < 0 || prevLod >= static_cast<int32_t>(count)) {
        while (stage + 1 < count && size < lods[stage + 1].screenSize) {
            ++stage;
        }
        return stage;
    }
    stage = static_cast<uint32_t>(prevLod);
    while (stage + 1 < count && size < lods[stage + 1].screenSize * (1.0f - kLodHysteresis)) {
        ++stage;
    }
    while (stage > 0 && size >= lods[stage].screenSize * (1.0f + kLodHysteresis)) {
        --stage;
    }
    return stage;
}

// ビュー(viewKey)ごとの前フレームの段。エンティティの index で引き、世代が違えば履歴なし。
// 書き込みは直列の段だけで行い、並列の段は Get だけを呼ぶ
class LodHistory {
public:
    int32_t Get(EntityID e) const
    {
        if (e.index >= stage_.size() || generation_[e.index] != e.generation + 1) {
            return -1;
        }
        return stage_[e.index];
    }
    void Set(EntityID e, uint32_t lod)
    {
        if (e.index >= stage_.size()) {
            stage_.resize(static_cast<size_t>(e.index) + 1, 0);
            generation_.resize(static_cast<size_t>(e.index) + 1, 0);
        }
        stage_[e.index] = static_cast<uint8_t>(lod);
        generation_[e.index] = e.generation + 1;
    }
    void Clear()
    {
        stage_.clear();
        generation_.clear();
    }

private:
    std::vector<uint8_t> stage_;
    std::vector<uint32_t> generation_; // entity.generation + 1 (0 = 無効スロット)
};

} // namespace mye
