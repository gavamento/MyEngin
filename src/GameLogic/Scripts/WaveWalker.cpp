// 音響ショーケース (M65c) の歩行者。床材タイルの上を往復するだけ。
//
// ★入力を一切読まない。`--synth-input` でも無入力でも**同じ道を同じ速さで歩く** —
//   replay 7 ペア目の被写体が入力に依存すると、記録と再生で足音の位置がずれる余地ができる。
// ★時間は登録フィールドの int カウンタで持つ (実時間も float の秒累積も使わない)。
// ★スクリプト名は名前順ソートで TypeId が決まるので、現行末尾の WavePinger より
//   後になる 'WaveW' にしてある (既存スクリプトの TypeId を 1 つも動かさない)。
#include "Shared/ScriptAPI.h"

struct WaveWalker : Script<WaveWalker> {
    int32_t ticks = 0;       // 登録フィールド = snapshot にも .rep にも載る sim 状態
    int32_t startDelay = 30; // シーン構築直後は CC が床に馴染んでいないので少し待つ
    int32_t legTicks = 285;  // 片道の tick 数。2.0 m/s x 4.75s = 9.5m = タイル 6 枚ぶん
    float speed = 2.0f;      // 水平速度 [m/s]。CharacterMove は m/s を保持する

    void Update(MyeUpdateContext& ctx)
    {
        const int32_t t = ticks - startDelay;
        float vx = 0.0f;
        if (t >= 0 && legTicks > 0) {
            // ★往復は**整数の割り算と偶奇**で決める。経過秒から sin を取ると
            //   折り返し位置が丸めに乗り、床材の境目がビルドで前後しうる
            vx = ((t / legTicks) % 2 == 0) ? speed : -speed;
        }
        ctx.api->CharacterMove(ctx.api->engine, ctx.self, { vx, 0.0f, 0.0f });
        ++ticks;
    }
};
REGISTER_SCRIPT(WaveWalker, FIELDS(ticks, startDelay, legTicks, speed));
