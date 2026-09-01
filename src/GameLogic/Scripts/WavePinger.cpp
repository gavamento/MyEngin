// 音響ショーケース (M65b) の音源。一定間隔で AcousticEmitter へ発音要求を書くだけ。
//
// **エンジンの ABI は 1 本も足していない** — 発音要求 (pendingLoudness / pendingRadiusM /
// pendingTone / ticksPerRing) は `AcousticEmitterComponent` の **sim 状態フィールド**なので、
// v11 からある汎用スロット `SetComponentField` (名前ハッシュ引き) でそのまま書ける
// (VehicleDemoDriver が車両入力で示したのと同じ形)。
// おまげに要求が sim 状態なので、snapshot / .rep / タイムトラベルが**何もしなくても**
// 「いつ鳴ったか」を運ぶ。
//
// ★時間は **登録フィールドの int カウンタ**で持つ。実時間も float の秒累積も使わない
//   (規則 2: 実時間は sim に混ぜない / 秒の float 累積は加算順で割れる)。
// ★スクリプト名は名前順ソートで TypeId が決まるので 'Wa v' で始めてある
//   (現行末尾の WalkerDemo より後。M65g の Watcher* が入っても後ろのまま)。
#include "Shared/ScriptAPI.h"

namespace {
// AcousticEmitterComponent (Engine/Core/Components.h) の名前ハッシュ。
// **毎 tick 取り直さない** (VehicleDemoDriver と同じ流儀)
const uint64_t kCompEmitter = MyeNameHash("AcousticEmitter");
const uint64_t kFieldLoudness = MyeNameHash("pendingLoudness");
const uint64_t kFieldRadius = MyeNameHash("pendingRadiusM");
const uint64_t kFieldTone = MyeNameHash("pendingTone");
const uint64_t kFieldTicksPerRing = MyeNameHash("ticksPerRing");
} // namespace

struct WavePinger : Script<WavePinger> {
    int32_t ticks = 0;         // 登録フィールド = snapshot にも .rep にも載る sim 状態
    int32_t everyTicks = 150;  // 発音間隔。既定 2.5 秒
    int32_t startDelay = 6;    // 最初の 1 発を遅らせる (シーン構築直後の 1 tick 目を避ける)
    float loudness = 1.0f;
    // ★到達距離 [m]。cellSize 0.5 なら 64 リング = kMaxWaveRing ちょうど。
    //   間取りの L 字経路 (音源→部屋 B の奥) が約 34m なので、部屋 B の手前までは
    //   確実に届き、奥は減衰で消える = **絵に「届く/届かない」の両方が出る**
    float radiusM = 32.0f;
    int32_t tone = 1;
    int32_t ringTicks = 2;     // 分周。2 = 15 m/s (cellSize 0.5 の場合)

    void Update(MyeUpdateContext& ctx)
    {
        // ★スクリプト層 (フェーズ 3) は音響フェーズ (3.4) の**直前**なので、
        //   ここで書いた要求は**同じ tick で**波になる
        const int32_t t = ticks - startDelay;
        if (t >= 0 && everyTicks > 0 && (t % everyTicks) == 0) {
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldTicksPerRing, ringTicks);
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldTone, tone);
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldRadius, radiusM);
            // 大きさは**最後**に書く — エンジンは pendingLoudness > 0 を発音の合図に
            // 見ているので、これを先に書くと半端な設定で鳴る余地ができる
            MyeSetField(ctx, ctx.self, kCompEmitter, kFieldLoudness, loudness);
        }
        ++ticks;
    }
};
REGISTER_SCRIPT(WavePinger,
                FIELDS(ticks, everyTicks, startDelay, loudness, radiusM, tone, ringTicks));
