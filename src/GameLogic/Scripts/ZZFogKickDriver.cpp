//====================================================================================
//                          ZZFogKickDriver.cpp
//  MyEngin/ 秋田蓮音                                                       11/09/2026
//                                          霧の嵩のデモの蹴る人 (--fog-kick-demo)
//====================================================================================
// 自動: 一辺 4 m の四角い経路を歩いて止まり、止まった所で前を蹴る (シーン内の経過 tick だけで決まる = 入力なしで決定的)。
// 操作: MoveX / MoveY (WASD・左スティック) で歩く。Fire (J・左クリック・RB) で前を蹴る。
//       入力があってから kManualHoldTicks の間は自動を止める (入力は replay に記録されるので決定性は崩れない)。
// 蹴り = 足の前の一点から外向きの Radial を kKickTicks 続けて出す (1 tick で嵩が動けるのは 1 マスまでなので、続けて押す)。
// 濃さは変えない。穴・縁の盛り上がり・埋め戻しは、場の嵩チャネルの sim (保存の移流と重力) が作る
// ★スクリプト名は名前順ソートで TypeId が決まる。既存スクリプトの TypeId を動かさないよう、
//   現行末尾の ZZFieldDemoDriver より後ろに並ぶ名前にしてある
#include <cmath>

#include "Shared/ScriptAPI.h"

namespace {

constexpr uint32_t kNullIndex = 0xFFFFFFFFu; // エンジンの null エンティティ。{} の {0, 0} は実在のエンティティを指しうる
constexpr float kWalkSpeed = 2.0f;        // [m/s]
constexpr int32_t kCycleTicks = 180;      // 自動: 1 区間 (歩く + 止まる) の長さ
constexpr int32_t kWalkTicks = 120;       // 自動: 区間のうち歩く tick 数 (4 m)
constexpr int32_t kAutoKickTick = 130;    // 自動: 区間の中で蹴る tick (止まってから少し後)
constexpr int32_t kManualHoldTicks = 300; // 操作の入力から自動へ戻るまで [tick] (5 秒)
constexpr float kAxisThreshold = 0.2f;
constexpr int32_t kKickTicks = 14;        // 蹴りの長さ [tick]。この間 Radial を出し続ける
constexpr int32_t kKickCooldownTicks = 20; // 蹴り始めから次に蹴れるまで [tick]
constexpr float kKickReach = 1.2f;        // 体の中心から押す点までの水平距離 [m]
constexpr float kKickHeight = 0.2f;       // 押す点の高さ [m] (層の中)
constexpr float kKickRadius = 2.0f;       // [m]
constexpr float kKickSpeed = 12.0f;       // 外向きの速さ [m/s] (マス 0.25 m の流れの上限 15 m/s の内側)
// 見た目の足 (子 "KickFoot") のローカル位置。蹴りの間だけ前へ出す
constexpr float kFootY = -0.7f;
constexpr float kFootRestZ = 0.25f;
constexpr float kFootReachZ = 0.75f;

// 自動の経路の向き (+x → +z → -x → -z の順に一周)
MyeVec3 AutoDirection(int32_t cycle)
{
    switch (cycle % 4) {
    case 0: return { 1.0f, 0.0f, 0.0f };
    case 1: return { 0.0f, 0.0f, 1.0f };
    case 2: return { -1.0f, 0.0f, 0.0f };
    default: return { 0.0f, 0.0f, -1.0f };
    }
}

// 水平の単位ベクトル (x, z) を向く y 軸まわりの回転。三角関数を使わず半角の公式で作る
MyeQuat YawToward(float dx, float dz)
{
    const float c = std::sqrt((1.0f + dz) * 0.5f);
    const float s = std::sqrt((1.0f - dz) * 0.5f);
    return MyeQuat{ 0.0f, dx < 0.0f ? -s : s, 0.0f, c };
}

} // namespace

struct ZZFogKickDriver : Script<ZZFogKickDriver> {
    int32_t ticks = 0;          // 台本の時計
    int32_t manualTicks = 0;    // 0 より大きい間は操作 (自動を止める)
    int32_t kickLeft = 0;       // 残りの蹴りの tick (0 = 蹴っていない)
    int32_t cooldown = 0;       // 次に蹴れるまで
    int32_t kickCount = 0;      // 通算の蹴りの数
    MyeVec3 facing = { 1.0f, 0.0f, 0.0f }; // 向き (水平の単位ベクトル)
    MyeVec3 kickPos = { 0.0f, 0.0f, 0.0f }; // 押している点 (蹴り始めに決める)
    MyeEntityId foot = { kNullIndex, 0 };   // 見た目の足

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        if (!api->IsAlive(api->engine, foot)) {
            foot = api->FindByName(api->engine, "KickFoot");
        }
        const MyeVec3 pos = MyeSelf(ctx).GetWorldPosition();

        const float ax = MyeAxis(ctx, "MoveX");
        const float ay = MyeAxis(ctx, "MoveY");
        const bool moving = std::fabs(ax) > kAxisThreshold || std::fabs(ay) > kAxisThreshold;
        const bool firePressed = MyeActionPressed(ctx, "Fire");
        if (moving || firePressed) {
            manualTicks = kManualHoldTicks;
        }

        MyeVec3 velocity = { 0.0f, 0.0f, 0.0f };
        bool autoKick = false;
        if (manualTicks > 0) {
            --manualTicks;
            if (moving) {
                // カメラは +z を見ているので W (MoveY 正) = +z
                const float len = std::sqrt(ax * ax + ay * ay);
                facing = { ax / len, 0.0f, ay / len };
                const float k = len > 1.0f ? kWalkSpeed / len : kWalkSpeed;
                velocity = { ax * k, 0.0f, ay * k };
            }
        } else {
            const int32_t phase = ticks % kCycleTicks;
            facing = AutoDirection(ticks / kCycleTicks);
            if (phase < kWalkTicks) {
                velocity = { facing.x * kWalkSpeed, 0.0f, facing.z * kWalkSpeed };
            }
            autoKick = phase == kAutoKickTick;
        }

        if (cooldown > 0) {
            --cooldown;
        }
        if (cooldown == 0 && (firePressed || autoKick)) {
            kickLeft = kKickTicks;
            cooldown = kKickCooldownTicks;
            kickPos = { pos.x + facing.x * kKickReach, kKickHeight, pos.z + facing.z * kKickReach };
            ++kickCount;
        }
        if (kickLeft > 0) {
            velocity = { 0.0f, 0.0f, 0.0f }; // 蹴っている間は立ち止まる
            MyeFieldRadial(ctx, kickPos, kKickRadius, kKickSpeed);
            --kickLeft;
        }
        api->CharacterMove(api->engine, ctx.self, velocity);
        MyeSelf(ctx).SetLocalRotation(YawToward(facing.x, facing.z));

        // 足: 蹴りの前半で前へ出し、後半で戻す
        if (api->IsAlive(api->engine, foot)) {
            const int32_t done = kKickTicks - kickLeft; // 0..kKickTicks (蹴っていないときは kKickTicks)
            const float half = static_cast<float>(kKickTicks) * 0.5f;
            const float t = kickLeft > 0 ? 1.0f - std::fabs(static_cast<float>(done) - half) / half : 0.0f;
            MyeGameObject{ foot, api }.SetLocalPosition({ 0.0f, kFootY, kFootRestZ + (kFootReachZ - kFootRestZ) * t });
        }
        ++ticks;
    }
};
REGISTER_SCRIPT(ZZFogKickDriver,
                FIELDS(MYE_F_JP(ticks, "経過 tick"), MYE_F_JP(manualTicks, "操作の残り tick"),
                       MYE_F_JP(kickCount, "蹴りの数")));
