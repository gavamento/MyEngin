//====================================================================================
//                          NavModifierDriver.cpp
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          --nav-demo の NavMeshModifier を tick で動かし、エリアを切り替える
//====================================================================================
// Modifier を動かし (= 外して付け直す)、歩行不可にし、元のエリアへ戻す。結果は NavMeshModifier の登録フィールドと
// Transform に書き戻る sim 状態で、リプレイ検証が Debug / Release の TileCache 更新 (エリアの塗り替え) の一致を毎回確かめる。
#include "Shared/ScriptAPI.h"

struct NavModifierDriver : Script<NavModifierDriver> {
    int32_t switched = 0; // 動かした・切り替えた回数 (被覆の本体)

    void Update(MyeUpdateContext& ctx)
    {
        constexpr uint64_t kMoveTick = 120;     // 別の場所へ動かす
        constexpr uint64_t kBlockTick = 240;    // 歩行不可にする (= 切り抜きと同じ結果)
        constexpr uint64_t kRestoreTick = 330;  // 元のエリアへ戻す
        constexpr float kMoveZ = -5.0f;
        constexpr int32_t kBlockedArea = 1;
        constexpr int32_t kRegularArea = 3;
        constexpr uint64_t comp = MyeNameHash("NavMeshModifier");
        constexpr uint64_t fArea = MyeNameHash("area");

        if (ctx.tickIndex == kMoveTick) {
            MyeVec3 p = {};
            ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
            p.z += kMoveZ;
            ctx.api->SetLocalPosition(ctx.api->engine, ctx.self, p);
            ++switched;
        } else if (ctx.tickIndex == kBlockTick) {
            if (MyeSetField(ctx, ctx.self, comp, fArea, kBlockedArea)) {
                ++switched;
            }
        } else if (ctx.tickIndex == kRestoreTick) {
            if (MyeSetField(ctx, ctx.self, comp, fArea, kRegularArea)) {
                ++switched;
            }
        }
    }
};
REGISTER_SCRIPT(NavModifierDriver, FIELDS(MYE_F_JP(switched, "動かした・切り替えた回数")));
