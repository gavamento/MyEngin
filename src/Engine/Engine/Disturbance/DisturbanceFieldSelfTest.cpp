//====================================================================================
//                          DisturbanceFieldSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceFieldSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Disturbance/DisturbanceRender.h"
#include "Engine/Engine/Disturbance/DisturbanceState.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Renderer/Passes/DisturbanceFieldPass.h"

namespace mye {

namespace {

constexpr float kDt = 1.0f / 60.0f;

// 物理 (任意) → Transform → かき乱しの順に回す 1 本の sim。TickRunner のフェーズ 3〜4 と同じ並び
struct Sim {
    Scene scene;
    TransformSystem transforms;
    std::unique_ptr<PhysicsSystem> phys;

    World& W() { return scene.GetWorld(); }
    DisturbanceState& D() { return scene.Disturbance(); }
    void Flush()
    {
        W().ApplyStructuralChanges();
        transforms.Update(W());
    }
    void Step(int ticks = 1)
    {
        for (int i = 0; i < ticks; ++i) {
            if (phys) {
                phys->Update(W(), kDt);
            }
            transforms.Update(W());
            DisturbanceStep(W(), D());
            W().ApplyStructuralChanges();
        }
    }
    uint64_t Hash()
    {
        SimRefs refs;
        refs.scene = &scene;
        return HashWorld(W(), refs.HashSources());
    }
};

EntityID AddField(Sim& s, float x, float y, float z, int cellsX, int cellsZ, uint32_t channels, const char* name = "Field")
{
    GameObject go = s.scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* f = go.AddComponent<DisturbanceFieldComponent>();
    f->cellsX = cellsX;
    f->cellsZ = cellsZ;
    f->channels = channels;
    return go.Id();
}

EntityID AddSource(Sim& s, const char* name, float x, float y, float z)
{
    GameObject go = s.scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    go.AddComponent<DisturbanceSourceComponent>();
    return go.Id();
}

DisturbanceFieldComponent* FieldOf(Sim& s, EntityID e)
{
    return s.W().GetComponent<DisturbanceFieldComponent>(e);
}

DisturbanceSourceComponent* SourceOf(Sim& s, EntityID e)
{
    return s.W().GetComponent<DisturbanceSourceComponent>(e);
}

LocalTransform* TransformOf(Sim& s, EntityID e)
{
    return s.W().GetComponent<LocalTransform>(e);
}

float MaxAbsDev(const std::vector<float>& a, float rest)
{
    float m = 0.0f;
    for (float v : a) {
        m = std::max(m, std::fabs(v - rest));
    }
    return m;
}

float MaxFlow(const DisturbanceSlot& s)
{
    float m = 0.0f;
    for (size_t i = 0; i < s.flowX.size(); ++i) {
        m = std::max(m, std::sqrt(s.flowX[i] * s.flowX[i] + s.flowZ[i] * s.flowZ[i]));
    }
    return m;
}

bool SameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool SameSlot(const DisturbanceSlot& a, const DisturbanceSlot& b)
{
    return a.entity == b.entity && a.cellsX == b.cellsX && a.cellsZ == b.cellsZ && a.channels == b.channels
           && std::memcmp(a.originCell, b.originCell, sizeof(a.originCell)) == 0 && a.baseY == b.baseY
           && SameBits(a.flowX, b.flowX) && SameBits(a.flowZ, b.flowZ) && SameBits(a.density, b.density)
           && SameBits(a.trail, b.trail) && SameBits(a.ripple, b.ripple) && SameBits(a.ripplePrev, b.ripplePrev);
}

// 静止状態: 流れ 0、濃さ rest、跡 0、波紋 0 (ビットで比べる)
bool IsRestSlot(const DisturbanceSlot& s, float rest)
{
    const std::vector<float> zero(s.CellCount(), 0.0f);
    const std::vector<float> dense(s.CellCount(), rest);
    return SameBits(s.flowX, zero) && SameBits(s.flowZ, zero)
           && (s.density.empty() || SameBits(s.density, dense)) && (s.trail.empty() || SameBits(s.trail, zero))
           && (s.ripple.empty() || SameBits(s.ripple, zero)) && (s.ripplePrev.empty() || SameBits(s.ripplePrev, zero));
}

// 点 pos・半径 radius・流れの向き dir・各チャネルを持つインパルス
FieldImpulse MakeImpulse(float x, float y, float z, float radius, float dirX, float dirZ, float speed)
{
    FieldImpulse imp;
    imp.pos[0] = x;
    imp.pos[1] = y;
    imp.pos[2] = z;
    imp.radius = radius;
    imp.dir[0] = dirX;
    imp.dir[1] = 0.0f;
    imp.dir[2] = dirZ;
    imp.speed = speed;
    return imp;
}

// 2 ワールド並走 (acc 9) と往復 (acc 4) が使う場面。場 2 個 (寸法違い) + 動く源 + 定常の源
struct Scenario {
    EntityID walker = kNullEntity;
};

Scenario BuildScenario(Sim& s)
{
    Scenario sc;
    const EntityID a = AddField(s, 0.0f, 0.0f, 0.0f, 64, 64, 7, "FieldA");
    const EntityID b = AddField(s, 10.0f, 0.0f, 3.0f, 32, 48, 3, "FieldB");
    sc.walker = AddSource(s, "Walker", -4.0f, 0.0f, 0.0f);
    const EntityID fan = AddSource(s, "Fan", 6.0f, 0.0f, 2.0f);
    s.Flush();
    (void)a;
    FieldOf(s, b)->cellSize = 0.5f;
    auto* w = SourceOf(s, sc.walker);
    w->radius = 1.2f;
    w->trailDepth = 0.4f;
    w->ripple = 0.05f;
    auto* f = SourceOf(s, fan);
    f->mode = 1;
    f->constantVelocity = { 0.0f, 0.0f, 1.5f };
    f->densityDelta = 0.2f;
    f->radius = 1.0f;
    return sc;
}

// 毎 tick の入力: 歩く源を動かし、いくつかの tick で step の後にインパルスを積む (LateUpdate 相当)
void DriveScenario(Sim& s, const Scenario& sc, int tick)
{
    TransformOf(s, sc.walker)->position.x = -4.0f + 0.04f * static_cast<float>(tick % 400);
    TransformOf(s, sc.walker)->position.z = 0.02f * static_cast<float>(tick % 90);
}

void LateInputs(Sim& s, int tick)
{
    if (tick % 150 == 99) {
        FieldImpulse imp = MakeImpulse(1.0f, 0.2f, 1.0f, 1.5f, 1.0f, 0.5f, 4.0f);
        imp.density = -0.6f;
        imp.trail = 0.3f;
        imp.ripple = 0.1f;
        DisturbanceQueueImpulse(s.scene, imp);
    }
    if (tick % 150 == 120) {
        FieldImpulse imp = MakeImpulse(10.0f, 0.0f, 3.0f, 2.0f, 0.0f, 0.0f, 3.0f);
        imp.shape = kFieldImpulseRadial;
        DisturbanceQueueImpulse(s.scene, imp);
    }
}

} // namespace

namespace {

// heavy = false: Fast 層 (小さい場・短い tick 数で全項目)、true: Full 層 (仕様どおりの大きさ・tick 数の重い 4 項目だけ)
bool RunDisturbanceFieldImpl(bool heavy)
{
    MYE_LOG_INFO("==== Disturbance field self test (%s) ====", heavy ? "long" : "fast");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    RegisterBuiltinComponents();

    // ---- 場の無いシーンのハッシュ列は動かない (acc 1) ----
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        GameObject go = sim->scene.CreateGameObjectTracked("Plain");
        go.SetLocalPosition(1.0f, 2.0f, 3.0f);
        sim->Flush();
        SimRefs refs;
        refs.scene = &sim->scene;
        SimSources wired = refs.HashSources();
        SimSources unwired = wired;
        unwired.disturbance = nullptr;
        check(wired.disturbance == &sim->scene.Disturbance(), "gate: SimSourcesOf wires the scene's disturbance state");
        check(HashWorld(sim->W(), wired) == HashWorld(sim->W(), unwired),
              "gate: an empty DisturbanceState does not move the world hash");
        sim->Step(5);
        check(HashWorld(sim->W(), wired) == HashWorld(sim->W(), unwired), "gate: stepping a scene without fields keeps the hash");
        check(!sim->D().HasHashableState(), "gate: a scene without fields has no hashable state");
        // 場を置くと節が畳まれる (ゲートが開かない故障を拾う)
        AddField(*sim, 0.0f, 0.0f, 0.0f, 16, 16, 1);
        sim->Flush();
        sim->Step();
        check(HashWorld(sim->W(), wired) != HashWorld(sim->W(), unwired), "gate: a field makes the section fold");
    }

    // ---- 源もインパルスも無い場は 600 tick 静止状態とビット一致 (acc 2) ----
    {
        const int side = heavy ? 128 : 32;
        const int ticks = heavy ? 600 : 120;
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 3.3f, 0.5f, -2.1f, side, side, 7);
        sim->Flush();
        sim->Step();
        check(sim->D().Slots().size() == 1 && sim->D().Slots()[0].CellCount() == static_cast<uint32_t>(side * side),
              "rest: the field is created with the requested size");
        const DisturbanceSlot first = sim->D().Slots()[0];
        check(IsRestSlot(first, 1.0f), "rest: a new field is the rest state (flow 0, density 1, trail 0, ripple 0)");
        sim->Step(ticks - 1);
        check(SameSlot(sim->D().Slots()[0], first) && IsRestSlot(sim->D().Slots()[0], 1.0f),
              "rest: without sources every array stays bit-identical to the rest state");
        (void)f;
    }

    // ---- 源を止めた後の減衰と復元 (acc 3) ----
    {
        // Fast は残し率 0.9 で 150 tick (0.9^150 < 1e-6)、Full は既定の 0.99 で 1200 tick
        const int ticks = heavy ? 1200 : 150;
        auto sim = std::make_unique<Sim>();
        const EntityID decayField = AddField(*sim, 0.0f, 0.0f, 0.0f, heavy ? 64 : 32, heavy ? 64 : 32, 7);
        const EntityID walker = AddSource(*sim, "Walker", -3.0f, 0.0f, 0.0f);
        sim->Flush();
        if (!heavy) {
            FieldOf(*sim, decayField)->densityKeep = 0.9f;
        }
        auto* src = SourceOf(*sim, walker);
        src->radius = 1.0f;
        src->trailDepth = 0.5f;
        src->ripple = 0.1f;
        src->densityDelta = -0.5f;
        for (int t = 0; t < 30; ++t) {
            TransformOf(*sim, walker)->position.x = -3.0f + 0.1f * static_cast<float>(t);
            sim->Step();
        }
        const DisturbanceSlot& s0 = sim->D().Slots()[0];
        const float dev0 = MaxAbsDev(s0.density, 1.0f);
        check(dev0 > 0.05f && MaxAbsDev(s0.trail, 0.0f) > 0.05f && MaxFlow(s0) > 0.5f,
              "decay: walking for 30 ticks clears density, leaves a trail and stirs the flow");
        sim->W().DestroyEntity(walker);
        sim->Flush();
        float prevDev = 1e9f, prevTrail = 1e9f, prevFlow = 1e9f;
        bool monotone = true;
        for (int t = 0; t < ticks; ++t) {
            sim->Step();
            const DisturbanceSlot& s = sim->D().Slots()[0];
            const float dev = MaxAbsDev(s.density, 1.0f);
            const float trail = MaxAbsDev(s.trail, 0.0f);
            const float flow = MaxFlow(s);
            monotone = monotone && dev <= prevDev && trail <= prevTrail && flow <= prevFlow;
            prevDev = dev;
            prevTrail = trail;
            prevFlow = flow;
        }
        check(monotone, "decay: |density - rest|, trail and flow speed never increase after the source stops");
        check(prevDev < 1.0e-4f, "decay: density comes back to within 1e-4 of the rest value");
        MYE_LOG_INFO("  [disturbance] after %d ticks: density dev %.3g, trail %.3g, flow %.3g", ticks,
                     static_cast<double>(prevDev), static_cast<double>(prevTrail), static_cast<double>(prevFlow));
    }
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 40, 40, 2);
        const EntityID walker = AddSource(*sim, "Walker", -2.0f, 0.0f, 0.0f);
        sim->Flush();
        FieldOf(*sim, f)->trailKeep = 1.0f;
        SourceOf(*sim, walker)->trailDepth = 0.5f;
        for (int t = 0; t < 20; ++t) {
            TransformOf(*sim, walker)->position.x = -2.0f + 0.1f * static_cast<float>(t);
            sim->Step();
        }
        sim->W().DestroyEntity(walker);
        sim->Flush();
        sim->Step();
        const std::vector<float> trail = sim->D().Slots()[0].trail;
        sim->Step(600);
        check(MaxAbsDev(trail, 0.0f) > 0.1f && SameBits(trail, sim->D().Slots()[0].trail),
              "decay: trailKeep = 1 keeps the trail unchanged for 600 ticks");
    }

    // ---- 境界 (acc 5) ----
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        // 16 x 16 マス x 0.5 m。原点 (0,0,0) なので x, z は [-4, 4)
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 16, 16, 7);
        sim->Flush();
        FieldOf(*sim, f)->cellSize = 0.5f;
        sim->Step();
        // 完全に外 / 半径 0 / 縦に離れすぎ: 状態不変
        const DisturbanceSlot before = sim->D().Slots()[0];
        FieldImpulse out = MakeImpulse(100.0f, 0.0f, 100.0f, 2.0f, 1.0f, 0.0f, 5.0f);
        out.density = -1.0f;
        out.trail = 1.0f;
        out.ripple = 1.0f;
        FieldImpulse zero = MakeImpulse(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 5.0f);
        zero.density = -1.0f;
        FieldImpulse high = MakeImpulse(0.0f, 50.0f, 0.0f, 2.0f, 1.0f, 0.0f, 5.0f);
        high.density = -1.0f;
        DisturbanceQueueImpulse(sim->scene, out);
        DisturbanceQueueImpulse(sim->scene, zero);
        DisturbanceQueueImpulse(sim->scene, high);
        sim->Step();
        check(SameSlot(sim->D().Slots()[0], before), "bounds: impulses entirely outside, with radius 0 or far above leave the state unchanged");
        // 角 (-4, -4) に半径 1: 格子内の 1/4 の円だけが変わる。変わったマスの集合を総当たりの期待と比べる
        FieldImpulse corner = MakeImpulse(-4.0f, 0.0f, -4.0f, 1.0f, 1.0f, 0.0f, 5.0f);
        corner.density = -0.5f;
        DisturbanceQueueImpulse(sim->scene, corner);
        sim->Step();
        const DisturbanceSlot& s = sim->D().Slots()[0];
        bool setMatches = true;
        int changed = 0;
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const float cx = (static_cast<float>(s.originCell[0] + x) + 0.5f) * 0.5f;
                const float cz = (static_cast<float>(s.originCell[2] + z) + 0.5f) * 0.5f;
                const float d = std::sqrt((cx + 4.0f) * (cx + 4.0f) + (cz + 4.0f) * (cz + 4.0f));
                const bool expectChange = d < 1.0f;
                const bool didChange = s.density[static_cast<size_t>(x + 16 * z)] != 1.0f;
                setMatches = setMatches && expectChange == didChange;
                changed += didChange ? 1 : 0;
            }
        }
        check(setMatches && changed > 0, "bounds: a corner impulse changes exactly the cells inside the radius and nothing else");
        // 反対の角 (半径の半分が格子の外) でも同様
        FieldImpulse farCorner = MakeImpulse(3.9f, 0.0f, 3.9f, 1.0f, -1.0f, 0.0f, 5.0f);
        farCorner.density = -0.5f;
        DisturbanceQueueImpulse(sim->scene, farCorner);
        sim->Step();
        check(sim->D().Slots()[0].flowX.size() == 256u && sim->D().Slots()[0].density.size() == 256u
                  && sim->D().Slots()[0].flowX[255] != 0.0f,
              "bounds: an impulse half outside the far corner is clipped and the arrays keep their size");
    }

    // ---- 源の速度の 3 経路と Constant の向き (acc 6) ----
    if (!heavy) {
        const auto flowDir = [](const DisturbanceSlot& s, float& sx, float& sz) {
            sx = 0.0f;
            sz = 0.0f;
            for (size_t i = 0; i < s.flowX.size(); ++i) {
                sx += s.flowX[i];
                sz += s.flowZ[i];
            }
        };
        const auto cosToX = [](float sx, float sz) {
            const float len = std::sqrt(sx * sx + sz * sz);
            return len > 0.0f ? sx / len : 0.0f;
        };
        // (a) CharacterController: moveInput で歩かせる
        {
            auto sim = std::make_unique<Sim>();
            sim->phys = std::make_unique<PhysicsSystem>();
            GameObject ground = sim->scene.CreateGameObjectTracked("Ground");
            ground.SetLocalPosition(0.0f, -0.5f, 0.0f);
            auto* col = ground.AddComponent<ColliderComponent>();
            col->shape = collidershape::kBox;
            col->halfExtents = { 20.0f, 0.5f, 20.0f };
            AddField(*sim, 0.0f, 0.0f, 0.0f, 64, 64, 1);
            GameObject ch = sim->scene.CreateGameObjectTracked("Walker");
            ch.SetLocalPosition(-6.0f, 0.9f, 0.0f);
            ch.AddComponent<CharacterControllerComponent>();
            ch.AddComponent<DisturbanceSourceComponent>();
            sim->Flush();
            sim->Step(30);
            sim->W().GetComponent<CharacterControllerComponent>(ch.Id())->moveInput.x = 2.0f;
            sim->Step(20);
            float sx = 0.0f, sz = 0.0f;
            flowDir(sim->D().Slots()[0], sx, sz);
            check(sx > 0.0f && cosToX(sx, sz) > 0.9f, "source: a walking CharacterController pushes the flow along its velocity");
        }
        // (b) 非 kinematic Rigidbody の初速
        {
            auto sim = std::make_unique<Sim>();
            AddField(*sim, 0.0f, 0.0f, 0.0f, 32, 32, 1);
            const EntityID e = AddSource(*sim, "Body", 0.0f, 0.0f, 0.0f);
            sim->Flush();
            auto* rb = sim->W().AddComponent<RigidbodyComponent>(e);
            rb->velocity = { 0.0f, 0.0f, 2.0f };
            sim->Step(3);
            float sx = 0.0f, sz = 0.0f;
            flowDir(sim->D().Slots()[0], sx, sz);
            check(sz > 0.0f && std::fabs(sx) < 0.1f * sz, "source: a dynamic Rigidbody pushes the flow along its linear velocity");
            // kinematic の剛体は速度を読まず、位置の差分に落ちる (動かさなければ流れは 0)
            sim->W().GetComponent<RigidbodyComponent>(e)->isKinematic = true;
            sim->Step(200);
            check(MaxFlow(sim->D().Slots()[0]) < 1.0e-3f, "source: a kinematic Rigidbody falls back to the position difference (still = no flow)");
        }
        // (c) どちらも無い: LocalTransform を毎 tick 動かす
        {
            auto sim = std::make_unique<Sim>();
            AddField(*sim, 0.0f, 0.0f, 0.0f, 64, 64, 1);
            const EntityID e = AddSource(*sim, "Mover", -5.0f, 0.0f, 0.0f);
            sim->Flush();
            for (int t = 0; t < 30; ++t) {
                TransformOf(*sim, e)->position.x = -5.0f + (2.0f / 60.0f) * static_cast<float>(t);
                sim->Step();
            }
            float sx = 0.0f, sz = 0.0f;
            flowDir(sim->D().Slots()[0], sx, sz);
            check(sx > 0.0f && cosToX(sx, sz) > 0.9f, "source: a moving entity without a body pushes the flow along its position difference");
            check(SourceOf(*sim, e)->prevValid == 1 && SourceOf(*sim, e)->prevPos.x == TransformOf(*sim, e)->position.x,
                  "source: prevPos is written at the end of each step");
        }
        // Constant の向きはエンティティの回転に従う
        {
            const auto constantFlow = [&](float yawDeg, float& sx, float& sz) {
                auto sim = std::make_unique<Sim>();
                AddField(*sim, 0.0f, 0.0f, 0.0f, 32, 32, 1);
                GameObject fan = sim->scene.CreateGameObjectTracked("Fan");
                fan.SetLocalRotationEuler(0.0f, yawDeg, 0.0f);
                auto* src = fan.AddComponent<DisturbanceSourceComponent>();
                src->mode = 1;
                src->constantVelocity = { 0.0f, 0.0f, 2.0f };
                src->radius = 1.5f;
                sim->Flush();
                sim->Step(5);
                flowDir(sim->D().Slots()[0], sx, sz);
            };
            float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
            constantFlow(0.0f, ax, az);
            constantFlow(90.0f, bx, bz);
            check(az > 0.0f && std::fabs(ax) < 0.01f * az, "source: a constant source blows along its local +z");
            check(bx > 0.0f && std::fabs(bz) < 0.01f * bx, "source: rotating the entity by 90 degrees rotates the flow by 90 degrees");
        }
        // 速さ係数: fullSpeed 以上で 1、半分で 0.5
        {
            const auto densityDrop = [&](float speed) {
                auto sim = std::make_unique<Sim>();
                const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 32, 32, 1);
                const EntityID e = AddSource(*sim, "Body", 0.0f, 0.0f, 0.0f);
                sim->Flush();
                FieldOf(*sim, f)->densityKeep = 1.0f;
                auto* src = SourceOf(*sim, e);
                src->fullSpeed = 4.0f;
                src->densityDelta = -0.5f;
                src->radius = 2.0f;
                auto* rb = sim->W().AddComponent<RigidbodyComponent>(e);
                rb->velocity = { speed, 0.0f, 0.0f };
                sim->Step();
                return 1.0f - *std::min_element(sim->D().Slots()[0].density.begin(), sim->D().Slots()[0].density.end());
            };
            const float full = densityDrop(4.0f);
            const float over = densityDrop(8.0f);
            const float half = densityDrop(2.0f);
            check(full > 0.0f && std::fabs(over - full) < 1e-6f, "source: the speed factor saturates at fullSpeed");
            check(std::fabs(half - 0.5f * full) < 1e-5f, "source: half of fullSpeed gives a factor of 0.5");
        }
    }

    // ---- 波紋 (acc 7) ----
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 32, 32, 4);
        sim->Flush();
        (void)f;
        sim->Step();
        FieldImpulse imp = MakeImpulse(0.0f, 0.0f, 0.0f, 0.6f, 0.0f, 0.0f, 0.0f);
        imp.ripple = 0.2f;
        DisturbanceQueueImpulse(sim->scene, imp);
        sim->Step(20);
        const DisturbanceSlot& s = sim->D().Slots()[0];
        const auto at = [&](int x, int z) { return s.ripple[static_cast<size_t>(x + 32 * z)]; };
        const float p1 = at(19, 16), p2 = at(12, 16), p3 = at(16, 19), p4 = at(16, 12);
        check(p1 != 0.0f && std::memcmp(&p1, &p2, 4) == 0 && std::memcmp(&p1, &p3, 4) == 0 && std::memcmp(&p1, &p4, 4) == 0,
              "ripple: +x, -x, +z, -z spread bit-identically from a symmetric center");
    }
    {
        const int ticks = heavy ? 10000 : 1000;
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 24, 24, 4);
        sim->Flush();
        FieldOf(*sim, f)->rippleSpeed = 1000.0f;
        sim->Step();
        FieldImpulse imp = MakeImpulse(0.3f, 0.0f, -0.4f, 1.0f, 0.0f, 0.0f, 0.0f);
        imp.ripple = 0.5f;
        DisturbanceQueueImpulse(sim->scene, imp);
        bool finite = true;
        float peak = 0.0f;
        for (int t = 0; t < ticks; ++t) {
            sim->Step();
            for (float v : sim->D().Slots()[0].ripple) {
                finite = finite && std::isfinite(v);
                peak = std::max(peak, std::fabs(v));
            }
        }
        check(finite && peak <= kMaxRippleHeight, "ripple: rippleSpeed = 1000 stays finite and bounded (10000 ticks in the long run)");
    }

    // ---- 場の寿命 (acc 8) ----
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 32, 32, 3);
        sim->Flush();
        sim->Step();
        DisturbanceQueueImpulse(sim->scene, MakeImpulse(0.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f, 4.0f));
        sim->Step();
        check(!IsRestSlot(sim->D().Slots()[0], 1.0f), "life: an impulse disturbs the field");
        FieldOf(*sim, f)->cellsX = 48;
        sim->Step();
        check(sim->D().Slots().size() == 1 && sim->D().Slots()[0].cellsX == 48 && IsRestSlot(sim->D().Slots()[0], 1.0f),
              "life: changing cellsX rebuilds the field at rest on that tick");
        // 他の欄の変更は作り直さない
        DisturbanceQueueImpulse(sim->scene, MakeImpulse(0.0f, 0.0f, 0.0f, 2.0f, 1.0f, 0.0f, 4.0f));
        sim->Step();
        FieldOf(*sim, f)->velocityKeep = 0.5f;
        sim->Step();
        check(!IsRestSlot(sim->D().Slots()[0], 1.0f), "life: changing a keep rate does not rebuild the field");
        // 破棄: スロットが消え、節が畳まれない
        SimRefs refs;
        refs.scene = &sim->scene;
        SimSources wired = refs.HashSources();
        SimSources unwired = wired;
        unwired.disturbance = nullptr;
        sim->W().DestroyEntity(f);
        sim->Flush();
        sim->Step();
        check(sim->D().Slots().empty() && !sim->D().HasHashableState() && HashWorld(sim->W(), wired) == HashWorld(sim->W(), unwired),
              "life: destroying the field drops the slot and folds no section");
    }
    {
        auto sim = std::make_unique<Sim>();
        EntityID ids[5];
        for (int i = 0; i < 5; ++i) {
            ids[i] = AddField(*sim, static_cast<float>(i) * 40.0f, 0.0f, 0.0f, 8, 8, 1);
        }
        sim->Flush();
        sim->Step(3);
        bool order = sim->D().Slots().size() == 4;
        for (size_t i = 0; order && i < 4; ++i) {
            order = sim->D().Slots()[i].entity == ids[i];
        }
        check(order, "life: only the first 4 fields (by entity index) are simulated");
        check(sim->D().OverflowWarnCount() == 1, "life: the 5th field warns exactly once");
        sim->Step(10);
        check(sim->D().OverflowWarnCount() == 1, "life: the warning is not repeated");
        // 先頭が消えたら 5 個目が繰り上がる
        sim->W().DestroyEntity(ids[0]);
        sim->Flush();
        sim->Step();
        check(sim->D().Slots().size() == 4 && sim->D().Slots()[3].entity == ids[4], "life: destroying a field promotes the 5th");
        sim->D().Clear();
        check(sim->D().Slots().empty() && sim->D().Queue().empty(), "life: Clear drops every slot and queued impulse");
    }
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        const EntityID v = AddField(*sim, 0.0f, 0.0f, 0.0f, 8, 8, 1);
        sim->Flush();
        FieldOf(*sim, v)->shape = kDisturbanceVolume;
        sim->Step(3);
        check(sim->D().Slots().empty() && sim->D().OverflowWarnCount() == 1, "life: a volume field creates no state and warns once");
    }

    // ---- キュー ----
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        bool allAccepted = true;
        for (uint32_t i = 0; i < kMaxDisturbanceQueue; ++i) {
            allAccepted = allAccepted && DisturbanceQueueImpulse(sim->scene, MakeImpulse(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f));
        }
        check(allAccepted && !DisturbanceQueueImpulse(sim->scene, MakeImpulse(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f)),
              "queue: 256 impulses fit, the 257th is refused");
        FieldImpulse bad = MakeImpulse(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f);
        bad.pos[0] = std::nanf("");
        sim->D().Queue().clear();
        check(!DisturbanceQueueImpulse(sim->scene, bad), "queue: a non-finite impulse is refused");
        DisturbanceQueueImpulse(sim->scene, MakeImpulse(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f));
        sim->Step();
        check(sim->D().Queue().empty(), "queue: a scene without fields still empties the queue on the next step");
    }

    // ---- インパルスの 3 形と読み取り ----
    if (!heavy) {
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 32, 32, 1);
        sim->Flush();
        FieldOf(*sim, f)->velocityKeep = 1.0f;
        sim->Step();
        FieldImpulse radial = MakeImpulse(0.0f, 0.0f, 0.0f, 1.5f, 0.0f, 0.0f, 3.0f);
        radial.shape = kFieldImpulseRadial;
        DisturbanceQueueImpulse(sim->scene, radial);
        sim->Step();
        const DisturbanceSlot& s = sim->D().Slots()[0];
        // 中心から +x 側のマスは +x 向き、-x 側は -x 向き
        const size_t right = static_cast<size_t>(18 + 32 * 16), left = static_cast<size_t>(13 + 32 * 16);
        check(s.flowX[right] > 0.0f && s.flowX[left] < 0.0f, "impulse: radial pushes outward");
        FieldSampleOut out;
        const DirectX::XMFLOAT3 cellCenter = { (static_cast<float>(s.originCell[0] + 18) + 0.5f) * 0.25f, 0.2f,
                                               (static_cast<float>(s.originCell[2] + 16) + 0.5f) * 0.25f };
        check(DisturbanceSampleAt(sim->D(), sim->W(), kNullEntity, cellCenter, out) && out.flow[0] == s.flowX[right] && out.field == f,
              "sample: at a cell center the bilinear value equals the cell");
        check(!DisturbanceSampleAt(sim->D(), sim->W(), kNullEntity, { 500.0f, 0.2f, 0.0f }, out) && out.flow[0] == 0.0f,
              "sample: outside the grid returns false with zeros");
        check(!DisturbanceSampleAt(sim->D(), sim->W(), kNullEntity, { 0.0f, 20.0f, 0.0f }, out), "sample: far above the layer is outside");
        FieldImpulse cone = MakeImpulse(0.0f, 0.0f, 0.0f, 3.0f, 1.0f, 0.0f, 3.0f);
        cone.shape = kFieldImpulseCone;
        cone.coneCos = 0.9f;
        sim->D().Clear();
        sim->Step();
        DisturbanceQueueImpulse(sim->scene, cone);
        sim->Step();
        const DisturbanceSlot& c = sim->D().Slots()[0];
        check(c.flowX[static_cast<size_t>(18 + 32 * 16)] > 0.0f && c.flowX[static_cast<size_t>(16 + 32 * 22)] == 0.0f,
              "impulse: a cone pushes inside the cone and leaves the sides alone");
    }

    // ---- スナップショット往復 (acc 4) ----
    if (!heavy) {
        auto a = std::make_unique<Sim>();
        const Scenario sc = BuildScenario(*a);
        a->Flush();
        for (int t = 0; t < 130; ++t) {
            DriveScenario(*a, sc, t);
            a->Step();
            LateInputs(*a, t);
        }
        // 往復の対象に、step の後に積んだ配送待ち (LateUpdate 相当) を含める
        DisturbanceQueueImpulse(a->scene, MakeImpulse(0.0f, 0.1f, 0.0f, 1.0f, 1.0f, 0.0f, 2.0f));
        DisturbanceQueueImpulse(a->scene, MakeImpulse(10.0f, 0.1f, 3.0f, 1.0f, 0.0f, 1.0f, 2.0f));
        DisturbanceQueueImpulse(a->scene, MakeImpulse(-2.0f, 0.1f, 1.0f, 1.0f, -1.0f, 0.0f, 2.0f));
        check(a->D().Slots().size() == 2 && a->D().Queue().size() >= 3, "snapshot: two fields and queued impulses before capture");
        SimRefs refsA;
        refsA.scene = &a->scene;
        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refsA, blob), "snapshot: capture succeeds");
        const uint64_t hashA = a->Hash();

        auto b = std::make_unique<Sim>();
        SimRefs refsB;
        refsB.scene = &b->scene;
        check(RestoreSimSnapshot(refsB, blob.data(), blob.size()), "snapshot: restore into a fresh scene succeeds");
        check(b->D().Slots().size() == 2 && b->D().Queue().size() == a->D().Queue().size()
                  && b->Hash() == hashA,
              "snapshot: the restored scene has the same fields, queue and world hash");
        const size_t queued = b->D().Queue().size();
        b->transforms.Update(b->W());
        a->transforms.Update(a->W());
        bool same = true;
        const auto id = [&](Sim& s, const char* name) { return s.scene.Find(name).Id(); };
        const Scenario scB{ id(*b, "Walker") };
        for (int t = 130; t < 230 && same; ++t) {
            DriveScenario(*a, sc, t);
            DriveScenario(*b, scB, t);
            a->Step();
            b->Step();
            if (t == 130) {
                same = same && b->D().Queue().empty() && queued >= 3;
            }
            LateInputs(*a, t);
            LateInputs(*b, t);
            same = same && a->Hash() == b->Hash();
        }
        check(same, "snapshot: 100 ticks after the round trip every tick hash matches, and the queue is consumed on the first step");
        // 壊れた blob は何も書き換えない
        std::vector<std::byte> broken = blob;
        const size_t cut = broken.size() - 1000;
        const uint64_t before = b->Hash();
        check(!RestoreSimSnapshot(refsB, broken.data(), cut) && b->Hash() == before, "snapshot: a truncated blob is refused and changes nothing");
    }

    // ---- 2 ワールドの並走と焼き込み (acc 9) ----
    {
        const int ticks = heavy ? 600 : 100;
        auto a = std::make_unique<Sim>();
        auto b = std::make_unique<Sim>();
        const Scenario sa = BuildScenario(*a);
        const Scenario sb = BuildScenario(*b);
        a->Flush();
        b->Flush();
        bool same = true;
        uint64_t finalHash = 0;
        for (int t = 0; t < ticks; ++t) {
            DriveScenario(*a, sa, t);
            DriveScenario(*b, sb, t);
            a->Step();
            b->Step();
            LateInputs(*a, t);
            LateInputs(*b, t);
            const uint64_t ha = a->Hash();
            same = same && ha == b->Hash();
            finalHash = ha;
        }
        MYE_LOG_INFO("  [disturbance] two-world scenario final hash @%d = %016llX", ticks,
                     static_cast<unsigned long long>(finalHash));
        check(same, "determinism: two worlds hash-match on every tick");
        if (heavy) {
            // 600 tick 後の最終ハッシュ。Debug == Release (欄や計算を変えたら理由を添えて再採取する)
            constexpr uint64_t kBakedFinalHash = 0x92713D47B7CE447Aull;
            check(finalHash == kBakedFinalHash, "determinism: the final hash at tick 600 equals the baked value (Debug == Release)");
        }
    }

    // ---- 描画への詰め替え (M96b。GPU は使わない = 転送指示の中身だけを見る) ----
    {
        auto s = std::make_unique<Sim>();
        DisturbanceFieldUpload up;
        check(BuildDisturbanceFieldUpload(s->W(), nullptr, up) == 0, "render: a scene without fields packs no layer");
        const EntityID f = AddField(*s, 10.0f, 2.0f, -4.0f, 32, 32, kDisturbanceChannelDensity);
        s->Flush();
        // 状態が無い (編集中) は定数。原点は最初の step が作る格子と同じ
        check(BuildDisturbanceFieldUpload(s->W(), &s->D(), up) == 1 && !up.layers[0].hasState
                  && up.layers[0].originCellX == 40 - 16 && up.layers[0].originCellZ == -16 - 16
                  && up.layers[0].baseY == 2.0f && up.layers[0].restDensity == 1.0f && up.layers[0].hasDensity,
              "render: before the first step the layer is a constant at the grid the step will build");
        s->Step(2);
        const DisturbanceSlot& slot = s->D().Slots()[0];
        check(BuildDisturbanceFieldUpload(s->W(), &s->D(), up) == 1 && up.layers[0].hasState
                  && up.layers[0].originCellX == slot.originCell[0] && up.layers[0].originCellZ == slot.originCell[2]
                  && up.layers[0].density == slot.density.data() && up.layers[0].trail == nullptr
                  && up.layers[0].entityIndex == f.index && up.serial == s->D().RenderSerial(),
              "render: with a state the layer points at the slot's arrays and carries the serial");
        const uint64_t serialBefore = s->D().RenderSerial();
        s->D().Clear();
        check(s->D().RenderSerial() > serialBefore, "render: Clear advances the serial so the next upload is not skipped");
        // 寸法を変えて次の step の前: スロットは使えない (作り直し待ち) ので定数に戻る
        s->Step(1);
        s->W().GetComponent<DisturbanceFieldComponent>(f)->cellsX = 48;
        check(BuildDisturbanceFieldUpload(s->W(), &s->D(), up) == 1 && !up.layers[0].hasState && up.layers[0].cellsX == 48,
              "render: a layer whose slot no longer matches the settings is drawn as a constant until the step rebuilds it");
    }

    return failCount == 0;
}

} // namespace

bool RunDisturbanceFieldSelfTest()
{
    return RunDisturbanceFieldImpl(false);
}

bool RunDisturbanceFieldLongSelfTest()
{
    return RunDisturbanceFieldImpl(true);
}

} // namespace mye
