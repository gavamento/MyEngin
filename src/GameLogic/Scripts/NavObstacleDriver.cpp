//====================================================================================
//                          NavObstacleDriver.cpp
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          --nav-demo の NavMeshObstacle を tick で出し入れ・移動する
//====================================================================================
// NavMeshObstacle の carve を倒して撤去し、位置を変えて carve を戻す (= 追加)。
// 目的地の書き換えと同じく、結果は NavMeshObstacle の登録フィールドと Transform に書き戻る sim 状態で、
// リプレイ検証が Debug / Release の TileCache 更新 (追加・撤去・Commit) の一致を毎回確かめる。
#include "Shared/ScriptAPI.h"

struct NavObstacleDriver : Script<NavObstacleDriver> {
    int32_t switched = 0; // 出し入れ・移動した回数 (被覆の本体)

    void Update(MyeUpdateContext& ctx)
    {
        constexpr uint64_t kRemoveTick = 150; // carve を倒す = 障害物が外れる
        constexpr uint64_t kReturnTick = 210; // 別の場所へ動かして carve を立てる = 障害物が付く
        constexpr float kMoveZ = 5.0f;
        constexpr uint64_t comp = MyeNameHash("NavMeshObstacle");
        constexpr uint64_t fCarve = MyeNameHash("carve");

        if (ctx.tickIndex == kRemoveTick) {
            const bool off = false;
            if (MyeSetField(ctx, ctx.self, comp, fCarve, off)) {
                ++switched;
            }
        } else if (ctx.tickIndex == kReturnTick) {
            MyeVec3 p = {};
            ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
            p.z += kMoveZ;
            ctx.api->SetLocalPosition(ctx.api->engine, ctx.self, p);
            const bool on = true;
            if (MyeSetField(ctx, ctx.self, comp, fCarve, on)) {
                ++switched;
            }
        }
    }
};
REGISTER_SCRIPT(NavObstacleDriver, FIELDS(MYE_F_JP(switched, "出し入れ・移動した回数")));
