/*----
 CameraSelect.h  描画するカメラの選択規則 (RenderSystem / PixelPerfect / UILayout の共通)
 作成者: 秋田蓮音                                11/11/2026
----*/
#pragma once
#include <cstdint>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"

namespace mye::camselect {

// 走査順で最初のカメラを仮に選び、isPrimary のカメラが出たらそれに替える。
// ★アーキタイプ内の return はそのアーキタイプの走査だけを打ち切るので、後のアーキタイプの
//   isPrimary はさらに上書きする (従来の 3 か所と同じ結果。変えると既存シーンの絵が変わる)。
// CameraOwner (M101) を持つカメラは owner.lane == viewLane のときだけ候補になる。
// 持たないカメラは全レーンの候補 (既存シーンは何も変わらない)。
// viewLane は描画側ではこのマシンのレーン (NetLocalPlayer)。sim 側 (UILayout のヒットテスト) は
// マシンで値が変わると分岐するので常に 0 を渡す。
// pick(Archetype&, uint32_t row) は選択が更新されるたびに呼ばれる (最後の呼び出しが結果)。
template <class Pick>
inline bool ForEachCameraPick(World& world, uint32_t viewLane, Pick&& pick)
{
    bool found = false;
    const ComponentTypeId req[] = { CameraComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(CameraComponent::sTypeId);
        const int oi = arch.FindTypeIndex(CameraOwnerComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (oi >= 0) {
                const auto* o = static_cast<const CameraOwnerComponent*>(arch.GetPtr(oi, row));
                if (o->lane < 0 || static_cast<uint32_t>(o->lane) != viewLane) {
                    continue;
                }
            }
            const auto* c = static_cast<const CameraComponent*>(arch.GetPtr(ci, row));
            if (!found || c->isPrimary) {
                pick(arch, row);
                found = true;
                if (c->isPrimary) {
                    return;
                }
            }
        }
    });
    return found;
}

} // namespace mye::camselect
