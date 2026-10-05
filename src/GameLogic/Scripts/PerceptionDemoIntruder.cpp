//====================================================================================
//                          PerceptionDemoIntruder.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          --perception-demo の侵入者を四角く歩かせ、足音と攻撃を知らせる
//====================================================================================
// 知覚のショーケース (--perception-demo) の侵入者に付き、リプレイ検証で毎回走る。
// 4 隅を順に CharacterMove で歩き、一定間隔で足音を PerceptionReportNoise、tick 360 に Guard C へ
// PerceptionReportDamage を出す。★戻り値 (聞こえた数・届いたか) を登録フィールドへ書き戻すのが本体 —
// 呼ぶだけでは ABI v25 の Debug/Release divergence を replay_verify が検知できない (NavDemoDriver と同じ流儀)
#include <cmath>

#include "Shared/ScriptAPI.h"

struct PerceptionDemoIntruder : Script<PerceptionDemoIntruder> {
    int32_t corner = 0;         // 次に向かう隅 (0..3)
    int32_t noises = 0;         // 鳴らした足音の数
    int32_t heardTotal = 0;     // 足音を聞いた見張りの延べ数 (PerceptionReportNoise の戻り値の和)
    int32_t damageReported = 0; // 攻撃が届いたか (PerceptionReportDamage の戻り値)

    void Update(MyeUpdateContext& ctx)
    {
        constexpr float kSpeed = 2.5f;         // 歩く速さ (m/s)
        constexpr float kArrive = 0.25f;       // 隅に着いたとみなす水平距離
        constexpr float kCcHalfHeight = 0.9f;  // CharacterController の既定の高さの半分
        constexpr uint64_t kStepEvery = 45;    // 足音の間隔 (tick)
        constexpr float kStepLoudness = 1.0f;
        constexpr float kStepRange = 14.0f;    // 足音が届く距離
        constexpr uint64_t kAttackTick = 360;
        constexpr float kAttackDamage = 10.0f;
        static constexpr MyeVec3 kCorners[4] = {
            { -8.0f, 0.0f, -8.0f }, { 8.0f, 0.0f, -8.0f }, { 8.0f, 0.0f, 8.0f }, { -8.0f, 0.0f, 8.0f }
        };

        MyeVec3 p = {};
        ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
        const MyeVec3 feet = { p.x, p.y - kCcHalfHeight, p.z };

        // 隅へ向かう。着いたら次の隅 (最初の隅は出発点なので 1 から回る)
        const int32_t next = (corner + 1) % 4;
        const float dx = kCorners[next].x - feet.x;
        const float dz = kCorners[next].z - feet.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d <= kArrive) {
            corner = next;
        }
        const MyeVec3 move = d > kArrive ? MyeVec3{ dx / d * kSpeed, 0.0f, dz / d * kSpeed } : MyeVec3{};
        ctx.api->CharacterMove(ctx.api->engine, ctx.self, move);

        if (ctx.tickIndex % kStepEvery == 0) {
            ++noises;
            heardTotal += MyePerceptionReportNoise(ctx, feet, kStepLoudness, kStepRange, ctx.self);
        }
        if (ctx.tickIndex == kAttackTick) {
            const MyeEntityId guardC = ctx.api->FindByName(ctx.api->engine, "Guard C");
            damageReported = MyePerceptionReportDamage(ctx, guardC, ctx.self, kAttackDamage, feet) ? 1 : 0;
        }
    }
};
REGISTER_SCRIPT(PerceptionDemoIntruder,
                FIELDS(MYE_F_JP(corner, "次の隅"), MYE_F_JP(noises, "足音の数"), MYE_F_JP(heardTotal, "聞かれた延べ数"),
                       MYE_F_JP(damageReported, "攻撃が届いた")));
