//====================================================================================
//                          FractureDamageProbe.cpp
//  MyEngin/ 秋田蓮音                                                     09/26/2026
//                                          ABI v22 (ApplyFractureDamage/onBreak) の恒久 probe
//====================================================================================
// 破壊ショーケース (--fracture-demo) の固定壁 (FractureWall) に付き、着弾より先に
// 自分自身をスクリプトから割る。onBreak を受けた回数と荷重を sim 状態 (登録フィールド)
// へ書き戻すのが本体 — 呼ぶだけでは ABI v22 の Debug/Release divergence を
// replay_verify が検知できない (SchemaHealthDemo / PartRaycastDemo と同じ流儀)。
#include "Shared/ScriptAPI.h"

struct FractureDamageProbe : Script<FractureDamageProbe> {
    int32_t brokenCount = 0;  // onBreak を受けた回数 (被覆の本体)
    float lastImpulse = 0.0f; // 直近の onBreak が渡した荷重 [N]
    bool fired = false;       // ApplyFractureDamage を撃ったか (1 回だけにする)

    void Update(MyeUpdateContext& ctx)
    {
        // 着弾 (FractureBallWall、tick 30 前後) より先に割る
        constexpr uint64_t kFireTick = 5;
        if (fired || ctx.tickIndex != kFireTick) {
            return;
        }
        fired = true;
        MyeVec3 point{};
        ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &point);
        ctx.api->ApplyFractureDamage(ctx.api->engine, ctx.self, point, 0.0f, 10000.0f);
    }

    void OnBreak(MyeUpdateContext& ctx, MyeEntityId piece, MyeVec3 point, float impulse)
    {
        (void)ctx;
        (void)piece;
        (void)point;
        ++brokenCount;
        lastImpulse = impulse;
    }
};
REGISTER_SCRIPT(FractureDamageProbe,
                FIELDS(MYE_F_JP(brokenCount, "破断回数"), MYE_F_JP(lastImpulse, "直近の荷重"),
                       fired));
