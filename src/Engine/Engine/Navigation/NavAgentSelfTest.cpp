//====================================================================================
//                          NavAgentSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMeshAgent・dtCrowd・SimSnapshot の Nav 節の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Navigation/NavAgentSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Navigation/NavBake.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Navigation/NavSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kStepHeight = 0.3f;        // 庭の段差 = Surface.maxClimb の既定ちょうど (CC.stepOffset の既定とも同じ)
constexpr float kOverStepHeight = 0.35f;   // maxClimb を 5 cm 超える段差。経路にならない
constexpr float kRampDeg = 30.0f;          // 庭の坂。既定の Surface (maxSlopeDeg 45) で登れる
constexpr uint64_t kYardGuid = 0x4E41564147454E31ull;  // メモリ登録の GUID (ファイルを作らない)
constexpr uint64_t kOpenGuid = 0x4E41564147454E32ull;
constexpr uint64_t kFieldGuid = 0x4E41564147454E33ull;
// Debug で採取し、Release で同じ値になることを確認して焼く (docs\adr\ADR-023-navmesh.md)
constexpr uint64_t kExpectedYardHash = 0x2ABC7F449D843943ull;

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

GameObject AddBox(Scene& scene, const char* name, float x, float y, float z, float hx, float hy, float hz)
{
    GameObject go = scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* col = go.AddComponent<ColliderComponent>();
    col->shape = collidershape::kBox;
    col->halfExtents = { hx, hy, hz };
    return go;
}

// 既定の Surface 設定 (autoCellSize / maxClimb 0.3 / maxSlopeDeg 45 / tileSize)。範囲だけ庭に合わせる
// yShift: 範囲の Y をずらす量。地面の天面とボクセル境界の位置関係を変えて段差の量子化を確かめるため
EntityID AddSurface(Scene& scene, float halfX, float halfZ, float maxSlopeDeg = 45.0f, float yShift = 0.0f)
{
    GameObject go = scene.CreateGameObjectTracked("Surface");
    auto* sf = go.AddComponent<NavMeshSurfaceComponent>();
    sf->center = { 0.0f, 3.0f + yShift, 0.0f };
    sf->size = { halfX * 2.0f, 10.0f, halfZ * 2.0f };
    sf->maxSlopeDeg = maxSlopeDeg;
    return go.Id();
}

// 立っている Agent (CC のカプセルは足元 y = 地面)。dest が null なら目的地なし
EntityID AddAgent(Scene& scene, const char* name, float x, float groundY, float z, const float* dest,
                  bool withController, int avoidance = 2)
{
    GameObject go = scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, groundY + 0.9f, z);
    if (withController) {
        go.AddComponent<CharacterControllerComponent>();
    }
    auto* agent = go.AddComponent<NavMeshAgentComponent>();
    agent->avoidanceQuality = avoidance;
    if (dest != nullptr) {
        agent->destination = { dest[0], dest[1], dest[2] };
        agent->hasDestination = true;
    }
    return go.Id();
}

// 3.4b (nav) -> 3.6 (物理) -> 4 (Transform) の順に回す最小の tick
struct Sim {
    explicit Sim(Scene& s) : scene(s) {}
    Scene& scene;
    NavSystem nav;
    PhysicsSystem physics;
    TransformSystem transforms;
    uint64_t tick = 0;

    World& GetWorld() { return scene.GetWorld(); }
    void Step()
    {
        nav.Update(GetWorld(), kDt);
        physics.Update(GetWorld(), kDt);
        transforms.Update(GetWorld());
        ++tick;
    }
};

bool BakeSurface(Scene& scene, EntityID surface, uint64_t guid, int* polyCount)
{
    World& world = scene.GetWorld();
    world.ApplyStructuralChanges();
    TransformSystem transforms;
    transforms.Update(world);
    NavBakeInputs in;
    if (!NavPrepareBakeInputs(world, surface, in)) {
        return false;
    }
    const NavBakeOutput out = NavBakeAsset(in.config, in.soup, nullptr);
    if (out.status != NavBakeStatus::Ok) {
        return false;
    }
    NavMeshAsset::RegisterInMemory(guid, out.data);
    world.GetComponent<NavMeshSurfaceComponent>(surface)->navAsset = AssetID{ guid };
    if (polyCount != nullptr) {
        *polyCount = out.polyCount;
    }
    return true;
}

// 段差・坂・孤島のある庭。Agent は呼び出し側が足す
struct Yard {
    EntityID surface;
    float platformTop = 0.0f;
};

// 長さ 4 m・厚さ 0.2 m の坂 (下端の上面が地面から 0.2 * cos 浮く) と、その先の台。台の上面の高さを返す
float AddRampAndPlatform(Scene& scene, float angleDeg)
{
    constexpr float kPi = 3.14159265f;
    const float angle = angleDeg * kPi / 180.0f;
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    GameObject ramp = AddBox(scene, "Ramp", 4.0f + 2.0f * c + 0.1f * s, 2.0f * s - 0.1f * c, 0.0f, 2.0f, 0.1f, 3.0f);
    ramp.GetComponent<LocalTransform>()->rotation = { 0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f) };
    const float platformTop = 4.0f * s;
    AddBox(scene, "Platform", 9.0f, platformTop * 0.5f, 0.0f, 1.8f, platformTop * 0.5f, 3.0f);
    return platformTop;
}

Yard BuildYard(Scene& scene)
{
    AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
    // x = -5.5..-2.5 を覆う低い台 (全幅): 渡るには maxClimb ちょうどの段差を 2 回越える
    AddBox(scene, "Step", -4.0f, kStepHeight * 0.5f, 0.0f, 1.5f, kStepHeight * 0.5f, 12.0f);
    Yard yard;
    yard.platformTop = AddRampAndPlatform(scene, kRampDeg);
    // 登れない高さの孤島
    AddBox(scene, "Island", 0.0f, 1.5f, 8.0f, 2.0f, 1.5f, 2.0f);
    // maxClimb を 5 cm 超える低い台 (CC の stepOffset でも登れない高さ)
    AddBox(scene, "Deck", 6.0f, kOverStepHeight * 0.5f, -9.0f, 1.5f, kOverStepHeight * 0.5f, 1.5f);
    yard.surface = AddSurface(scene, 13.0f, 13.0f);
    return yard;
}

float DistXZ(const LocalTransform& a, const LocalTransform& b)
{
    const float dx = a.position.x - b.position.x;
    const float dz = a.position.z - b.position.z;
    return std::sqrt(dx * dx + dz * dz);
}

// CC (stepOffset を指定) が真正面から登れる段差の高さを測る。moveInput で直進させ、段の上に乗ったかを見る。
// 戻り値は登れた最大の高さ (候補の中で)
float MeasureClimb(float speed, float stepOffset, bool logTable)
{
    const float candidates[] = { 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.50f };
    float best = 0.0f;
    for (const float h : candidates) {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 20.0f, 0.5f, 5.0f);
        AddBox(scene, "Step", 6.0f, h * 0.5f, 0.0f, 3.0f, h * 0.5f, 5.0f); // 前面 x = 3
        GameObject ch = scene.CreateGameObjectTracked("Char");
        ch.SetLocalPosition(0.0f, 0.9f, 0.0f);
        ch.AddComponent<CharacterControllerComponent>()->stepOffset = stepOffset;
        scene.GetWorld().ApplyStructuralChanges();
        PhysicsSystem physics;
        TransformSystem transforms;
        auto* cc = ch.GetComponent<CharacterControllerComponent>();
        auto* lt = ch.GetComponent<LocalTransform>();
        // 段の前面 (x = 3) を 1 m 越えたところで止める (台の端 x = 9 から落ちた結果を見ない)
        for (int i = 0; i < 1200 && lt->position.x < 4.0f; ++i) { // 0.5 m/s でも段の手前から着く長さ
            cc->moveInput = { speed, 0.0f, 0.0f };
            physics.Update(scene.GetWorld(), kDt);
            transforms.Update(scene.GetWorld());
        }
        const bool climbed = lt->position.x >= 4.0f && lt->position.y > 0.9f + h - 0.05f;
        if (logTable) {
            MYE_LOG_INFO("  [climb] stepOffset %.2f speed %.1f m/s step %.2f m -> x %.2f y(feet) %.3f : %s", stepOffset,
                         speed, h, lt->position.x, lt->position.y - 0.9f, climbed ? "climbed" : "blocked");
        }
        if (climbed) {
            best = std::max(best, h);
        }
    }
    return best;
}

uint64_t WorldHashOf(Sim& sim, const NavSystem* nav)
{
    return HashWorld(sim.GetWorld(), SimSourcesOf(sim.scene, nullptr, nullptr, nullptr, nav));
}

double MicrosSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

bool RunNavAgentSelfTest()
{
    MYE_LOG_INFO("==== NavAgent (dtCrowd / CC / SimSnapshot Nav section) self test ====");
    Checker ck;

    // ---- 0. CC が越えられる段差 (Surface.maxClimb の既定値と CC.stepOffset の既定値が揃っていること) ----
    {
        const CharacterControllerComponent ccDefault;
        const NavMeshSurfaceComponent surfaceDefault;
        ck.Check(ccDefault.stepOffset == surfaceDefault.maxClimb, "CC.stepOffset の既定 == Surface.maxClimb の既定 (0.3)");
        const float climb35 = MeasureClimb(3.5f, ccDefault.stepOffset, true);
        const float climb15 = MeasureClimb(1.5f, ccDefault.stepOffset, true);
        const float climbSlow = MeasureClimb(0.5f, ccDefault.stepOffset, false);
        MYE_LOG_INFO("  [climb] max climbable step: %.2f m at 3.5 m/s, %.2f m at 1.5 m/s, %.2f m at 0.5 m/s", climb35,
                     climb15, climbSlow);
        ck.Check(climb35 >= kStepHeight && climb15 >= kStepHeight && climbSlow >= kStepHeight,
                 "既定の CC は maxClimb ちょうどの段差を速度によらず越える (テストの前提)");
        ck.Check(climb35 < kOverStepHeight + 0.01f && climb15 < kOverStepHeight + 0.01f && climbSlow < kOverStepHeight + 0.01f,
                 "...maxClimb を 5 cm 超える段差は越えない");
    }

    // ---- 1. 庭: 段差・坂・孤島・NoPath・Inactive・Idle ----
    uint64_t yardHashAtEnd = 0;
    {
        Scene scene;
        const Yard yard = BuildYard(scene);
        const float toPlatform[3] = { 9.4f, yard.platformTop, 0.0f };
        const float toIsland[3] = { 0.0f, 3.0f, 8.0f };
        const float farAway[3] = { 0.0f, 0.0f, 60.0f };
        const EntityID climber = AddAgent(scene, "Climber", -9.0f, 0.0f, -6.0f, toPlatform, true);
        const EntityID islander = AddAgent(scene, "Islander", -9.0f, 0.0f, 6.0f, toIsland, true);
        const EntityID lost = AddAgent(scene, "Lost", -9.0f, 0.0f, 0.0f, farAway, true);
        const EntityID noBody = AddAgent(scene, "NoBody", -9.0f, 0.0f, 9.0f, toPlatform, false);
        const EntityID idle = AddAgent(scene, "Idle", -9.0f, 0.0f, -9.0f, nullptr, true);
        const float toDeck[3] = { 6.0f, kOverStepHeight, -9.0f };
        const EntityID curber = AddAgent(scene, "Curber", -9.0f, 0.0f, -11.0f, toDeck, true);
        ck.Check(BakeSurface(scene, yard.surface, kYardGuid, nullptr), "庭をベイクできる");

        Sim sim(scene);
        World& world = sim.GetWorld();
        for (int i = 0; i < 1200; ++i) {
            sim.Step();
        }
        const auto* climberAgent = world.GetComponent<NavMeshAgentComponent>(climber);
        const auto* climberLt = world.GetComponent<LocalTransform>(climber);
        float dx = climberLt->position.x - toPlatform[0];
        float dz = climberLt->position.z - toPlatform[2];
        const float climberFeetY = climberLt->position.y - 0.9f;
        MYE_LOG_INFO("  [yard] climber status %d at (%.2f, %.2f, %.2f), remaining %.2f", climberAgent->status,
                     climberLt->position.x, climberFeetY, climberLt->position.z, climberAgent->remainingDistance);
        ck.Check(climberAgent->status == navagentstatus::kArrived && !climberAgent->pathPartial,
                 "段差と坂を越えて台の上の目的地へ着く (Arrived、部分経路ではない)");
        ck.Check(std::sqrt(dx * dx + dz * dz) < 0.4f && std::fabs(climberFeetY - yard.platformTop) < 0.15f,
                 "...実際に台の上の目的地の近くに立っている");

        const auto* islanderAgent = world.GetComponent<NavMeshAgentComponent>(islander);
        const auto* islanderLt = world.GetComponent<LocalTransform>(islander);
        MYE_LOG_INFO("  [yard] islander status %d pathPartial %d at (%.2f, %.2f)", islanderAgent->status,
                     islanderAgent->pathPartial ? 1 : 0, islanderLt->position.x, islanderLt->position.z);
        ck.Check(islanderAgent->status == navagentstatus::kArrived && islanderAgent->pathPartial,
                 "登れない孤島の上の目的地は部分経路で、届く限りの最寄りに着く (Arrived + pathPartial)");
        ck.Check(islanderLt->position.y - 0.9f < 0.2f && DistXZ(*islanderLt, *world.GetComponent<LocalTransform>(idle)) > 1.0f,
                 "...孤島の上には登っていない");

        const auto* curberAgent = world.GetComponent<NavMeshAgentComponent>(curber);
        const auto* curberLt = world.GetComponent<LocalTransform>(curber);
        MYE_LOG_INFO("  [yard] curber status %d pathPartial %d at (%.2f, %.2f, %.2f)", curberAgent->status,
                     curberAgent->pathPartial ? 1 : 0, curberLt->position.x, curberLt->position.y - 0.9f, curberLt->position.z);
        // 台の縁に最も近い届く点が複数あり得る (台の中心を狙うと四辺が同距離) ので、着く先ではなく「部分経路になる」「登らない」を見る
        ck.Check(curberAgent->pathPartial && curberAgent->status != navagentstatus::kNoPath
                     && curberLt->position.y - 0.9f < 0.2f,
                 "maxClimb を 5 cm 超える台の上の目的地は経路にならない (部分経路になり、台には登らない)");

        const auto* lostAgent = world.GetComponent<NavMeshAgentComponent>(lost);
        ck.Check(lostAgent->status == navagentstatus::kNoPath, "ナビメッシュの外の目的地は NoPath");
        const auto* lostLt = world.GetComponent<LocalTransform>(lost);
        ck.Check(std::fabs(lostLt->position.x - (-9.0f)) < 0.01f, "NoPath の Agent は動かない");

        const auto* bodyAgent = world.GetComponent<NavMeshAgentComponent>(noBody);
        ck.Check(bodyAgent->status == navagentstatus::kInactive, "CharacterController が無い Agent は Inactive");

        const auto* idleAgent = world.GetComponent<NavMeshAgentComponent>(idle);
        ck.Check(idleAgent->status == navagentstatus::kIdle, "目的地の無い Agent は Idle");

        // 目的地の取り消しと再設定
        auto* climberMut = world.GetComponent<NavMeshAgentComponent>(climber);
        climberMut->destination = { -9.0f, 0.0f, -6.0f };
        for (int i = 0; i < 600; ++i) {
            sim.Step();
        }
        climberMut = world.GetComponent<NavMeshAgentComponent>(climber);
        climberLt = world.GetComponent<LocalTransform>(climber);
        ck.Check(climberMut->status == navagentstatus::kArrived && std::fabs(climberLt->position.x - (-9.0f)) < 0.4f,
                 "目的地を変えると新しい目的地へ戻って着く");
        climberMut->hasDestination = false;
        sim.Step();
        ck.Check(world.GetComponent<NavMeshAgentComponent>(climber)->status == navagentstatus::kIdle,
                 "hasDestination を倒すと Idle になる");

        // Agent の経路が描画線に出る
        sim.GetWorld().GetComponent<NavMeshAgentComponent>(climber)->hasDestination = true;
        sim.GetWorld().GetComponent<NavMeshAgentComponent>(climber)->destination = { toPlatform[0], toPlatform[1], toPlatform[2] };
        for (int i = 0; i < 30; ++i) {
            sim.Step();
        }
        std::vector<DebugLineCmd> withPath;
        sim.nav.AppendDebugLines(world, withPath);
        world.GetComponent<NavMeshSurfaceComponent>(yard.surface)->drawAgentPaths = false;
        std::vector<DebugLineCmd> withoutPath;
        sim.nav.AppendDebugLines(world, withoutPath);
        ck.Check(withPath.size() > withoutPath.size(), "Agent の経路が描画線に出て、表示フラグで消せる");

        // 動かせない理由の遷移: Rigidbody が付くと CC は無効
        world.AddComponentRaw(idle, RigidbodyComponent::sTypeId);
        world.ApplyStructuralChanges();
        sim.Step();
        ck.Check(world.GetComponent<NavMeshAgentComponent>(idle)->status == navagentstatus::kInactive,
                 "Rigidbody が付くと Inactive (CC が無効になる)");
        yardHashAtEnd = WorldHashOf(sim, &sim.nav);
        MYE_LOG_INFO("  [yard] world hash after %llu ticks = %016llX", static_cast<unsigned long long>(sim.tick),
                     static_cast<unsigned long long>(yardHashAtEnd));
    }

    // ---- 1b. 登れる傾斜の設定が効く: 既定 (45 度) なら 30 度の坂を越え、20 度に下げるとその坂は経路から外れる ----
    for (const float maxSlopeDeg : { 45.0f, 20.0f }) {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const float platformTop = AddRampAndPlatform(scene, kRampDeg);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f, maxSlopeDeg);
        const float toPlatform[3] = { 9.0f, platformTop, 0.0f };
        const EntityID walker = AddAgent(scene, "SlopeWalker", 1.0f, 0.0f, 0.0f, toPlatform, true);
        ck.Check(BakeSurface(scene, surface, kYardGuid + 1, nullptr), "(傾斜の設定) 坂の庭をベイクできる");
        Sim sim(scene);
        for (int i = 0; i < 900; ++i) {
            sim.Step();
        }
        const auto* agent = sim.GetWorld().GetComponent<NavMeshAgentComponent>(walker);
        const auto* lt = sim.GetWorld().GetComponent<LocalTransform>(walker);
        MYE_LOG_INFO("  [slope] maxSlopeDeg %.0f: status %d pathPartial %d at (%.2f, %.2f, %.2f)", maxSlopeDeg, agent->status,
                     agent->pathPartial ? 1 : 0, lt->position.x, lt->position.y - 0.9f, lt->position.z);
        if (maxSlopeDeg > 30.0f) {
            ck.Check(agent->status == navagentstatus::kArrived && !agent->pathPartial
                         && std::fabs(lt->position.y - 0.9f - platformTop) < 0.15f,
                     "既定の Surface (maxSlopeDeg 45) と CC (stepOffset 0.3) で 30 度の坂を越えて台の上へ着く");
        } else {
            ck.Check(agent->status == navagentstatus::kArrived && agent->pathPartial && lt->position.y - 0.9f < 0.4f,
                     "maxSlopeDeg を 20 度に下げると 30 度の坂は経路から外れる (部分経路で地面に留まる)");
        }
    }

    // ---- 1c. 段差の量子化: 地面の天面がボクセル境界ちょうど / 半セルずれの両方で、maxClimb ちょうどは越え +5 cm は越えない ----
    for (const float yShift : { 0.0f, 0.025f }) {
        for (const float deckHeight : { 0.3f, kOverStepHeight }) {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            AddBox(scene, "Deck", 4.0f, deckHeight * 0.5f, 0.0f, 2.0f, deckHeight * 0.5f, 12.0f); // 全幅の台 (x 2..6)
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f, 45.0f, yShift);
            const float dest[3] = { 5.0f, deckHeight, 0.0f };
            const EntityID walker = AddAgent(scene, "DeckWalker", -6.0f, 0.0f, 0.0f, dest, true);
            ck.Check(BakeSurface(scene, surface, kYardGuid + 2, nullptr), "(量子化) 台の庭をベイクできる");
            Sim sim(scene);
            for (int i = 0; i < 900; ++i) {
                sim.Step();
            }
            const auto* agent = sim.GetWorld().GetComponent<NavMeshAgentComponent>(walker);
            const auto* lt = sim.GetWorld().GetComponent<LocalTransform>(walker);
            const float feetY = lt->position.y - 0.9f;
            MYE_LOG_INFO("  [quantize] yShift %.3f deck %.2f: status %d pathPartial %d at (%.2f, %.2f, %.2f)", yShift,
                         deckHeight, agent->status, agent->pathPartial ? 1 : 0, lt->position.x, feetY, lt->position.z);
            char msg[160];
            if (deckHeight <= 0.31f) {
                std::snprintf(msg, sizeof(msg), "地面の天面とボクセル境界のずれ %.3f m: maxClimb ちょうどの台の上へ着く", yShift);
                ck.Check(agent->status == navagentstatus::kArrived && !agent->pathPartial && std::fabs(feetY - deckHeight) < 0.1f, msg);
            } else {
                std::snprintf(msg, sizeof(msg), "地面の天面とボクセル境界のずれ %.3f m: maxClimb+5cm の台は経路にならない (部分経路、登らない)", yShift);
                ck.Check(agent->pathPartial && agent->status != navagentstatus::kNoPath && feetY < 0.1f, msg);
            }
        }
    }

    // ---- 2. すれ違い ----
    {
        float minWith = 1e9f;
        float minWithout = 1e9f;
        for (int quality = 2; quality >= 0; quality -= 2) {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float toB[3] = { 6.0f, 0.0f, 0.0f };
            const float toA[3] = { -6.0f, 0.0f, 0.0f };
            const EntityID a = AddAgent(scene, "A", -6.0f, 0.0f, 0.05f, toB, true, quality);
            const EntityID b = AddAgent(scene, "B", 6.0f, 0.0f, -0.05f, toA, true, quality);
            BakeSurface(scene, surface, kOpenGuid, nullptr);
            Sim sim(scene);
            float minDist = 1e9f;
            for (int i = 0; i < 600; ++i) {
                sim.Step();
                minDist = std::min(minDist, DistXZ(*sim.GetWorld().GetComponent<LocalTransform>(a),
                                                   *sim.GetWorld().GetComponent<LocalTransform>(b)));
            }
            const bool arrived = sim.GetWorld().GetComponent<NavMeshAgentComponent>(a)->status == navagentstatus::kArrived
                && sim.GetWorld().GetComponent<NavMeshAgentComponent>(b)->status == navagentstatus::kArrived;
            MYE_LOG_INFO("  [pass] avoidance quality %d: min distance %.3f m (radius sum 0.6), both arrived %d", quality,
                         minDist, arrived ? 1 : 0);
            (quality > 0 ? minWith : minWithout) = minDist;
            ck.Check(arrived, quality > 0 ? "正面からすれ違った 2 体（回避あり）とも着く" : "正面からすれ違った 2 体（回避なし）とも着く");
        }
        ck.Check(minWith >= 0.6f - 0.02f, "回避ありのすれ違いの最小距離は半径の和以上 (許容 2 cm)");
        ck.Check(minWithout < 0.6f - 0.1f, "回避なしでは半径の和を下回る (すり抜ける)");
    }

    // ---- 3. SimSnapshot の Nav 節: 空の NavSystem へ復元して連続実行と一致 ----
    {
        Scene scene;
        const Yard yard = BuildYard(scene);
        const float toPlatform[3] = { 9.4f, yard.platformTop, 0.0f };
        const float toIsland[3] = { 0.0f, 3.0f, 8.0f };
        const float toWest[3] = { -10.0f, 0.0f, 0.0f };
        for (int i = 0; i < 6; ++i) {
            const float z = -7.0f + 2.5f * static_cast<float>(i);
            AddAgent(scene, "Walker", -9.0f, 0.0f, z, (i % 3) == 0 ? toPlatform : (i % 3) == 1 ? toIsland : toWest, true,
                     1 + i % 3);
        }
        BakeSurface(scene, yard.surface, kYardGuid, nullptr);

        Sim sim(scene);
        SimRefs refs;
        refs.scene = &scene;
        refs.nav = &sim.nav;
        uint64_t tickRef = 0;
        refs.tickIndex = &tickRef;

        constexpr int kWarm = 240;
        constexpr int kAhead = 360;
        for (int i = 0; i < kWarm; ++i) {
            sim.Step();
        }
        tickRef = sim.tick;
        std::vector<std::byte> blob;
        ck.Check(CaptureSimSnapshot(refs, blob), "Agent が歩いている途中で撮影できる");
        ck.Check(sim.nav.HashedAgentCount() == 6, "6 体が crowd に載っている");
        {
            NavByteWriter navBytes;
            sim.nav.SaveSnapshot(navBytes);
            MYE_LOG_INFO("  [perf] Nav section with 6 agents: %zu bytes (snapshot %zu bytes in total)", navBytes.Size(),
                         blob.size());
        }

        std::vector<uint64_t> continuous;
        std::vector<uint64_t> continuousEcs;
        std::vector<uint64_t> continuousNav;
        for (int i = 0; i < kAhead; ++i) {
            sim.Step();
            continuous.push_back(WorldHashOf(sim, &sim.nav));
            continuousEcs.push_back(WorldHashOf(sim, nullptr));
            continuousNav.push_back(sim.nav.StateHash());
        }

        // 空の NavSystem (別の Sim) へ復元して進める。World は同じ Scene を巻き戻す
        Sim restored(scene);
        restored.tick = kWarm;
        SimRefs refs2 = refs;
        refs2.nav = &restored.nav;
        uint64_t tick2 = 0;
        refs2.tickIndex = &tick2;
        ck.Check(restored.nav.Surfaces().empty(), "復元先の NavSystem は空");
        ck.Check(RestoreSimSnapshot(refs2, blob.data(), blob.size()), "空の NavSystem へ復元できる");
        ck.Check(restored.nav.Surfaces().size() == 1 && restored.nav.Surfaces()[0].state == NavSurfaceState::Loaded,
                 "復元で Surface の .mnav が先に読み込まれている (次の Update が読み直さない前提)");
        const NavTileStore* storeAtRestore = restored.nav.Surfaces()[0].store.get();
        std::vector<std::byte> again;
        CaptureSimSnapshot(refs2, again);
        ck.Check(again == blob, "復元直後の再撮影が元の blob とバイト一致 (Nav 節を含む)");
        bool same = true;
        for (int i = 0; i < kAhead; ++i) {
            restored.Step();
            const bool tickSame = WorldHashOf(restored, &restored.nav) == continuous[static_cast<size_t>(i)];
            if (!tickSame && same) {
                MYE_LOG_ERROR("  [restore] first divergence at +%d tick: ecs %s, nav %s", i + 1,
                              WorldHashOf(restored, nullptr) == continuousEcs[static_cast<size_t>(i)] ? "same" : "DIFFERENT",
                              restored.nav.StateHash() == continuousNav[static_cast<size_t>(i)] ? "same" : "DIFFERENT");
            }
            same = same && tickSame;
        }
        ck.Check(same, "復元して 360 tick 進めた毎 tick のハッシュが連続実行と一致 (経路・速度・回避を含む)");
        ck.Check(restored.nav.Surfaces()[0].store.get() == storeAtRestore, "復元後の Update は Surface を読み直さない");

        // 毎 tick 撮る -> 戻す -> 撮る (--snapshot-stress と同じ形) で blob が崩れない
        bool stable = true;
        for (int i = 0; i < 40 && stable; ++i) {
            restored.Step();
            std::vector<std::byte> b1;
            std::vector<std::byte> b2;
            CaptureSimSnapshot(refs2, b1);
            RestoreSimSnapshot(refs2, b1.data(), b1.size());
            CaptureSimSnapshot(refs2, b2);
            stable = b1 == b2;
        }
        ck.Check(stable, "tick ごとの 撮影 -> 復元 -> 再撮影 が一致し続ける");

        // 状態が Agent の移動の途中でも、巻き戻すと Arrived 済みの Agent が再び歩き出すことはない (slots の arrived が戻る)
        std::vector<std::byte> early;
        // 少し進んだ状態を早めの tick に巻き戻す: 同じ Sim に対して blob を当て、再び連続実行と同じ軌跡を辿る
        ck.Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "元の NavSystem へも巻き戻せる");
        bool replay = true;
        for (int i = 0; i < kAhead; ++i) {
            sim.Step();
            replay = replay && WorldHashOf(sim, &sim.nav) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(replay, "元の NavSystem で巻き戻して再実行しても連続実行と一致する");
    }

    // ---- 4. Presence gate: Agent が居ない / NavMesh 系が無いシーンのハッシュを動かさない ----
    {
        Scene scene;
        const Yard yard = BuildYard(scene);
        ck.Check(BakeSurface(scene, yard.surface, kYardGuid, nullptr), "(準備) 庭をベイクできる");
        Sim sim(scene);
        for (int i = 0; i < 10; ++i) {
            sim.Step();
        }
        const uint64_t with = WorldHashOf(sim, &sim.nav);
        const uint64_t without = WorldHashOf(sim, nullptr);
        ck.Check(with == without, "Surface だけで Agent が居ないシーンは Nav 節をハッシュに畳まない");

        Scene bare;
        AddBox(bare, "Ground", 0.0f, -0.5f, 0.0f, 5.0f, 0.5f, 5.0f);
        Sim bareSim(bare);
        for (int i = 0; i < 10; ++i) {
            bareSim.Step();
        }
        ck.Check(WorldHashOf(bareSim, &bareSim.nav) == WorldHashOf(bareSim, nullptr),
                 "NavMesh 系が無いシーンのハッシュは NavSystem の有無で変わらない");
        SimRefs bareRefs;
        bareRefs.scene = &bare;
        bareRefs.nav = &bareSim.nav;
        std::vector<std::byte> blob;
        CaptureSimSnapshot(bareRefs, blob);
        SimRefs noNav;
        noNav.scene = &bare;
        std::vector<std::byte> blobNoNav;
        CaptureSimSnapshot(noNav, blobNoNav);
        ck.Check(blob == blobNoNav && RestoreSimSnapshot(noNav, blob.data(), blob.size()),
                 "Nav 節は空で書かれ、NavSystem を持たない構成でも読み捨てて戻せる");
    }

    // ---- 5. 容量 (128) と計測 ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        std::vector<EntityID> agents;
        constexpr int kAgents = 130;
        for (int i = 0; i < kAgents; ++i) {
            const float x = -10.0f + 0.6f * static_cast<float>(i % 32);
            const float z = -8.0f + 0.9f * static_cast<float>(i / 32);
            const float dest[3] = { -x * 0.8f, 0.0f, -z - 3.0f };
            agents.push_back(AddAgent(scene, "Crowd", x, 0.0f, z, dest, true, 1 + i % 3));
        }
        ck.Check(BakeSurface(scene, surface, kFieldGuid, nullptr), "群衆の床をベイクできる");
        Sim sim(scene);
        double worstUpdate = 0.0;
        double worstCrowd = 0.0;
        double sumUpdate = 0.0;
        constexpr int kTicks = 240;
        for (int i = 0; i < kTicks; ++i) {
            sim.Step();
            worstUpdate = std::max(worstUpdate, sim.nav.Stats().updateUs);
            worstCrowd = std::max(worstCrowd, sim.nav.Stats().crowdUpdateUs);
            sumUpdate += sim.nav.Stats().updateUs;
        }
        int inactive = 0;
        int onCrowd = 0;
        for (const EntityID e : agents) {
            inactive += sim.GetWorld().GetComponent<NavMeshAgentComponent>(e)->status == navagentstatus::kInactive ? 1 : 0;
            onCrowd += 1;
        }
        ck.Check(sim.nav.HashedAgentCount() == 128 && inactive == kAgents - 128,
                 "容量 128 を超えた 2 体はキー順の後ろから Inactive");
        ck.Check(sim.GetWorld().GetComponent<NavMeshAgentComponent>(agents.back())->status == navagentstatus::kInactive,
                 "...Inactive になるのはキーが最後の Agent");
        MYE_LOG_INFO("  [perf] 128 agents (%d ticks): NavSystem::Update avg %.0f us, worst %.0f us; dtCrowd::update worst %.0f us",
                     kTicks, sumUpdate / kTicks, worstUpdate, worstCrowd);

        // Nav 節の大きさと撮影時間 (ロールバックの毎 tick capture に足される分)
        SimRefs refs;
        refs.scene = &scene;
        refs.nav = &sim.nav;
        SimRefs noNav;
        noNav.scene = &scene;
        std::vector<std::byte> withNav;
        std::vector<std::byte> withoutNav;
        auto t0 = std::chrono::steady_clock::now();
        constexpr int kReps = 50;
        for (int i = 0; i < kReps; ++i) {
            CaptureSimSnapshot(refs, withNav);
        }
        const double captureWith = MicrosSince(t0) / kReps;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; ++i) {
            CaptureSimSnapshot(noNav, withoutNav);
        }
        const double captureWithout = MicrosSince(t0) / kReps;
        t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kReps; ++i) {
            RestoreSimSnapshot(refs, withNav.data(), withNav.size());
        }
        const double restoreWith = MicrosSince(t0) / kReps;
        NavByteWriter navOnly;
        sim.nav.SaveSnapshot(navOnly);
        MYE_LOG_INFO("  [perf] Nav section %zu bytes; capture %.1f us (without Nav section %.1f us, snapshot %zu -> %zu bytes); restore %.1f us",
                     navOnly.Size(), captureWith, captureWithout, withoutNav.size(), withNav.size(), restoreWith);
        (void)onCrowd;
    }

    ck.Check(kExpectedYardHash == 0 || yardHashAtEnd == kExpectedYardHash,
             "庭のワールドハッシュが焼いた期待値と一致 (Debug / Release 一致)");

    if (ck.failCount == 0) {
        MYE_LOG_INFO("NavAgent self test: ALL PASS");
    } else {
        MYE_LOG_ERROR("NavAgent self test: %d FAILED", ck.failCount);
    }
    return ck.failCount == 0;
}

} // namespace mye
