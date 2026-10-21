//====================================================================================
//                          ZTwoDHero.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          --2d-demo の主人公 (ZTwoDHero) と動く床 (ZTwoDMover) のスクリプト
//====================================================================================
// ★入力は PlayerInput のミラーと同じ (tick, lane 0) の合成入力 / 実入力から取り、アクション名で引く
//   (assets\input\actions.json の並び順に依存しない)。--synth-input でも実入力でも同じコードが回る。
// ★時間は登録フィールドの tick カウンタで持つ (実時間も float の秒累積も使わない)。
// ★スクリプト名は名前順ソートで TypeId が決まるので、現行末尾の WaveWalker より後になる 'Z' で始めてある
//   (既存スクリプトの TypeId を 1 つも動かさない)。
#include "Shared/ScriptAPI.h"

namespace {
constexpr float kAxisThreshold = 0.3f;       // これを超えた軸入力だけを移動とみなす (パッドのアナログ値の揺れを丸める)
constexpr float kDownThreshold = -0.5f;      // MoveY がこれ未満なら「下」(一方通行床を降りる)
constexpr float kAttackReach = 0.65f;        // 攻撃が届く前方の距離 (m)。CC の半径 0.3 + 少し
constexpr float kFeetToCellCenter = 0.65f;   // CC 中心から足元のセルの中央までの下向き距離 (m)。CC の高さ 1.8 の半分 - 0.25
constexpr int32_t kAttackCooldownTicks = 20; // 連続攻撃の間隔 (tick)
constexpr int32_t kAttackDamage = 1;
constexpr uint32_t kNullIndex = 0xFFFFFFFFu;  // エンジンの null エンティティ (EntityID::kInvalidIndex)。{} の {0, 0} は実在のエンティティを指しうる
constexpr int32_t kGroundLayer = 1;          // level.tilemap.json の Ground レイヤー (0 Back / 1 Ground / 2 Front)

const uint64_t kCompCharacter = MyeNameHash("CharacterController");
const uint64_t kFieldIgnoreOneWay = MyeNameHash("ignoreOneWay");
const uint64_t kFieldWallContact = MyeNameHash("wallContact");
const uint64_t kCompSprite2D = MyeNameHash("Sprite2D");
const uint64_t kFieldFlipX = MyeNameHash("flipX");
const uint64_t kParamSpeed = MyeNameHash("speed");
} // namespace

// 左右 (MoveX)・ジャンプ (Jump)・下 (MoveY < 0 で一方通行を降りる)・攻撃 (Fire、または壁へ押し付けたとき) で動く主人公。
// 攻撃は前方の足元 2 セルへ TilemapDamageCell を送る (壊れるブロックを掘る)
struct ZTwoDHero : Script<ZTwoDHero> {
    float moveSpeed = 4.0f;
    float jumpSpeed = 9.0f;
    int32_t facing = 1;           // -1 左 / +1 右
    int32_t attackCooldown = 0;   // 残り tick
    int32_t attackCount = 0;      // 通算の攻撃回数 (selftest と目視用)
    MyeEntityId level = { kNullIndex, 0 };   // Tilemap を持つエンティティ ("Level")
    MyeEntityId sprite = { kNullIndex, 0 };  // 左右反転を書く先 ("HeroSprite")

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        if (!api->IsAlive(api->engine, level)) {
            level = api->FindByName(api->engine, "Level");
        }
        if (!api->IsAlive(api->engine, sprite)) {
            sprite = api->FindByName(api->engine, "HeroSprite");
        }

        const float axisX = MyeAxis(ctx, "MoveX");
        const int32_t moveX = axisX > kAxisThreshold ? 1 : (axisX < -kAxisThreshold ? -1 : 0);
        if (moveX != 0 && moveX != facing) {
            facing = moveX;
            bool flip = facing < 0;
            MyeSetField(ctx, sprite, kCompSprite2D, kFieldFlipX, flip);
        }
        api->CharacterMove(api->engine, ctx.self, { static_cast<float>(moveX) * moveSpeed, 0.0f, 0.0f });
        api->AnimatorSetFloat(api->engine, ctx.self, kParamSpeed, moveX != 0 ? 1.0f : 0.0f);

        // 下を押している間だけ一方通行の床を無視する
        bool ignore = MyeAxis(ctx, "MoveY") < kDownThreshold;
        bool current = false;
        if (MyeGetField(ctx, ctx.self, kCompCharacter, kFieldIgnoreOneWay, current) && current != ignore) {
            MyeSetField(ctx, ctx.self, kCompCharacter, kFieldIgnoreOneWay, ignore);
        }

        if (MyeActionPressed(ctx, "Jump") && api->CharacterIsGrounded(api->engine, ctx.self)) {
            api->CharacterJump(api->engine, ctx.self, jumpSpeed);
        }

        if (attackCooldown > 0) {
            --attackCooldown;
        }
        int32_t wall = 0;
        MyeGetField(ctx, ctx.self, kCompCharacter, kFieldWallContact, wall);
        const bool pushingWall = moveX != 0 && (wall == 2 || wall == moveX);
        if (attackCooldown == 0 && (MyeActionPressed(ctx, "Fire") || pushingWall)) {
            Attack(ctx);
        }
    }

private:
    // 前方の足元のセルと、その 1 つ上のセル (CC の高さ分) を同時に傷付ける
    void Attack(MyeUpdateContext& ctx)
    {
        MyeVec3 pos = MyeSelf(ctx).GetLocalPosition();
        const MyeVec3 front = { pos.x + static_cast<float>(facing) * kAttackReach, pos.y - kFeetToCellCenter, pos.z };
        int32_t cx = 0;
        int32_t cy = 0;
        if (!MyeTilemapWorldToCell(ctx, level, front, cx, cy)) {
            return;
        }
        MyeTilemapDamageCell(ctx, level, kGroundLayer, cx, cy, kAttackDamage);
        MyeTilemapDamageCell(ctx, level, kGroundLayer, cx, cy + 1, kAttackDamage);
        attackCooldown = kAttackCooldownTicks;
        ++attackCount;
    }
};
REGISTER_SCRIPT(ZTwoDHero,
                FIELDS(MYE_F_RANGE(moveSpeed, "移動速度 (m/s)", 0.0f, 20.0f),
                       MYE_F_RANGE(jumpSpeed, "ジャンプ初速 (m/s)", 0.0f, 20.0f), MYE_F_JP(facing, "向き (-1 / +1)"),
                       MYE_F_JP(attackCooldown, "攻撃の残り待ち (tick)"), MYE_F_JP(attackCount, "攻撃回数"),
                       MYE_F_JP(level, "タイルマップ"), MYE_F_JP(sprite, "反転先のスプライト")));

// kinematic Rigidbody (plane2D) の床を左右に往復させる。velocity を書くだけで動き、CC が乗る (Transform は触らない)
struct ZTwoDMover : Script<ZTwoDMover> {
    float speed = 2.0f;           // m/s
    int32_t halfPeriodTicks = 180; // 片道の tick 数
    int32_t ticks = 0;

    void Update(MyeUpdateContext& ctx)
    {
        static const uint64_t kCompRigidbody = MyeNameHash("Rigidbody");
        static const uint64_t kFieldVelocity = MyeNameHash("velocity");
        const bool forward = halfPeriodTicks <= 0 || (ticks / halfPeriodTicks) % 2 == 0;
        ++ticks;
        const MyeVec3 v = { forward ? speed : -speed, 0.0f, 0.0f };
        MyeSetField(ctx, ctx.self, kCompRigidbody, kFieldVelocity, v);
    }
};
REGISTER_SCRIPT(ZTwoDMover, FIELDS(MYE_F_RANGE(speed, "速さ (m/s)", 0.0f, 20.0f), MYE_F_JP(halfPeriodTicks, "片道の tick 数"),
                                   MYE_F_JP(ticks, "経過 tick")));
