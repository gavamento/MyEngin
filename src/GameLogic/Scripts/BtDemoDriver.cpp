//====================================================================================
//                          BtDemoDriver.cpp
//  MyEngin/ 秋田蓮音                                                     10/06/2026
//                                          --bt-demo のプレイヤー役を動かし、見張りの BT の段階をログに出す
//====================================================================================
// ビヘイビアツリーのショーケース (--bt-demo) のプレイヤー役に付き、リプレイ検証で毎回走る。
// 見張り A の巡回の視界へ入って見つかり、壁の向こうへ逃げて見失わせ、遠くで待つ。
// ★BtGetBlackboard / BtSetBlackboard の結果を sim 状態 (登録フィールド・見張り B の巡回ルート) に書き戻すのが本体 —
//   呼ぶだけでは ABI v27 の Debug/Release divergence を replay_verify が検知できない (NavDemoDriver と同じ流儀)。
// 僚機 B の巡回ルートだけ、BehaviorTreeComponent の Entity キーの初期値でなくここから BtSetBlackboard で渡す。
#include <cmath>

#include "Shared/ScriptAPI.h"

namespace {

// BB の Int キーを読む。キーが無い・未設定は fallback
int32_t ReadBbInt(const MyeUpdateContext& ctx, MyeEntityId owner, const char* key, int32_t fallback)
{
    MyeBbValue value = {};
    if (ctx.api->BtGetBlackboard(ctx.api->engine, owner, MyeNameHash(key), &value) == 0 || value.isSet == 0) {
        return fallback;
    }
    return value.i;
}

bool IsBbSet(const MyeUpdateContext& ctx, MyeEntityId owner, const char* key)
{
    MyeBbValue value = {};
    return ctx.api->BtGetBlackboard(ctx.api->engine, owner, MyeNameHash(key), &value) != 0 && value.isSet != 0;
}

} // namespace

struct BtDemoDriver : Script<BtDemoDriver> {
    // 見張り A の木が BB の stage へ書く値 (guard.bt.json の SetBlackboard と同じ)
    static constexpr int32_t kStagePatrol = 0;
    static constexpr int32_t kStageSpot = 1;
    static constexpr int32_t kStageChase = 2;
    static constexpr int32_t kStageSearch = 3;

    int32_t phase = 0;           // 0 = 接近 / 1 = 逃走 / 2 = 離れて待つ
    int32_t waypoint = 0;        // 逃走の何点目へ向かっているか
    int32_t routeAssigned = 0;   // 僚機 B の route を BtSetBlackboard で渡せたか
    int32_t guardStage = 0;      // 見張り A の stage (BtGetBlackboard の結果)
    int32_t hadTarget = 0;       // 見張り A の target が前 tick に設定済みだったか
    int32_t buddyStage = 0;      // 僚機 B の stage
    int32_t spotTick = -1;       // 各段階の開始 tick (-1 = まだ)
    int32_t chaseTick = -1;
    int32_t lostTick = -1;
    int32_t searchTick = -1;
    int32_t returnTick = -1;
    int32_t buddySearchTick = -1; // 僚機 B が知らせを受けて捜索に入った tick

    void Update(MyeUpdateContext& ctx)
    {
        constexpr float kCcHalfHeight = 0.9f;  // CharacterController の既定の高さの半分
        constexpr float kApproachSpeed = 4.0f; // m/s
        constexpr float kFleeSpeed = 5.5f;
        constexpr float kArrive = 0.3f;        // 点に着いたとみなす水平距離
        static constexpr MyeVec3 kBait = { -9.0f, 0.0f, -2.0f };
        static constexpr MyeVec3 kFlee[3] = { { -8.0f, 0.0f, 4.0f }, { 6.0f, 0.0f, 8.0f }, { 12.0f, 0.0f, 12.0f } };

        const MyeEntityId guardA = ctx.api->FindByName(ctx.api->engine, "Guard A");
        const MyeEntityId guardB = ctx.api->FindByName(ctx.api->engine, "Guard B");
        const MyeEntityId routeB = ctx.api->FindByName(ctx.api->engine, "Route B");

        // 僚機 B の巡回ルートを ABI で渡す。木のインスタンスができる前は 0 が返るので、渡せるまで毎 tick 試す
        if (routeAssigned == 0 && !MyeEntityIdIsNull(guardB) && !MyeEntityIdIsNull(routeB)) {
            MyeBbValue route = {};
            route.type = MYE_BB_ENTITY;
            route.isSet = 1;
            route.entity = routeB;
            if (ctx.api->BtSetBlackboard(ctx.api->engine, guardB, MyeNameHash("route"), &route) != 0) {
                routeAssigned = 1;
                MyeLogf(ctx, "[bt-demo] tick %llu: Guard B route assigned via BtSetBlackboard",
                        static_cast<unsigned long long>(ctx.tickIndex));
            }
        }

        // 見張り A の段階。stage が変わった tick を覚えてログに出す (BT の状態遷移)
        const int32_t tick = static_cast<int32_t>(ctx.tickIndex);
        const bool hasTarget = IsBbSet(ctx, guardA, "target");
        const int32_t stage = ReadBbInt(ctx, guardA, "stage", kStagePatrol);
        if (stage != guardStage) {
            if (stage == kStageSpot && spotTick < 0) {
                spotTick = tick;
                MyeLogf(ctx, "[bt-demo] tick %d: spot (Guard A noticed the player, alerts Guard B)", tick);
            } else if (stage == kStageChase && chaseTick < 0) {
                chaseTick = tick;
                MyeLogf(ctx, "[bt-demo] tick %d: chase", tick);
            } else if (stage == kStageSearch && searchTick < 0) {
                searchTick = tick;
                MyeLogf(ctx, "[bt-demo] tick %d: search", tick);
            } else if (stage == kStagePatrol && searchTick >= 0 && returnTick < 0) {
                returnTick = tick;
                MyeLogf(ctx, "[bt-demo] tick %d: back to patrol", tick);
            }
            guardStage = stage;
        }
        // 見失った = 追跡が始まった後で target が外れた tick (捜索の開始と同じ tick か直前)
        if (hadTarget != 0 && !hasTarget && lostTick < 0 && chaseTick >= 0) {
            lostTick = tick;
            MyeLogf(ctx, "[bt-demo] tick %d: lost sight", tick);
        }
        hadTarget = hasTarget ? 1 : 0;
        const int32_t buddy = ReadBbInt(ctx, guardB, "stage", kStagePatrol);
        if (buddy != buddyStage) {
            if (buddy == kStageSearch && buddySearchTick < 0) {
                buddySearchTick = tick;
                MyeLogf(ctx, "[bt-demo] tick %d: Guard B searches (GuardAlert event)", tick);
            }
            buddyStage = buddy;
        }

        // プレイヤー役の動き。見張りに見つかったら逃げ出す
        MyeVec3 p = {};
        ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
        const MyeVec3 feet = { p.x, p.y - kCcHalfHeight, p.z };
        if (phase == 0 && stage != kStagePatrol) {
            phase = 1;
        }
        MyeVec3 goal = kBait;
        float speed = kApproachSpeed;
        if (phase == 1) {
            goal = kFlee[waypoint];
            speed = kFleeSpeed;
        }
        const float dx = goal.x - feet.x;
        const float dz = goal.z - feet.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (phase == 1 && d <= kArrive) {
            if (waypoint + 1 < 3) {
                ++waypoint;
            } else {
                phase = 2;
            }
        }
        const bool moving = phase != 2 && d > kArrive;
        const MyeVec3 move = moving ? MyeVec3{ dx / d * speed, 0.0f, dz / d * speed } : MyeVec3{};
        ctx.api->CharacterMove(ctx.api->engine, ctx.self, move);
    }
};
REGISTER_SCRIPT(BtDemoDriver,
                FIELDS(MYE_F_JP(phase, "段階"), waypoint, MYE_F_JP(routeAssigned, "B のルートを渡した"),
                       MYE_F_JP(guardStage, "A の stage"), hadTarget, MYE_F_JP(buddyStage, "B の stage"), spotTick,
                       chaseTick, lostTick, searchTick, returnTick, buddySearchTick));
