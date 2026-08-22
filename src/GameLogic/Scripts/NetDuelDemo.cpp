// 2 人ネット対戦のデモ (M52i、--net-demo)。
//
// ABI v13 で開通した 2 系統を**使い分けの実例**として並べてある:
//
//   決定論の内側 … `MyeAxisFor` / `MyeActionPressedFor` (レーン指定のアクションマップ)。
//                   記録済み入力の純関数なので sim 状態へそのまま書いてよい。
//                   2 台は同じレーン値を消費するので、同じ位置・同じ得点になる。
//   決定論の外側 … `MyeNetLocalPlayer` / `MyeNetPingMs` / `MyeNetRollbackCount`。
//                   **自分がどちら側か・実時間・巻き戻し回数**という機種依存の値で、
//                   sim 状態へ書いた瞬間に 2 台のワールドハッシュが割れる。
//                   このデモでは NetHudDemo が UIElement (NoHash の描画レーン) に
//                   書くだけに留めてある。
//
// ★誤用したらどうなるかは、実際に試せる: 下の Update で score を
//   `score += (int)MyeNetRollbackCount(ctx);` のようにすると、数十 tick で
//   desync 検出が働き crash\desync_<tick>_p<lane>\ が両方の端末に出る。
//   「静かに壊れない」ことがこの設計の主張そのもの。
#include "Shared/ScriptAPI.h"

namespace {
// 自分がどのレーンを読むかは PlayerInput ミラー (M52g) から取る。
// **毎 tick 文字列を舐めない** — 名前ハッシュは 1 回だけ作る
const uint64_t kCompPlayerInput = MyeNameHash("PlayerInput");
const uint64_t kFieldPlayerIndex = MyeNameHash("playerIndex");

constexpr float kGravity = 18.0f;
constexpr float kHalfRangeX = 9.0f;
constexpr float kHalfRangeZ = 6.0f;
// 得点リングの内径 / 外径 (DemoContent の ScoreRing の見た目と合わせてある)
constexpr float kRingInner = 2.4f;
constexpr float kRingOuter = 3.2f;

float Clamp(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
} // namespace

struct NetDuelDemo : Script<NetDuelDemo> {
    float moveSpeed = 5.0f;
    float jumpSpeed = 6.5f;
    float vy = 0.0f;     // 垂直速度 (登録フィールド = スナップショットにも載る sim 状態)
    int32_t score = 0;   // リングへ入った回数
    int32_t inZone = 0;  // 前 tick にリング内だったか (入った瞬間だけ数えるためのエッジ)

    void Update(MyeUpdateContext& ctx)
    {
        int32_t lane = 0;
        // ミラーが読めない = PlayerInput を持たないエンティティに付いている。
        // 黙って動かないのが正しい (勝手にレーン 0 で動かすと配線ミスが隠れる)
        if (!MyeGetField(ctx, ctx.self, kCompPlayerInput, kFieldPlayerIndex, lane)) {
            return;
        }
        const uint32_t player = static_cast<uint32_t>(lane < 0 ? 0 : lane);

        // ---- 決定論の内側: レーン指定のアクションマップ (v13) ----
        const float mx = MyeAxisFor(ctx, "MoveX", player);
        const float my = MyeAxisFor(ctx, "MoveY", player);
        const bool jump = MyeActionPressedFor(ctx, "Jump", player);

        MyeGameObject self = MyeSelf(ctx);
        MyeVec3 pos = self.GetLocalPosition();
        pos.x += mx * moveSpeed * ctx.dt;
        pos.z += my * moveSpeed * ctx.dt;
        if (jump && pos.y <= 0.5001f) {
            vy = jumpSpeed;
        }
        vy -= kGravity * ctx.dt;
        pos.y += vy * ctx.dt;
        if (pos.y < 0.5f) {
            pos.y = 0.5f;
            vy = 0.0f;
        }
        pos.x = Clamp(pos.x, -kHalfRangeX, kHalfRangeX);
        pos.z = Clamp(pos.z, -kHalfRangeZ, kHalfRangeZ);
        self.SetLocalPosition(pos);

        // ---- 得点: 中央リングへ入った瞬間に +1 ----
        // ★平方根を使わず二乗のまま比べる。丸めの経路を 1 つ減らすほど、
        //   「Debug と Release でビット一致する」を守るのが楽になる (規則 4/5 の精神)
        const float d2 = pos.x * pos.x + pos.z * pos.z;
        const int32_t now = (d2 >= kRingInner * kRingInner && d2 <= kRingOuter * kRingOuter) ? 1 : 0;
        if (now != 0 && inZone == 0) {
            ++score;
        }
        inZone = now;

        // 得点は大きさで見せる (sim 状態なので 2 台で同じ値になる = 決定論の内側)
        const float s = 1.0f + 0.06f * static_cast<float>(score % 8);
        const MyeVec3 sc = { s, s, s };
        ctx.api->SetLocalScale(ctx.api->engine, ctx.self, sc);
    }
};

REGISTER_SCRIPT(NetDuelDemo, FIELDS(moveSpeed, jumpSpeed, vy, score, inZone));
