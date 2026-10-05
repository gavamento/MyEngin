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
    // ABI v24 (Nav*) の結果。tick 120 に 1 回だけ引いて sim 状態へ書き戻す (呼ぶだけでは replay が割れを検知できない)
    int32_t cornerCount = 0;   // NavFindPath の角の数
    int32_t pathPartial = 0;   // 同・届く限りの最寄りまでか
    float sampleY = 0.0f;      // NavSamplePosition が返した点の高さ
    int32_t rayHit = 0;        // NavRaycast が壁で止まったか
    MyeVec3 randomPoint = {};  // NavFindRandomPoint の点 (World の RNG)
    int32_t stateStatus = -1;  // NavGetAgentState の status
    // ABI v26 (M84d2) の結果。120 tick の撮影 (shot_verify の nav) より後で呼び、同じく sim 状態へ書き戻す
    int32_t filteredCorners = 0; // NavFindPathFiltered (navFilter 0) の角の数 = cornerCount と同じはず
    int32_t calcStatus = -1;     // NavCalculatePath の status
    int32_t calcCorners = 0;     // 同・角の数
    int32_t setPathOk = 0;       // NavSetPath を受け付けたか
    int32_t warpOk = 0;          // NavWarp を受け付けたか

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

        constexpr uint64_t kNavApiTick = 120;
        constexpr uint32_t kAllAreas = 0xFFFFFFFFu;
        if (ctx.tickIndex == kNavApiTick) {
            MyeVec3 p = {};
            ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
            const MyeVec3 feet = { p.x, p.y - kCapsuleHalfHeight, p.z };
            const MyeVec3 farCorner = { 10.0f, 0.0f, -9.0f };
            MyeVec3 corners[32] = {};
            bool partial = false;
            cornerCount = MyeNavFindPath(ctx, 0, feet, farCorner, kAllAreas, corners, 32, partial);
            pathPartial = partial ? 1 : 0;
            MyeVec3 sample = {};
            if (MyeNavSamplePosition(ctx, 0, { 0.0f, 5.0f, 0.0f }, { 1.0f, 6.0f, 1.0f }, kAllAreas, sample)) {
                sampleY = sample.y;
            }
            MyeNavRaycastHit hit = {};
            if (MyeNavRaycast(ctx, 0, feet, farCorner, kAllAreas, hit)) {
                rayHit = hit.hit;
            }
            MyeNavFindRandomPoint(ctx, 0, feet, 3.0f, kAllAreas, randomPoint);
            MyeNavAgentState state = {};
            if (MyeNavGetAgentState(ctx, ctx.self, state)) {
                stateStatus = state.status;
            }
            filteredCorners = MyeNavFindPathFiltered(ctx, 0, feet, farCorner, kAllAreas, 0, corners, 32, partial);
        }

        // v26: 細かい制御 (汎用フィールド) と経路の受け渡し・瞬間移動。Agent ごとに出発点の z で値を変える
        constexpr uint64_t fPriority = MyeNameHash("avoidancePriority");
        constexpr uint64_t fStopped = MyeNameHash("isStopped");
        if (ctx.tickIndex == 150) {
            const int32_t priority = static_cast<int32_t>(home.z + 10.0f) * 5;
            MyeSetField(ctx, ctx.self, comp, fPriority, priority);
        }
        if (ctx.tickIndex == 180) {
            MyeNavPath path = {};
            if (MyeNavCalculatePath(ctx, ctx.self, { 10.0f, 0.0f, -9.0f }, path)) {
                calcStatus = path.status;
                calcCorners = path.cornerCount;
                setPathOk = MyeNavSetPath(ctx, ctx.self, path) ? 1 : 0;
            }
        }
        if (ctx.tickIndex == 240 || ctx.tickIndex == 270) {
            MyeSetField(ctx, ctx.self, comp, fStopped, ctx.tickIndex == 240);
        }
        if (ctx.tickIndex == 420) {
            warpOk = MyeNavWarp(ctx, ctx.self, { home.x + 1.0f, home.y, home.z }) ? 1 : 0;
        }
        // 同じ値の再設定は経路を引き直さない (汎用フィールドの書き込みと同じ結果になるはず)
        if (ctx.tickIndex == kSwitchTick) {
            MyeNavSetDestination(ctx, ctx.self, home);
        }
    }
};
REGISTER_SCRIPT(NavDemoDriver,
                FIELDS(MYE_F_JP(switched, "切り替え回数"), MYE_F_JP(lastRemaining, "直近の残り距離"), home, homeSet,
                       cornerCount, pathPartial, sampleY, rayHit, randomPoint, stateStatus, filteredCorners, calcStatus,
                       calcCorners, setPathOk, warpOk));
