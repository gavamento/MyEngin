// ローカルマルチプレイのデモ (M52g)。
//
// **エンジンの ABI は 1 本も足さずに**「自分がどのプレイヤーの入力で動くか」を読む:
// エンジンが毎 tick 書く `PlayerInputComponent` のミラーを、v11 の汎用フィールドスロット
// (GetComponentField、名前ハッシュ引き) で読むだけ。レーン別の専用スロット
// (GetActionForPlayer / GetAxisForPlayer) は M52i の ABI v13 へ束ねる。
//
// 読む index は assets\input\actions.json の**定義順**:
//   axes[0] = MoveX / axes[1] = MoveY / アクション bit0 = Jump
#include "Shared/ScriptAPI.h"

namespace {
// PlayerInputComponent (Engine/Core/Components.h) の名前ハッシュ。
// **毎 tick ハッシュを取り直さない** — 文字列を舐めるコストを 4 体 × 60Hz 払う理由が無い
const uint64_t kCompPlayerInput = MyeNameHash("PlayerInput");
const uint64_t kFieldAxes = MyeNameHash("axes");
const uint64_t kFieldPressed = MyeNameHash("pressedBits");
const uint64_t kFieldConnected = MyeNameHash("connected");

constexpr float kGravity = 18.0f;
constexpr float kHalfRangeX = 8.0f;
constexpr float kHalfRangeZ = 5.0f;

float Clamp(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
} // namespace

struct LocalPlayerDemo : Script<LocalPlayerDemo> {
    float moveSpeed = 4.0f;
    float jumpSpeed = 6.0f;
    float vy = 0.0f;        // 垂直速度 (登録フィールド = スナップショットにも載る sim 状態)
    int32_t jumpCount = 0;  // 押した回数 (レーンごとに違う値になるので配線の目視にも使える)

    void Update(MyeUpdateContext& ctx)
    {
        MyeVec4 axes = {};
        uint32_t pressed = 0;
        bool connected = false;
        // ミラーが読めない = PlayerInput を持たないエンティティに付いている。
        // 黙って動かないのが正しい (勝手にレーン 0 で動かすと配線ミスが隠れる)
        if (!MyeGetField(ctx, ctx.self, kCompPlayerInput, kFieldConnected, connected)) {
            return;
        }
        MyeGetField(ctx, ctx.self, kCompPlayerInput, kFieldAxes, axes);
        MyeGetField(ctx, ctx.self, kCompPlayerInput, kFieldPressed, pressed);

        MyeGameObject self = MyeSelf(ctx);
        MyeVec3 pos = self.GetLocalPosition();
        if (connected) {
            pos.x += axes.x * moveSpeed * ctx.dt;
            pos.z += axes.y * moveSpeed * ctx.dt;
            if ((pressed & 1u) != 0 && pos.y <= 0.5001f) { // bit0 = Jump (actions.json の定義順)
                vy = jumpSpeed;
                ++jumpCount;
            }
        }
        // 接地判定込みの縦方向。接続が切れても落下だけは続ける (空中で凍らせない)
        vy -= kGravity * ctx.dt;
        pos.y += vy * ctx.dt;
        if (pos.y < 0.5f) {
            pos.y = 0.5f;
            vy = 0.0f;
        }
        // 場外へ出さない — 600 tick 流し続けると合成入力でも普通に画面外へ消える
        pos.x = Clamp(pos.x, -kHalfRangeX, kHalfRangeX);
        pos.z = Clamp(pos.z, -kHalfRangeZ, kHalfRangeZ);
        self.SetLocalPosition(pos);
    }
};

REGISTER_SCRIPT(LocalPlayerDemo, FIELDS(moveSpeed, jumpSpeed, vy, jumpCount));
