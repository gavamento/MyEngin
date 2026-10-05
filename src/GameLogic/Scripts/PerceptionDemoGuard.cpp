//====================================================================================
//                          PerceptionDemoGuard.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          --perception-demo の見張りを知覚した相手の方へ振り向かせる
//====================================================================================
// 知覚のショーケース (--perception-demo) の見張り (AIPerception) に付き、リプレイ検証で毎回走る。
// PerceptionGet で知覚している相手を読み、今知覚している相手 (無ければ最も新しい記憶の予測位置) の方へ
// 少しずつ向きを変える。向き (LocalTransform の回転) と読んだ結果を登録フィールドへ書き戻すので、
// 知覚の結果と ABI v25 の Debug/Release divergence を replay_verify が検知できる。
// ★三角関数を使わない (平方根だけ): 向きは水平の単位ベクトルで持ち、回転は半角の公式で作る —
//   GameLogic.dll の構成ごとに sin / cos の実装が変わっても sim の値が割れないように
#include <cmath>

#include "Shared/ScriptAPI.h"

struct PerceptionDemoGuard : Script<PerceptionDemoGuard> {
    float faceX = 0.0f;         // 向いている方向 (水平の単位ベクトル)
    float faceZ = 1.0f;
    bool faceSet = false;
    int32_t perceived = 0;      // 直近に読んだ知覚している相手の数
    int32_t senses = 0;         // 向いた相手の感覚 (この tick に知覚していれば currentSenses、記憶なら 0)
    int32_t alertTicks = 0;     // 何かを知覚していた tick の累計

    void Update(MyeUpdateContext& ctx)
    {
        constexpr float kTurnRate = 0.08f; // 1 tick に目標の向きへ寄せる割合

        if (!faceSet) {
            MyeQuat q = {};
            ctx.api->GetLocalRotation(ctx.api->engine, ctx.self, &q);
            // y 軸まわりの回転 q の +Z = (2(xz + wy), 2(yz - wx), 1 - 2(x^2 + y^2)) の水平成分
            faceX = 2.0f * (q.x * q.z + q.w * q.y);
            faceZ = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
            const float l = std::sqrt(faceX * faceX + faceZ * faceZ);
            faceX = l > 1e-6f ? faceX / l : 0.0f;
            faceZ = l > 1e-6f ? faceZ / l : 1.0f;
            faceSet = true;
        }

        // 今知覚している相手を優先し、無ければ最も新しい記憶。予測位置の方を向く
        perceived = MyePerceptionGetCount(ctx, ctx.self);
        int32_t best = -1;
        MyePercept bestP = {};
        for (int32_t i = 0; i < perceived; ++i) {
            MyePercept q = {};
            if (!MyePerceptionGet(ctx, ctx.self, i, q)) {
                continue;
            }
            const bool better = best < 0 || (q.currentSenses != 0 && bestP.currentSenses == 0)
                || ((q.currentSenses != 0) == (bestP.currentSenses != 0) && q.lastSensedTick > bestP.lastSensedTick);
            if (better) {
                best = i;
                bestP = q;
            }
        }
        if (best < 0) {
            senses = 0;
            return;
        }
        ++alertTicks;
        senses = static_cast<int32_t>(bestP.currentSenses);

        MyeVec3 p = {};
        ctx.api->GetLocalPosition(ctx.api->engine, ctx.self, &p);
        float tx = bestP.predictedPos.x - p.x;
        float tz = bestP.predictedPos.z - p.z;
        const float tl = std::sqrt(tx * tx + tz * tz);
        if (tl < 1e-3f) {
            return;
        }
        tx /= tl;
        tz /= tl;
        float nx = faceX + (tx - faceX) * kTurnRate;
        float nz = faceZ + (tz - faceZ) * kTurnRate;
        const float nl = std::sqrt(nx * nx + nz * nz);
        if (nl < 1e-4f) {
            // 真後ろ: 寄せると長さが 0 になるので、右へ 90 度回してから寄せ直す
            nx = faceZ;
            nz = -faceX;
        } else {
            nx /= nl;
            nz /= nl;
        }
        faceX = nx;
        faceZ = nz;
        // +Z を (faceX, faceZ) へ向ける y 軸回転。cos θ = faceZ、sin θ = faceX の半角
        const float c = faceZ;
        const float half = std::sqrt((1.0f + c) * 0.5f);
        const float s = std::sqrt((1.0f - c) * 0.5f);
        const MyeQuat rot = { 0.0f, faceX >= 0.0f ? s : -s, 0.0f, half };
        ctx.api->SetLocalRotation(ctx.api->engine, ctx.self, rot);
    }
};
REGISTER_SCRIPT(PerceptionDemoGuard,
                FIELDS(faceX, faceZ, faceSet, MYE_F_JP(perceived, "知覚している数"), MYE_F_JP(senses, "向いた相手の感覚"),
                       MYE_F_JP(alertTicks, "気付いていた tick")));
