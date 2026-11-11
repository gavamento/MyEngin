//====================================================================================
//                          DistanceCull.h
//  MyEngin/ 秋田蓮音                                                       11/11/2026
//                                          距離カリングの判定 (実効距離・AABB までの距離・掛けるビュー) の純関数
//====================================================================================
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Scene/Tags.h"

// 描画専用の判定で、ワールドハッシュには関与しない。正本の仕様は plans\m99-culling\spec.md 4.1.3
namespace mye::distancecull {

// 実効距離 = {物ごと, 物が持つ各タグ, プロジェクト既定} のうち 0 より大きい最小。0 = 距離では落とさない。
// tagTable は 64 要素 (タグ番号 → 距離。0 = 設定なし)、tagMask の bit i = タグ i
inline float EffectiveMaxDrawDistance(float perObject, uint64_t tagMask, const float* tagTable, float projectDefault)
{
    float best = 0.0f;
    const auto take = [&best](float d) {
        if (d > 0.0f && (best == 0.0f || d < best)) {
            best = d;
        }
    };
    take(perObject);
    take(projectDefault);
    while (tagMask != 0) {
        take(tagTable[std::countr_zero(tagMask)]);
        tagMask &= tagMask - 1;
    }
    return best;
}

// 物 e の実効距離。タグは自分 + 祖先の OR (Tags::EffectiveMask と同じ継承)、RenderCulling は自分 → 祖先の順で
// 最初に見つかった 1 個の maxDrawDistance を使う (子の物が祖先の値を丸ごと上書きする)。
// useTags = false ならタグは引かない。exemptFromSizeCulling は同じ 1 個から outExempt へ (null なら無視)。
// 祖先を辿るので、呼び出し側は RenderCulling もタグ別の距離も無いときは呼ばず projectDefault をそのまま使う
inline float ResolveMaxDrawDistance(World& world, EntityID e, bool useTags, const float* tagTable,
                                    float projectDefault, bool* outExempt = nullptr)
{
    float perObject = 0.0f;
    bool exempt = false;
    bool found = false;
    for (EntityID cur = e; !found && !cur.IsNull(); cur = world.GetParent(cur)) {
        if (const auto* rc = world.GetComponent<RenderCullingComponent>(cur)) {
            perObject = rc->maxDrawDistance;
            exempt = rc->exemptFromSizeCulling;
            found = true;
        }
    }
    const uint64_t tagMask = useTags ? Tags::EffectiveMask(world, e) : 0;
    if (outExempt != nullptr) {
        *outExempt = exempt;
    }
    return EffectiveMaxDrawDistance(perObject, tagMask, tagTable, projectDefault);
}

// 点からワールド AABB の最近点までの距離。点が箱の内側なら 0
inline float DistanceToAabb(const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& lo, const DirectX::XMFLOAT3& hi)
{
    const float dx = std::max({ lo.x - p.x, 0.0f, p.x - hi.x });
    const float dy = std::max({ lo.y - p.y, 0.0f, p.y - hi.y });
    const float dz = std::max({ lo.z - p.z, 0.0f, p.z - hi.z });
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// maxDistance <= 0 は判定しない。境界ちょうど (d == maxDistance) は残す
inline bool ExceedsMaxDistance(const DirectX::XMFLOAT3& cameraPos, const DirectX::XMFLOAT3& lo,
                               const DirectX::XMFLOAT3& hi, float maxDistance)
{
    return maxDistance > 0.0f && DistanceToAabb(cameraPos, lo, hi) > maxDistance;
}

// このビューに距離・画面サイズのカリングを掛けるか。viewKey 1 = Runtime、3 = Game View は常に、
// 2 = Scene View は sceneViewToo (セッション限りのトグル) のときだけ、0 = プレビュー・プローブ焼きは掛けない
inline bool AppliesToView(uint32_t viewKey, bool sceneViewToo)
{
    return viewKey == 1 || viewKey == 3 || (viewKey == 2 && sceneViewToo);
}

// 画面サイズカリングのヒステリシス幅 (閾値の 10%。LOD と同じ)
inline constexpr float kSizeCullHysteresis = 0.1f;

// 画面サイズで落とすか (spec 4.1.4)。screenSize = BoxScreenSize (lodBias なし)。prevCulled > 0 = 前フレームに
// このビューで画面サイズにより落とした (履歴なしは 0 以下)。落とした物は閾値 × 1.1 を超えるまで落としたまま。
// minSize <= 0・除外・箱を持てない物は落とさない
inline bool CullsByScreenSize(float screenSize, float minSize, int32_t prevCulled, bool exempt, bool alwaysVisible)
{
    if (minSize <= 0.0f || exempt || alwaysVisible) {
        return false;
    }
    const float limit = prevCulled > 0 ? minSize * (1.0f + kSizeCullHysteresis) : minSize;
    return screenSize < limit;
}

} // namespace mye::distancecull
