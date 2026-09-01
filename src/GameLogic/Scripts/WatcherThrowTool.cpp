// 企画 (三校企画.md) §5 の囮 (石 / 瓶) — M65g。**エンジンには 1 行も足していない**。
//
// ★**プレハブ資産にしなかった**。`.prefab.json` を経由すると `PrefabInstance.prefabHash`
//   (ハッシュ対象) に**正規化した絶対パスのハッシュ**が載り、sim 状態がチェックアウト先に
//   依存する。`Spawner.cpp` が M13 の頃から使っている「CreateGameObject +
//   SetMeshRenderer("builtin://...") + AddComponentByName」なら、そこに 1 つも
//   パス由来の値が入らない。計画本文は prefab と書いていたが、こちらが正しい。
// ★着弾音は**スクリプトが自分で鳴らす**。M65c の衝撃音は接触した面の PhysMat から
//   出るので、床材タイルの外 (素の床・壁) に落ちた石は 1 度も鳴らない = 企画 §5 の
//   「投げた先の地形が一瞬見える」が成立しない。飛翔中の物を数本だけ覚えておき、
//   速度が落ちた tick に AcousticEmitter へ要求を書く。
// ★止まった判定は**速度**で見る (接触ではない)。GetContactInfo は物理より後 =
//   LateUpdate でしか読めず、Update から読むと常に 0 が返る (EngineAPI.h v14 の注記)。
#include <cmath>

#include "Shared/ScriptAPI.h"

namespace {
constexpr uint8_t kVkQ = 0x51; // 石
constexpr uint8_t kVkE = 0x45; // 瓶

const uint64_t kCompEmitter = MyeNameHash("AcousticEmitter");
const uint64_t kFieldLoudness = MyeNameHash("pendingLoudness");
const uint64_t kFieldRadius = MyeNameHash("pendingRadiusM");
const uint64_t kFieldTone = MyeNameHash("pendingTone");
const uint64_t kFieldTicksPerRing = MyeNameHash("ticksPerRing");
const uint64_t kFieldCooldown = MyeNameHash("cooldownTicks");

const uint64_t kCompRigidbody = MyeNameHash("Rigidbody");
const uint64_t kFieldMass = MyeNameHash("mass");
const uint64_t kFieldRestitution = MyeNameHash("restitution");

// ★**登録フィールドは 16 本まで** (ScriptAPI.h の MYE_SF_FOREACH)。動かない調整値は
//   ここへ置く — フィールドにすると snapshot / ハッシュ / DLL リロードの復元に載る
constexpr int32_t kCooldownTicks = 20;
constexpr float kEyeHeight = 0.7f;
constexpr float kMuzzleAheadM = 0.7f; // 自分のコライダから出す距離
// 企画 §5 の表: 石 = 小〜中 (短い音) / 瓶 = 大 (割れて響く)
constexpr float kStoneLoudness = 0.35f;
constexpr float kStoneRadiusM = 12.0f;
constexpr float kBottleLoudness = 1.0f;
constexpr float kBottleRadiusM = 30.0f;
constexpr float kLandSpeed = 1.4f;       // これを下回ったら着弾とみなす [m/s]
constexpr int32_t kArmTicks = 8;         // 投げた直後は判定しない (手元で鳴らない)
constexpr int32_t kMaxFlightTicks = 400; // 保険。転がり続ける物を永久に追わない
} // namespace

struct WatcherThrowTool : Script<WatcherThrowTool> {
    // ---- 手持ち (企画 §5: 所持数には上限がある) ----
    int32_t stones = 4;
    int32_t bottles = 2;
    int32_t cooldown = 0;
    int32_t prevStoneKey = 0;
    int32_t prevBottleKey = 0;

    float throwSpeed = 11.0f;
    float throwLift = 2.4f; // 上向き成分 [m/s]。放物線にして「先を覗く」距離を稼ぐ

    // ---- 飛翔中 (最大 3 本。着弾を見張る対象) ----
    MyeEntityId fly0 = {};
    MyeEntityId fly1 = {};
    MyeEntityId fly2 = {};
    int32_t age0 = 0;
    int32_t age1 = 0;
    int32_t age2 = 0;
    int32_t kind0 = 0; // 0=石 1=瓶
    int32_t kind1 = 0;
    int32_t kind2 = 0;

    void Update(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        if (cooldown > 0) {
            --cooldown;
        }
        TrackFlights(ctx);

        const int32_t qk = api->KeyDown(api->engine, kVkQ);
        const int32_t ek = api->KeyDown(api->engine, kVkE);
        const bool stoneEdge = (qk != 0) && (prevStoneKey == 0);
        const bool bottleEdge = (ek != 0) && (prevBottleKey == 0);
        prevStoneKey = qk;
        prevBottleKey = ek;
        if (cooldown > 0) {
            return;
        }
        // ★石を先に見る (同じ tick に両方押されても順序が決まる)
        if (stoneEdge && stones > 0) {
            if (Throw(ctx, 0)) {
                --stones;
                cooldown = kCooldownTicks;
                MyeLogf(ctx, "watcher: threw stone (stones=%d bottles=%d)", stones, bottles);
            }
        } else if (bottleEdge && bottles > 0) {
            if (Throw(ctx, 1)) {
                --bottles;
                cooldown = kCooldownTicks;
                MyeLogf(ctx, "watcher: threw bottle (stones=%d bottles=%d)", stones, bottles);
            }
        }
    }

    MyeEntityId* Fly(int32_t i)
    {
        MyeEntityId* f[3] = { &fly0, &fly1, &fly2 };
        return (i >= 0 && i < 3) ? f[i] : nullptr;
    }
    int32_t* Age(int32_t i)
    {
        int32_t* a[3] = { &age0, &age1, &age2 };
        return (i >= 0 && i < 3) ? a[i] : nullptr;
    }
    int32_t* Kind(int32_t i)
    {
        int32_t* k[3] = { &kind0, &kind1, &kind2 };
        return (i >= 0 && i < 3) ? k[i] : nullptr;
    }

    // 飛翔中のものを見張り、止まった tick に発音要求を書いて追跡を終える
    void TrackFlights(MyeUpdateContext& ctx)
    {
        const MyeEngineApi* api = ctx.api;
        for (int32_t i = 0; i < 3; ++i) {
            MyeEntityId* f = Fly(i);
            if (MyeEntityIdIsNull(*f)) {
                continue;
            }
            if (!api->IsAlive(api->engine, *f)) {
                *f = {};
                continue;
            }
            int32_t& age = *Age(i);
            ++age;
            if (age == 1) {
                Arm(ctx, *f, *Kind(i) != 0); // 生成の次の tick = ここで初めて書ける
                continue;
            }
            MyeVec3 v = {};
            api->GetVelocity(api->engine, *f, &v);
            const float sp2 = v.x * v.x + v.y * v.y + v.z * v.z;
            const bool landed = (age > kArmTicks) && (sp2 < kLandSpeed * kLandSpeed);
            if (!landed && age < kMaxFlightTicks) {
                continue;
            }
            const bool bottle = (*Kind(i) != 0);
            // 順序は WavePinger と同じ — **大きさは最後**。エンジンは
            // pendingLoudness > 0 を発音の合図に見ているので、先に書くと半端に鳴る
            MyeSetField(ctx, *f, kCompEmitter, kFieldTicksPerRing, int32_t{ 2 });
            MyeSetField(ctx, *f, kCompEmitter, kFieldTone, int32_t{ bottle ? 3 : 2 });
            MyeSetField(ctx, *f, kCompEmitter, kFieldRadius,
                        bottle ? kBottleRadiusM : kStoneRadiusM);
            MyeSetField(ctx, *f, kCompEmitter, kFieldLoudness,
                        bottle ? kBottleLoudness : kStoneLoudness);
            if (bottle) {
                // 割れる (企画 §5)。★破棄は tick 末 (フェーズ 7) で、音響フェーズ (3.4) の
                //   **後**なので、この tick の波はちゃんと立つ
                api->DestroyGameObject(api->engine, *f);
            }
            *f = {};
            age = 0;
        }
    }

    // 生成の**次の** tick に呼ぶ仕込み。コンポーネントが実在するのはここから
    void Arm(MyeUpdateContext& ctx, MyeEntityId e, bool bottle)
    {
        const MyeEngineApi* api = ctx.api;
        MyeSetField(ctx, e, kCompRigidbody, kFieldMass, bottle ? 0.6f : 0.35f);
        // ★反発 0。M65c で「反発 0.5 の箱が 5 tick 周期のリミットサイクルに入って
        //   900 tick で 151 回鳴った」を踏んでいる。跳ねる投擲物は永久音源になる
        MyeSetField(ctx, e, kCompRigidbody, kFieldRestitution, 0.0f);
        MyeSetField(ctx, e, kCompEmitter, kFieldCooldown, int32_t{ 30 });
        // 射出方向は**投げた本人の今の向き**から取り直す (1 tick ぶんのずれは
        // 数ミリラジアンで絵にも当たりにも出ない)。方向を持ち越すフィールドを
        // 3 本増やすより安い — 登録フィールドは 16 本しか無い
        MyeVec3 f = Forward(ctx);
        api->SetVelocity(api->engine, e,
                         { f.x * throwSpeed, f.y * throwSpeed + throwLift, f.z * throwSpeed });
    }

    // 体の回転から前方単位ベクトルを取る (角度を持っているのは WatcherFpsCamera だけ)。
    // fwd = (2(xz+wy), 2(yz-wx), 1-2(x^2+y^2))
    MyeVec3 Forward(MyeUpdateContext& ctx)
    {
        MyeQuat q = {};
        ctx.api->GetLocalRotation(ctx.api->engine, ctx.self, &q);
        float fx = 2.0f * (q.x * q.z + q.w * q.y);
        float fy = 2.0f * (q.y * q.z - q.w * q.x);
        float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
        const float len2 = fx * fx + fy * fy + fz * fz;
        if (len2 <= 1e-6f) {
            return { 0.0f, 0.0f, 1.0f };
        }
        const float inv = 1.0f / std::sqrt(len2); // sqrt は IEEE-754 で正しく丸められる
        return { fx * inv, fy * inv, fz * inv };
    }

    bool Throw(MyeUpdateContext& ctx, int32_t kind)
    {
        const MyeEngineApi* api = ctx.api;
        int32_t freeSlot = -1;
        for (int32_t i = 0; i < 3; ++i) {
            if (MyeEntityIdIsNull(*Fly(i))) {
                freeSlot = i;
                break;
            }
        }
        if (freeSlot < 0) {
            return false; // 3 本追いかけている間は投げられない
        }
        const MyeVec3 f = Forward(ctx);
        MyeVec3 pos = {};
        api->GetLocalPosition(api->engine, ctx.self, &pos);
        const MyeVec3 spawn = { pos.x + f.x * kMuzzleAheadM,
                                pos.y + kEyeHeight + f.y * kMuzzleAheadM,
                                pos.z + f.z * kMuzzleAheadM };
        const bool bottle = (kind != 0);
        const MyeEntityId e =
            api->CreateGameObject(api->engine, bottle ? "Watcher Bottle" : "Watcher Stone");
        if (MyeEntityIdIsNull(e)) {
            return false;
        }
        // ★半径は**スケールだけで決める**。Collider の既定は球 (shape 0) の radius 0.5 で、
        //   ワールドスケールが掛かるので scale 2r なら実効半径は r ちょうど。
        //   radius を書くと二重に掛かって「ぶつからない粒」になる
        const float r = bottle ? 0.16f : 0.12f;
        api->SetLocalPosition(api->engine, e, spawn);
        api->SetLocalScale(api->engine, e, { r * 2.0f, r * 2.0f, r * 2.0f });
        api->SetMeshRenderer(api->engine, e, "builtin://sphere",
                             bottle ? "adem_bottle" : "adem_stone");
        api->AddComponentByName(api->engine, e, "Collider");
        api->AddComponentByName(api->engine, e, "Rigidbody");
        api->AddComponentByName(api->engine, e, "AcousticEmitter");
        // ★★**ここで質量も速度も書かない**。スクリプトから足したコンポーネントは
        //   tick 末まで存在しないので (EngineAPI.h v14 の明文)、同じ tick に書いても
        //   1 バイトも入らない。実測: SetVelocity が 0 を返し、石は投げた場所へ
        //   ぽとりと落ちるだけだった。仕込みは次 tick の Arm() でやる
        *Fly(freeSlot) = e;
        *Age(freeSlot) = 0;
        *Kind(freeSlot) = kind;
        return true;
    }
};
REGISTER_SCRIPT(WatcherThrowTool,
                FIELDS(stones, bottles, cooldown, prevStoneKey, prevBottleKey, throwSpeed,
                       throwLift, fly0, fly1, fly2, age0, age1, age2, kind0, kind1, kind2));
