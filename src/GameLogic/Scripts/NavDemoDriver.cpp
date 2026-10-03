//====================================================================================
//                          NavDemoDriver.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          --nav-demo の Agent の目的地を tick で切り替える (汎用フィールド ABI の probe)
//====================================================================================
// ナビメッシュのショーケース (--nav-demo) の NavMeshAgent に付き、リプレイ検証で毎回走る。
// 出発点を覚え、固定 tick で目的地をそこへ戻す。
// ★GetComponentField / SetComponentField の結果を sim 状態 (NavMeshAgent.destination + 登録フィールド) に
//   書き戻すのが本体 — 呼ぶだけでは汎用 ABI の Debug/Release divergence を replay_verify が検知できない
//   (SchemaHealthDemo と同じ流儀)。
#include "Shared/ScriptAPI.h"

struct NavDemoDriver : Script<NavDemoDriver> {
    MyeVec3 home = {};          // 出発点 (足元)
    int32_t switched = 0;       // 目的地を書き換えた回数 (被覆の本体)
    float lastRemaining = 0.0f; // 最後に読んだ NavMeshAgent.remainingDistance
    bool homeSet = false;

    void Update(MyeUpdateContext& ctx)
    {
        // CharacterController の既定の高さ 1.8 の半分。デモの Agent は既定値のまま
        constexpr float kCapsuleHalfHeight = 0.9f;
        constexpr uint64_t kSwitchTick = 300;
        constexpr uint64_t comp = MyeNameHash("NavMeshAgent");
        constexpr uint64_t fDestination = MyeNameHash("destination");
        constexpr uint64_t fRemaining = MyeNameHash("remainingDistance");

        if (!homeSet) {
            MyeVec3 p = {};
            ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
            home = { p.x, p.y - kCapsuleHalfHeight, p.z };
            homeSet = true;
        }
        float remaining = 0.0f;
        if (MyeGetField(ctx, ctx.self, comp, fRemaining, remaining)) {
            lastRemaining = remaining;
        }
        if (ctx.tickIndex == kSwitchTick && MyeSetField(ctx, ctx.self, comp, fDestination, home)) {
            ++switched;
        }
    }
};
REGISTER_SCRIPT(NavDemoDriver,
                FIELDS(MYE_F_JP(switched, "切り替え回数"), MYE_F_JP(lastRemaining, "直近の残り距離"), home, homeSet));
