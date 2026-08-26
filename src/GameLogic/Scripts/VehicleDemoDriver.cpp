// 関節ショーケース (M60i) の運転手。
//
// **エンジンの ABI は 1 本も足していない** — 運転入力 (steer / throttle / brake) は
// `VehicleComponent` の **sim 状態フィールド**なので、v11 からある汎用スロット
// `SetComponentField` (名前ハッシュ引き) でそのまま書ける。車両のために専用スロットを
// 足さない、という M60 の決定 (M60j 廃止) を実走で示しているのがこのスクリプト。
// おまけに入力が sim 状態なので、snapshot / .rep / タイムトラベルが**何もしなくても**
// 運転操作を運ぶ。
//
// ★時間は **登録フィールドの int カウンタ**で持つ。実時間も float の秒累積も使わない
//   (規則 2: 実時間は sim に混ぜない / 秒の float 累積は加算順で割れる)。
#include "Shared/ScriptAPI.h"

namespace {
// VehicleComponent (Engine/Core/Components.h) の名前ハッシュ。
// **毎 tick 取り直さない** — 文字列を舐める理由が無い (LocalPlayerDemo と同じ流儀)
const uint64_t kCompVehicle = MyeNameHash("Vehicle");
const uint64_t kFieldSteer = MyeNameHash("steer");
const uint64_t kFieldThrottle = MyeNameHash("throttle");
const uint64_t kFieldBrake = MyeNameHash("brake");

// 走行プログラム (60Hz)。replay_verify は 600 tick 回すので、10 秒で
// 「加速 → 左旋回 → 右旋回 → 制動 → 停車保持」が全部載りきるように区切ってある。
// ★旋回を **S 字 (左→右)** にしてあるのは 2 つ理由がある:
//   ①舵の左右で対称に効くこと (片側だけだと符号の取り違えが replay に出ない)
//   ②一方向へ回し続けると 10 秒で大きく円を描いて**画面の外へ出てしまう**
// ★区切りが短めなのは **10 秒ぶんの走行距離を画角に収めるため**。全開のまま 10 秒
//   走らせると 60m 以上進んで画面の外へ出る (実測) — 速度と舵は「絵に映り続ける」
//   ことを優先して決めてある。数値としての限界は M60h2 の試験が押さえている
constexpr int32_t kAccelEnd = 72;  // 0.0〜1.2s: 全開直進 (駆動力)
constexpr int32_t kLeftEnd = 192;  // 1.2〜3.2s: 左へ舵 (摩擦円が効く)
constexpr int32_t kRightEnd = 312; // 3.2〜5.2s: 右へ舵で切り返す
constexpr int32_t kBrakeEnd = 402; // 5.2〜6.7s: フルブレーキ
                                   // 6.7s〜  : 停車したままブレーキを踏み続ける
constexpr float kTurnSteer = 0.5f;      // 切れ角 = 0.5 * 30 度 = 15 度 (旋回半径 約 10m)
constexpr float kTurnThrottle = 0.15f;  // 旋回中は転がり抵抗を補うだけ
} // namespace

struct VehicleDemoDriver : Script<VehicleDemoDriver> {
    int32_t ticks = 0; // 登録フィールド = スナップショットにも .rep にも載る sim 状態

    void Update(MyeUpdateContext& ctx)
    {
        float steer = 0.0f;
        float throttle = 0.0f;
        float brake = 0.0f;
        if (ticks < kAccelEnd) {
            throttle = 1.0f;
        } else if (ticks < kLeftEnd) {
            throttle = kTurnThrottle;
            steer = -kTurnSteer;
        } else if (ticks < kRightEnd) {
            throttle = kTurnThrottle;
            steer = kTurnSteer;
        } else if (ticks < kBrakeEnd) {
            brake = 1.0f;
        } else {
            // ★**停車後もブレーキを踏み続ける**のがこの区間の主張 — 制動力をそのまま
            //   入れると停車中の車が後ろへ走り出す (M60h2 の申し送り 3)。「速度をちょうど
            //   殺す上限」が効いていないと、ここで車が動き出して .rep に必ず出る
            brake = 1.0f;
        }
        // 書けない = Vehicle を持たないエンティティに付いている。黙って何もしないのが
        // 正しい (配線ミスを隠さない)
        MyeSetField(ctx, ctx.self, kCompVehicle, kFieldSteer, steer);
        MyeSetField(ctx, ctx.self, kCompVehicle, kFieldThrottle, throttle);
        MyeSetField(ctx, ctx.self, kCompVehicle, kFieldBrake, brake);
        ++ticks;
    }
};

REGISTER_SCRIPT(VehicleDemoDriver, FIELDS(ticks));
