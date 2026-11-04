//====================================================================================
//                          FeelDemo.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触り (M94、ADR-031) のデモと replay の被覆 (--feel-demo)
//====================================================================================
// 台本 (シーン内の経過 tick で決める = 入力なしで毎回同じ列になる = replay の feel ジョブの被覆):
//   リグ (このスクリプトの持ち主) を左右へ往復させ、カメラは SpringFollow で遅れて追う
//   tick  60 : プリセット "HeavyHit" (ヒットストップ + 揺れ + キック + 振動 + 画面効果)
//   tick 150 : コードで組んだヒットストップだけ (MyeHitStop) — 戻り値 0 = 止める対象なし
//   tick 200 : 時限スロー (MyeSlowMotion、イージングで等速へ戻る)
//   tick 300 : 揺れを鳴らしてから途中 (tick 310) で止める (MyeStopFeel)
//   tick 360 : プリセット "LightHit" を強さ 0.5 で
// 床で跳ねるボールは FeelTrigger でプリセット "Bump" を鳴らす (コード無しの経路)。
// ヒットストップ中はリグも止める (MyeIsSimStepping) — スクリプトは非ゲートなので、止めないと
// 「物理だけ止まってリグは動く」絵になる
#include "Shared/ScriptAPI.h"

namespace {

constexpr float kRigSpeed = 3.0f;      // [m/s]
constexpr float kRigHalfRange = 4.0f;  // 往復の片側 [m]

} // namespace

struct FeelDemo : Script<FeelDemo> {
    int32_t ticks = 0;            // 台本の時計 (このスクリプトが動いた tick 数)
    int32_t steppedTicks = 0;     // ゲート対象が進んだ tick 数 (ヒットストップ・スローで遅れる)
    float rigX = 0.0f;
    float rigDir = 1.0f;
    int32_t shakeHandle = 0;
    int32_t lastResult = 0;       // 直近の PlayFeel の戻り値 (Inspector で見る)

    void Update(MyeUpdateContext& ctx)
    {
        ++ticks;
        if (MyeIsSimStepping(ctx)) {
            ++steppedTicks;
            rigX += rigDir * kRigSpeed * ctx.dt;
            if (rigX > kRigHalfRange || rigX < -kRigHalfRange) {
                rigDir = -rigDir;
            }
            MyeSelf(ctx).SetLocalPosition({ rigX, 0.0f, 0.0f });
        }
        switch (ticks) {
        case 60: lastResult = MyePlayFeel(ctx, "HeavyHit"); break;
        case 150: lastResult = MyeHitStop(ctx, 8); break;
        case 200: lastResult = MyeSlowMotion(ctx, 30, 20, 30, MYE_EASE_OUT_QUAD); break;
        case 300: shakeHandle = MyeCameraShake(ctx, 0.08f, 1.5f, 60); break;
        case 310: MyeStopFeel(ctx, shakeHandle); break;
        case 360: lastResult = MyePlayFeel(ctx, "LightHit", 0.5f); break;
        default: break;
        }
    }
};
REGISTER_SCRIPT(FeelDemo,
                FIELDS(MYE_F_JP(ticks, "経過 tick"), MYE_F_JP(steppedTicks, "進んだ tick"),
                       MYE_F_JP(rigX, "リグの位置"), MYE_F_JP(rigDir, "リグの向き"),
                       MYE_F_JP(shakeHandle, "揺れのハンドル"), MYE_F_JP(lastResult, "直近の戻り値")));
