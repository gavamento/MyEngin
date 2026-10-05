//====================================================================================
//                          PerceptionSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          AI の知覚 (AIPerception / AIStimulusSource) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Perception/PerceptionSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Perception/PerceptionSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Script/EngineApiTable.h"

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kEye = 1.6f; // 見る側の目の高さ = 見られる点の高さ (視線を水平にする)

struct Checker {
    int failCount = 0;
    void Check(bool cond, const char* what)
    {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    }
};

// 知覚 -> Transform の最小の tick (TickRunner のフェーズ 3.4a と 4 だけ)
struct Sim {
    explicit Sim(Scene& s) : scene(s) {}
    Scene& scene;
    PerceptionSystem perception;
    TransformSystem transforms;
    std::vector<SolidContact> contacts;
    uint64_t tick = 1; // tick 0 を避ける (AcousticListener の「まだ聞いていない」と区別しやすく)

    World& GetWorld() { return scene.GetWorld(); }
    void Prepare()
    {
        GetWorld().ApplyStructuralChanges();
        transforms.Update(GetWorld());
    }
    void Step()
    {
        perception.Update(GetWorld(), tick, kDt, contacts);
        GetWorld().ApplyStructuralChanges();
        transforms.Update(GetWorld());
        ++tick;
    }
};

// +Z を向いた見る側 (yawDeg で回す)
EntityID AddObserver(Scene& scene, float x, float z, float yawDeg = 0.0f)
{
    GameObject go = scene.CreateGameObjectTracked("Observer");
    go.SetLocalPosition(x, 0.0f, z);
    go.SetLocalRotationEuler(0.0f, yawDeg, 0.0f);
    auto* p = go.AddComponent<AIPerceptionComponent>();
    p->sightRadius = 10.0f;
    p->loseSightRadius = 12.0f;
    p->fovDeg = 90.0f;
    p->eyeHeight = kEye;
    p->autoSuccessRange = 0.5f;
    return go.Id();
}

EntityID AddTarget(Scene& scene, float x, float z, int faction = 0)
{
    GameObject go = scene.CreateGameObjectTracked("Target");
    go.SetLocalPosition(x, 0.0f, z);
    auto* s = go.AddComponent<AIStimulusSourceComponent>();
    s->faction = faction;
    s->targetHeight = kEye;
    return go.Id();
}

EntityID AddWall(Scene& scene, float x, float z, float hx, float hz, bool trigger = false, int32_t layer = 0)
{
    GameObject go = scene.CreateGameObjectTracked("Wall");
    go.SetLocalPosition(x, 1.5f, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = collidershape::kBox;
    col->halfExtents = { hx, 1.5f, hz };
    col->isTrigger = trigger;
    col->layer = layer;
    return go.Id();
}

void Move(World& world, EntityID e, float x, float z)
{
    auto* t = world.GetComponent<LocalTransform>(e);
    t->position = { x, 0.0f, z };
}

const AIPercept* Find(World& world, EntityID observer, EntityID target)
{
    const auto* p = world.GetComponent<AIPerceptionComponent>(observer);
    for (int i = 0; i < p->perceivedCount; ++i) {
        if (p->percepts[i].target == target) {
            return &p->percepts[i];
        }
    }
    return nullptr;
}

bool Sees(World& world, EntityID observer, EntityID target)
{
    const AIPercept* q = Find(world, observer, target);
    return q != nullptr && (q->currentSenses & perceptionsense::kSight) != 0;
}

bool SensedNow(World& world, EntityID observer, EntityID target, uint32_t sense)
{
    const AIPercept* q = Find(world, observer, target);
    return q != nullptr && (q->currentSenses & sense) != 0;
}

// 1 体を見る側の前に置いて 1 tick 回し、見えたかを返す
bool SeesAt(float tx, float tz, void (*setup)(Scene&, EntityID observer) = nullptr)
{
    Scene scene;
    const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
    const EntityID tgt = AddTarget(scene, tx, tz);
    if (setup != nullptr) {
        setup(scene, obs);
    }
    Sim sim(scene);
    sim.Prepare();
    sim.Step();
    return Sees(sim.GetWorld(), obs, tgt);
}

} // namespace

bool RunPerceptionSelfTest()
{
    MYE_LOG_INFO("==== Perception (AIPerception / AIStimulusSource) self test ====");
    Checker ck;

    // ---- 1. 視覚: 距離と視野角 ----
    ck.Check(SeesAt(0.0f, 5.0f), "正面 5 m の相手が見える");
    ck.Check(!SeesAt(0.0f, 11.0f), "見える距離 (10 m) の外は見えない");
    ck.Check(SeesAt(3.0f, 4.0f), "視野角 90 度の内側 (約 37 度) は見える");
    ck.Check(!SeesAt(5.0f, 1.0f), "視野角の外 (約 79 度) は見えない");
    ck.Check(!SeesAt(0.0f, -3.0f), "真後ろは見えない");
    ck.Check(SeesAt(0.0f, -0.4f), "必ず気付く距離 (0.5 m) 以内なら真後ろでも見える");
    ck.Check(SeesAt(0.0f, -3.0f, [](Scene& s, EntityID o) {
                 s.GetWorld().GetComponent<AIPerceptionComponent>(o)->fovDeg = 360.0f;
             }),
             "視野角 360 度なら真後ろも見える");
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f, 180.0f);
        const EntityID front = AddTarget(scene, 0.0f, -5.0f);
        const EntityID back = AddTarget(scene, 0.0f, 5.0f);
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        ck.Check(Sees(sim.GetWorld(), obs, front) && !Sees(sim.GetWorld(), obs, back),
                 "180 度回した見る側は -Z の相手が見え、+Z の相手は見えない (前方 = ワールド行列の +Z)");
    }

    // ---- 2. 視覚: 壁 ----
    ck.Check(!SeesAt(0.0f, 6.0f, [](Scene& s, EntityID) { AddWall(s, 0.0f, 3.0f, 2.0f, 0.2f); }),
             "間に壁があると見えない");
    ck.Check(SeesAt(0.0f, 6.0f, [](Scene& s, EntityID) { AddWall(s, 0.0f, 3.0f, 2.0f, 0.2f, true); }),
             "トリガーは視線を遮らない");
    ck.Check(SeesAt(0.0f, 6.0f, [](Scene& s, EntityID o) {
                 AddWall(s, 0.0f, 3.0f, 2.0f, 0.2f, false, 5);
                 s.GetWorld().GetComponent<AIPerceptionComponent>(o)->losLayerMask = ~(1u << 5);
             }),
             "視線を遮るレイヤーに入っていない壁は遮らない");
    ck.Check(SeesAt(0.0f, 6.0f, [](Scene& s, EntityID o) {
                 // 自分の体のコライダー (目を包むカプセル) は視線を遮らない
                 auto* col = s.GetWorld().AddComponent<ColliderComponent>(o);
                 col->shape = collidershape::kCapsule;
                 col->radius = 0.4f;
                 col->height = 4.0f;
             }),
             "自分の体のコライダーは視線を遮らない");
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID tgt = AddTarget(scene, 0.0f, 6.0f);
        {
            auto* col = scene.GetWorld().AddComponent<ColliderComponent>(tgt);
            col->shape = collidershape::kCapsule;
            col->radius = 0.4f;
            col->height = 4.0f;
        }
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        ck.Check(Sees(sim.GetWorld(), obs, tgt), "相手の体のコライダーは視線を遮らない");
    }

    // ---- 3. 見失う距離 ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID tgt = AddTarget(scene, 0.0f, 9.0f);
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        const bool first = Sees(sim.GetWorld(), obs, tgt);
        Move(sim.GetWorld(), tgt, 0.0f, 11.0f);
        sim.Step(); // Transform が新しい位置を確定
        sim.Step();
        const bool kept = Sees(sim.GetWorld(), obs, tgt);
        Move(sim.GetWorld(), tgt, 0.0f, 13.0f);
        sim.Step();
        sim.Step();
        const bool lost = !Sees(sim.GetWorld(), obs, tgt);
        ck.Check(first && kept && lost, "見えている相手は見失う距離 (12 m) まで追い続け、その外で見失う");

        // 見失った後に 11 m へ戻っても、見える距離 (10 m) に入るまでは見つからない
        Move(sim.GetWorld(), tgt, 0.0f, 11.0f);
        sim.Step();
        sim.Step();
        ck.Check(!Sees(sim.GetWorld(), obs, tgt), "見失った相手は見える距離に入るまで見つからない");
    }

    // ---- 4. 陣営 ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID enemy = AddTarget(scene, -1.0f, 5.0f, 0);
        const EntityID friendTgt = AddTarget(scene, 0.0f, 5.0f, 1); // 見る側の既定 faction 1 と同じ
        const EntityID neutral = AddTarget(scene, 1.0f, 5.0f, 2);
        scene.GetWorld().GetComponent<AIPerceptionComponent>(obs)->hostileMask = 1u << 0;
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        World& w = sim.GetWorld();
        ck.Check(Sees(w, obs, enemy) && !Sees(w, obs, friendTgt) && !Sees(w, obs, neutral),
                 "既定では敵の陣営だけに気付く (味方・中立は無視)");
        auto* p = w.GetComponent<AIPerceptionComponent>(obs);
        p->detectFriendlies = true;
        p->detectNeutrals = true;
        sim.Step();
        ck.Check(Sees(w, obs, enemy) && Sees(w, obs, friendTgt) && Sees(w, obs, neutral),
                 "味方・中立に気付く設定なら全員に気付く");
        ck.Check(PerceptionAttitudeOf(*p, 1) == PerceptionAttitude::Friendly
                     && PerceptionAttitudeOf(*p, 0) == PerceptionAttitude::Hostile
                     && PerceptionAttitudeOf(*p, 2) == PerceptionAttitude::Neutral
                     && PerceptionAttitudeOf(*p, 99) == PerceptionAttitude::Neutral,
                 "態度: 同じ陣営は味方、hostileMask のビットは敵、それ以外 (範囲外を含む) は中立");
    }

    // ---- 5. 記憶と予測 ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID tgt = AddTarget(scene, -2.0f, 5.0f);
        auto* p = scene.GetWorld().GetComponent<AIPerceptionComponent>(obs);
        p->forgetTicks = 120;
        p->predictionTicks = 30;
        p->fovDeg = 160.0f;
        Sim sim(scene);
        sim.Prepare();
        // 右へ 3 m/s で歩く (1 tick 0.05 m)
        constexpr float kSpeed = 3.0f;
        float x = -2.0f;
        bool sawWhileWalking = true;
        for (int i = 0; i < 30; ++i) {
            sim.Step();
            sawWhileWalking = sawWhileWalking && Sees(sim.GetWorld(), obs, tgt);
            x += kSpeed * kDt;
            Move(sim.GetWorld(), tgt, x, 5.0f);
        }
        ck.Check(sawWhileWalking, "歩いている相手を見続ける");
        const AIPercept* q = Find(sim.GetWorld(), obs, tgt);
        ck.Check(q != nullptr && std::fabs(q->velocity.x - kSpeed) < 0.01f && std::fabs(q->velocity.z) < 0.01f,
                 "続けて見た位置から速度 (3 m/s、+X) を求める");
        // 遠くへ消える (見える距離の外へ瞬間移動) — 予測は最後の速度で進む
        Move(sim.GetWorld(), tgt, 50.0f, 50.0f);
        sim.Step();
        sim.Step(); // Transform の確定 + 見失った tick
        q = Find(sim.GetWorld(), obs, tgt);
        const bool lostButRemembered = q != nullptr && (q->currentSenses & perceptionsense::kSight) == 0;
        ck.Check(lostButRemembered, "見失っても記憶している");
        const DirectX::XMFLOAT3 last = q != nullptr ? q->lastSensedPos : DirectX::XMFLOAT3{};
        for (int i = 0; i < 40; ++i) {
            sim.Step();
        }
        q = Find(sim.GetWorld(), obs, tgt);
        const float expect = last.x + kSpeed * 30.0f * kDt; // predictionTicks (30) で頭打ち
        ck.Check(q != nullptr && std::fabs(q->predictedPos.x - expect) < 1e-3f && std::fabs(q->predictedPos.z - last.z) < 1e-3f,
                 "予測位置 = 最後の位置 + 速度 x predictionTicks (それ以上は進めない)");
        for (int i = 0; i < 90; ++i) {
            sim.Step();
        }
        ck.Check(Find(sim.GetWorld(), obs, tgt) == nullptr, "forgetTicks (120) を過ぎると忘れる");
    }
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID tgt = AddTarget(scene, 0.0f, 5.0f);
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        const bool seen = Sees(sim.GetWorld(), obs, tgt);
        sim.GetWorld().DestroyEntity(tgt);
        sim.Step(); // tick 末で破棄
        sim.Step();
        ck.Check(seen && Find(sim.GetWorld(), obs, tgt) == nullptr
                     && sim.GetWorld().GetComponent<AIPerceptionComponent>(obs)->perceivedCount == 0,
                 "消えた相手は即座に忘れる (古いハンドルを残さない)");
    }

    // ---- 6. スロットの上限 ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        std::vector<EntityID> targets;
        for (int i = 0; i < 10; ++i) {
            targets.push_back(AddTarget(scene, -1.0f + 0.2f * static_cast<float>(i), 2.0f + 0.5f * static_cast<float>(i)));
        }
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        const auto* p = sim.GetWorld().GetComponent<AIPerceptionComponent>(obs);
        bool nearest8 = p->perceivedCount == kMaxPercepts && p->seenCount == kMaxPercepts;
        for (int i = 0; i < 10; ++i) {
            nearest8 = nearest8 && (Sees(sim.GetWorld(), obs, targets[static_cast<size_t>(i)]) == (i < 8));
        }
        ck.Check(nearest8, "10 体見えても記録は 8 件 (近い順)");
    }

    // ---- 7. 聴覚 (Distance モード) ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID player = AddTarget(scene, 0.0f, -6.0f);  // 背後 = 見えない
        const EntityID ally = AddTarget(scene, 0.0f, -6.0f, 1); // 見る側と同じ陣営
        Sim sim(scene);
        sim.Prepare();
        World& w = sim.GetWorld();
        const float at[3] = { 0.0f, kEye, -5.0f };
        ck.Check(PerceptionReportNoise(w, at, 1.0f, 10.0f, player) == 1, "5 m 先の音 (到達 10 m) は聞こえる");
        const auto* p = w.GetComponent<AIPerceptionComponent>(obs);
        ck.Check(std::fabs(p->pendingNoiseStrength - 0.5f) < 1e-5f, "音量は距離で減衰する (1 - 5/10 = 0.5)");
        sim.Step();
        const AIPercept* q = Find(w, obs, player);
        ck.Check(q != nullptr && (q->currentSenses & perceptionsense::kHearing) != 0
                     && (q->currentSenses & perceptionsense::kSight) == 0 && std::fabs(q->lastSensedPos.z + 5.0f) < 1e-5f,
                 "聞こえた音は鳴らした者の知覚になり、位置は音の位置");
        ck.Check(p->pendingNoiseStrength == 0.0f, "保留欄は消費される");
        const float far[3] = { 0.0f, kEye, -25.0f };
        ck.Check(PerceptionReportNoise(w, far, 5.0f, 100.0f, player) == 0, "聞こえる距離 (20 m) より遠い音は聞こえない");
        ck.Check(PerceptionReportNoise(w, at, 1.0f, 10.0f, ally) == 0, "味方の出した音は (味方に気付かない設定では) 聞こえない");
        ck.Check(PerceptionReportNoise(w, at, 1.0f, 10.0f, obs) == 0, "自分の音は聞こえない");
        const float near[3] = { 0.0f, kEye, -9.8f };
        ck.Check(PerceptionReportNoise(w, near, 1.0f, 10.0f, player) == 0, "閾値 (0.05) 未満まで減衰した音は聞こえない");
        // 名乗らない音 (石など) は hearUnaffiliated で決まる
        GameObject stone = scene.CreateGameObjectTracked("Stone");
        w.ApplyStructuralChanges();
        ck.Check(PerceptionReportNoise(w, at, 1.0f, 10.0f, stone.Id()) == 1, "所属の無い音も既定では聞こえる");
        w.GetComponent<AIPerceptionComponent>(obs)->hearUnaffiliated = false;
        w.GetComponent<AIPerceptionComponent>(obs)->pendingNoiseStrength = 0.0f;
        ck.Check(PerceptionReportNoise(w, at, 1.0f, 10.0f, stone.Id()) == 0, "hearUnaffiliated を切ると所属の無い音は聞こえない");
        // 強い方が残る
        w.GetComponent<AIPerceptionComponent>(obs)->hearUnaffiliated = true;
        const float closer[3] = { 0.0f, kEye, -2.0f };
        PerceptionReportNoise(w, at, 1.0f, 10.0f, player);
        PerceptionReportNoise(w, closer, 1.0f, 10.0f, stone.Id());
        PerceptionReportNoise(w, at, 1.0f, 10.0f, player);
        ck.Check(w.GetComponent<AIPerceptionComponent>(obs)->pendingNoiseSource == stone.Id(),
                 "同じ tick に複数の音が届いたら強い方が残る");
    }

    // ---- 8. 聴覚 (Acoustic モード) ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID player = AddTarget(scene, 4.0f, -4.0f);
        scene.GetWorld().GetComponent<AIPerceptionComponent>(obs)->hearingMode = hearingmode::kAcoustic;
        scene.GetWorld().AddComponent<AcousticListenerComponent>(obs);
        Sim sim(scene);
        sim.Prepare();
        World& w = sim.GetWorld();
        auto* ear = w.GetComponent<AcousticListenerComponent>(obs);
        ear->lastHeardTick = sim.tick; // 音響 (フェーズ 3.4) が同じ tick に配った体にする
        ear->lastHeardPos = { 3.0f, 0.0f, -3.0f };
        ear->lastLoudness = 0.4f;
        ear->lastSourceEntity = player;
        const float at[3] = { 0.0f, kEye, -1.0f };
        ck.Check(PerceptionReportNoise(w, at, 1.0f, 10.0f, player) == 0, "Acoustic モードは ReportNoise を距離で判定しない");
        sim.Step();
        const AIPercept* q = Find(w, obs, player);
        ck.Check(q != nullptr && (q->currentSenses & perceptionsense::kHearing) != 0 && std::fabs(q->lastSensedPos.x - 3.0f) < 1e-5f
                     && std::fabs(q->strength - 0.4f) < 1e-5f,
                 "Acoustic モードは耳に届いた音 (推定位置・音量・音源) を聞く");
        sim.Step();
        q = Find(w, obs, player);
        ck.Check(q != nullptr && q->currentSenses == 0, "耳に届いたのが前の tick なら今の tick は聞いていない (記憶だけ残る)");
    }

    // ---- 9. ダメージと接触 ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID attacker = AddTarget(scene, 0.0f, -8.0f); // 背後
        Sim sim(scene);
        sim.Prepare();
        World& w = sim.GetWorld();
        ck.Check(PerceptionReportDamage(w, obs, attacker, 10.0f, nullptr), "ダメージを知らせられる");
        ck.Check(!PerceptionReportDamage(w, attacker, obs, 10.0f, nullptr), "AIPerception の無い相手には知らせられない");
        sim.Step();
        const AIPercept* q = Find(w, obs, attacker);
        ck.Check(q != nullptr && (q->currentSenses & perceptionsense::kDamage) != 0 && std::fabs(q->lastSensedPos.z + 8.0f) < 1e-5f
                     && q->strength == 10.0f,
                 "見えない攻撃者をダメージで知覚し、位置は攻撃者の位置");
    }
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID player = AddTarget(scene, 0.0f, -0.62f); // 背後 0.62 m (半径の和 0.6 + 余裕 0.05 以内)
        const EntityID farOne = AddTarget(scene, 0.0f, -0.8f);
        scene.GetWorld().GetComponent<AIPerceptionComponent>(obs)->autoSuccessRange = 0.0f;
        scene.GetWorld().AddComponent<CharacterControllerComponent>(obs);
        scene.GetWorld().AddComponent<CharacterControllerComponent>(player);
        scene.GetWorld().AddComponent<CharacterControllerComponent>(farOne);
        Sim sim(scene);
        sim.Prepare();
        sim.Step();
        ck.Check(SensedNow(sim.GetWorld(), obs, player, perceptionsense::kTouch)
                     && !SensedNow(sim.GetWorld(), obs, player, perceptionsense::kSight)
                     && !SensedNow(sim.GetWorld(), obs, farOne, perceptionsense::kTouch),
                 "背後で触れた CharacterController を接触で知覚する (離れていれば知覚しない)");
    }

    // ---- 10. スクリプト API (ABI v25): EngineApi のテーブル越しに呼ぶ ----
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        const EntityID seen = AddTarget(scene, 0.0f, 5.0f);
        const EntityID hidden = AddTarget(scene, 0.0f, -6.0f);
        const EntityID wallTarget = AddTarget(scene, 3.0f, 6.0f);
        AddWall(scene, 1.5f, 3.0f, 0.5f, 0.2f);
        Sim sim(scene);
        sim.Prepare();
        ScriptApiContext sctx;
        sctx.scene = &scene;
        sctx.tickIndex = sim.tick;
        MyeEngineApi api;
        BuildEngineApi(api, &sctx);
        const auto id = [](EntityID e) { return MyeEntityId{ e.index, e.generation }; };
        ck.Check(api.PerceptionCanSee(api.engine, id(obs), id(seen)) == 1
                     && api.PerceptionCanSee(api.engine, id(obs), id(hidden)) == 0
                     && api.PerceptionCanSee(api.engine, id(obs), id(wallTarget)) == 0
                     && api.PerceptionCanSee(api.engine, id(obs), id(obs)) == 0,
                 "(API) PerceptionCanSee: 正面は見え、背後・壁の陰・刺激源の無い相手は見えない");
        ck.Check(api.PerceptionGetCount(api.engine, id(obs)) == 0, "(API) まだ知覚のフェーズを回していなければ 0 件");
        ck.Check(api.PerceptionReportNoise(api.engine, MyeVec3{ 0.0f, kEye, -4.0f }, 1.0f, 10.0f, id(hidden)) == 1,
                 "(API) PerceptionReportNoise: 背後の音が聞こえる");
        ck.Check(api.PerceptionReportDamage(api.engine, id(obs), id(wallTarget), 3.0f, MyeVec3{}) == 1
                     && api.PerceptionReportDamage(api.engine, id(seen), id(obs), 3.0f, MyeVec3{}) == 0,
                 "(API) PerceptionReportDamage: AIPerception を持つ相手にだけ知らせられる");
        sim.Step();
        MyePercept got[3] = {};
        const int32_t n = api.PerceptionGetCount(api.engine, id(obs));
        bool all = n == 3;
        for (int32_t i = 0; i < n && i < 3; ++i) {
            all = all && api.PerceptionGet(api.engine, id(obs), i, &got[i]) == 1;
        }
        // 並びは相手のエンティティキー順 = 追加した順 (seen, hidden, wallTarget)
        ck.Check(all && got[0].target.index == seen.index && got[0].currentSenses == 1
                     && got[1].target.index == hidden.index && got[1].currentSenses == 2
                     && got[2].target.index == wallTarget.index && got[2].currentSenses == 4,
                 "(API) PerceptionGet: 視覚・聴覚・ダメージの 3 件がキー順に並ぶ");
        MyePercept untouched = {};
        untouched.strength = 123.0f;
        ck.Check(api.PerceptionGet(api.engine, id(obs), 3, &untouched) == 0 && untouched.strength == 123.0f
                     && api.PerceptionGet(api.engine, id(seen), 0, &untouched) == 0
                     && api.PerceptionGetCount(api.engine, id(seen)) == 0,
                 "(API) 範囲外・AIPerception 非所持は 0 で、出力を触らない");
    }

    // ---- 11. AIPerception が無いシーンを動かさない / スナップショット ----
    {
        Scene scene;
        AddTarget(scene, 0.0f, 3.0f);
        AddWall(scene, 0.0f, 1.0f, 1.0f, 0.2f);
        Sim sim(scene);
        sim.Prepare();
        const uint64_t before = HashWorld(sim.GetWorld());
        const uint64_t rngBefore = sim.GetWorld().Rng().State();
        sim.perception.Update(sim.GetWorld(), sim.tick, kDt, sim.contacts);
        ck.Check(HashWorld(sim.GetWorld()) == before && sim.GetWorld().Rng().State() == rngBefore
                     && sim.perception.Stats().perceivers == 0,
                 "AIPerception が無ければハッシュも RNG も動かない");
    }
    {
        Scene scene;
        const EntityID obs = AddObserver(scene, 0.0f, 0.0f);
        AddObserver(scene, 4.0f, 0.0f, 270.0f);
        const EntityID tgt = AddTarget(scene, -3.0f, 6.0f);
        AddWall(scene, 1.0f, 6.0f, 0.5f, 0.5f);
        Sim sim(scene);
        sim.Prepare();
        auto walk = [&](uint64_t t) {
            const float x = -3.0f + 0.06f * static_cast<float>(t % 120);
            Move(sim.GetWorld(), tgt, x, 6.0f);
            if (t % 7 == 0) {
                const float at[3] = { x, 0.0f, 6.0f };
                PerceptionReportNoise(sim.GetWorld(), at, 1.0f, 12.0f, tgt);
            }
        };
        for (int i = 0; i < 50; ++i) {
            walk(sim.tick);
            sim.Step();
        }
        SimRefs refs;
        refs.scene = &scene;
        uint64_t tickRef = sim.tick;
        refs.tickIndex = &tickRef;
        std::vector<std::byte> blob;
        ck.Check(CaptureSimSnapshot(refs, blob), "知覚の途中で撮影できる");
        const uint64_t startTick = sim.tick;
        std::vector<uint64_t> continuous;
        for (int i = 0; i < 120; ++i) {
            walk(sim.tick);
            sim.Step();
            continuous.push_back(HashWorld(sim.GetWorld()));
        }
        ck.Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "巻き戻せる");
        Sim again(scene);
        again.tick = startTick;
        again.transforms.Update(again.GetWorld());
        bool same = true;
        for (int i = 0; i < 120; ++i) {
            walk(again.tick);
            again.Step();
            same = same && HashWorld(again.GetWorld()) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(same, "巻き戻して 120 tick 進めた毎 tick のハッシュが連続実行と一致 (状態は全部コンポーネント)");
        ck.Check(again.GetWorld().GetComponent<AIPerceptionComponent>(obs)->perceivedCount > 0, "テスト中に何かを知覚している");
    }

    // ---- 12. 計測: 50 体が 50 体を見る ----
    {
        Scene scene;
        for (int i = 0; i < 50; ++i) {
            const float x = static_cast<float>(i % 10) * 3.0f - 15.0f;
            const float z = static_cast<float>(i / 10) * 3.0f - 7.5f;
            AddObserver(scene, x, z, static_cast<float>(i * 37 % 360));
            AddTarget(scene, x + 1.5f, z + 1.5f);
        }
        for (int i = 0; i < 20; ++i) {
            AddWall(scene, static_cast<float>(i % 5) * 6.0f - 12.0f, static_cast<float>(i / 5) * 5.0f - 7.0f, 0.8f, 0.3f);
        }
        Sim sim(scene);
        sim.Prepare();
        double worst = 0.0;
        double total = 0.0;
        int rays = 0;
        for (int i = 0; i < 30; ++i) {
            sim.Step();
            worst = std::max(worst, sim.perception.Stats().updateUs);
            total += sim.perception.Stats().updateUs;
            rays = sim.perception.Stats().losRays;
        }
        MYE_LOG_INFO("  [perf] 50 observers x 50 targets, 20 walls: avg %.1f us / worst %.1f us per tick, %d LOS rays",
                     total / 30.0, worst, rays);
        ck.Check(rays > 0, "計測シーンで視線のレイを撃っている");
    }

    if (ck.failCount == 0) {
        MYE_LOG_INFO("==== Perception self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Perception self test: %d FAILED ====", ck.failCount);
    }
    return ck.failCount == 0;
}

} // namespace mye
