//====================================================================================
//                          RtScope.h
//  MyEngin/ 秋田蓮音                                                       09/30/2026
//                                          RT の適用範囲の判定 (描画 / Inspector 共通)
//====================================================================================
#pragma once
#include <cstdint>

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Scene/TagNames.h"

namespace mye {

class World;

// どこで決まったか (Inspector が「実効: ON (親 X の設定)」のように根拠を出すため)
enum class RtScopeSource : uint8_t {
    Default,  // どの設定にも当たらなかった = 既定 OFF
    Explicit, // RayTracingComponent の ON/OFF (自分または祖先。from が持ち主)
    Tag,      // タグ規則 (tagIndex が決め手のタグ)
};

struct RtScopeLane {
    bool on = false;
    RtScopeSource source = RtScopeSource::Default;
    EntityID from = kNullEntity; // Explicit のときの持ち主
    int32_t tagIndex = -1;       // Tag のときの決め手 (該当が複数なら最小番号)
};

struct RtScope {
    RtScopeLane inScene;  // BVH に入る (反射に映る / RT の影を落とす)
    RtScopeLane receiver; // RT の GI / 影 / 反射を受ける
};

// RT の適用範囲の判定。**規則はここの 1 本きり** — 描画 (RenderSystem) と Inspector が同じ
// 関数を見ることで「エディタの表示」と「実際の絵」が食い違わないようにする (Tags.h と同じ流儀)。
// レーンごとに:
//   1. 自分 → 祖先の順に RayTracingComponent を見て、最初の ON/OFF (継承は素通り)
//   2. どこにも無ければタグ規則。タグは自分 + 祖先の OR (Tags::EffectiveMask と同じ継承)。
//      OFF 規則に当たれば OFF (一括除外を確実に効かせるため ON より強い)、次に ON 規則
//   3. どれにも当たらなければ既定 OFF
// ★個別設定をタグ規則より強くしたのは、「背景タグは全部 ON、ただしこの 1 個だけ OFF」の
//   ような例外を物に直接書けるようにするため
RtScope ResolveRtScope(World& world, EntityID e, const RtTagRules& rules);

} // namespace mye
