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
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/Navigation/NavBake.h"
#include "Engine/Engine/Navigation/NavDebugDraw.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Navigation/NavSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Engine/Script/ScriptKeys.h"

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kStepHeight = 0.3f;        // 庭の段差 = Surface.maxClimb の既定ちょうど (CC.stepOffset の既定とも同じ)
constexpr float kOverStepHeight = 0.35f;   // maxClimb を 5 cm 超える段差。経路にならない
constexpr float kRampDeg = 30.0f;          // 庭の坂。既定の Surface (maxSlopeDeg 45) で登れる
constexpr float kOnMeshTolerance = 0.05f; // HasPolyAt: 最寄り点がこの距離以内なら、その点はナビメッシュの上
constexpr uint64_t kYardGuid = 0x4E41564147454E31ull;  // メモリ登録の GUID (ファイルを作らない)
constexpr uint64_t kOpenGuid = 0x4E41564147454E32ull;
constexpr uint64_t kFieldGuid = 0x4E41564147454E33ull;
constexpr uint64_t kRavineGuid = 0x4E41564147454E34ull;
constexpr uint64_t kApiGuid = 0x4E41564147454E35ull;  // スクリプト API のテストの庭
// Debug で採取し、Release で同じ値になることを確認して焼く (docs\adr\ADR-023-navmesh.md)。
// 0f659f3 (Collider へ center / rotation を追加 = 庭の床・段差のハッシュ対象が増えた) で値が変わったので焼き直した
constexpr uint64_t kExpectedYardHash = 0xFECB095AB373213Bull;

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
        nav.PostPhysics(GetWorld(), kDt);
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

// 箱の障害物 (carve あり)。中心 (x, y, z)、寸法 (sx, sy, sz)
EntityID AddBoxObstacle(Scene& scene, float x, float y, float z, float sx, float sy, float sz)
{
    GameObject go = scene.CreateGameObjectTracked("Obstacle");
    go.SetLocalPosition(x, y, z);
    auto* ob = go.AddComponent<NavMeshObstacleComponent>();
    ob->size = { sx, sy, sz };
    return go.Id();
}

EntityID AddCylinderObstacle(Scene& scene, float x, float y, float z, float radius, float height)
{
    GameObject go = scene.CreateGameObjectTracked("Obstacle");
    go.SetLocalPosition(x, y, z);
    auto* ob = go.AddComponent<NavMeshObstacleComponent>();
    ob->shape = navobstacleshape::kCylinder;
    ob->radius = radius;
    ob->height = height;
    return go.Id();
}

// 障害物の中心の足元に、ナビメッシュのポリゴンがあるか (切り抜かれていれば無い)
bool HasPolyAt(NavSystem& nav, float x, float y, float z)
{
    if (nav.Surfaces().empty() || nav.Surfaces()[0].state != NavSurfaceState::Loaded) {
        return false;
    }
    const float center[3] = { x, y, z };
    const float extents[3] = { 0.3f, 1.0f, 0.3f };
    dtQueryFilter filter;
    filter.setIncludeFlags(kNavFlagWalk);
    dtPolyRef ref = 0;
    float nearest[3] = {};
    nav.Surfaces()[0].query->findNearestPoly(center, extents, &filter, &ref, nearest);
    // findNearestPoly は範囲と外接箱が重なる凸ポリゴンの最寄り点を返す (穴の縁のポリゴンも拾う)。点そのものが歩けるかを見る
    const float dx = nearest[0] - x;
    const float dz = nearest[2] - z;
    return ref != 0 && dx * dx + dz * dz < kOnMeshTolerance * kOnMeshTolerance;
}

// 障害物の出し入れを tick で決める台本 (連続実行と復元後の実行が同じ操作を同じ tick に受けるように)
struct ObstacleScript {
    EntityID box;      // tick 40 に出る箱
    EntityID cylinder; // tick 90 に出る円柱
    void Apply(Sim& sim, uint64_t tick)
    {
        World& world = sim.GetWorld();
        Scene& scene = sim.scene;
        switch (tick) {
        case 40:
            box = AddBoxObstacle(scene, -5.0f, 1.0f, 0.0f, 1.0f, 2.0f, 6.0f);
            world.ApplyStructuralChanges();
            break;
        case 90:
            cylinder = AddCylinderObstacle(scene, -2.0f, 1.0f, -5.0f, 1.0f, 2.0f);
            world.ApplyStructuralChanges();
            break;
        case 140:
            world.GetComponent<LocalTransform>(box)->position.z += 3.0f; // 動かす = 外して付け直す
            break;
        case 190:
            world.GetComponent<NavMeshObstacleComponent>(cylinder)->carve = false;
            break;
        case 240:
            world.DestroyEntity(box);
            world.ApplyStructuralChanges();
            break;
        case 290:
            world.GetComponent<NavMeshObstacleComponent>(cylinder)->carve = true;
            break;
        default:
            break;
        }
    }
};

// 箱の Modifier。中心 (x, y, z)、寸法 (sx, sy, sz)、エリア
EntityID AddModifier(Scene& scene, float x, float y, float z, float sx, float sy, float sz, int area)
{
    GameObject go = scene.CreateGameObjectTracked("Modifier");
    go.SetLocalPosition(x, y, z);
    auto* modifier = go.AddComponent<NavMeshModifierComponent>();
    modifier->size = { sx, sy, sz };
    modifier->area = area;
    return go.Id();
}

// 点の足元のポリゴンのエリア。ナビメッシュが無い / 点がナビメッシュの外なら -1
int AreaAt(NavSystem& nav, float x, float y, float z)
{
    if (nav.Surfaces().empty() || nav.Surfaces()[0].state != NavSurfaceState::Loaded) {
        return -1;
    }
    const float center[3] = { x, y, z };
    const float extents[3] = { 0.3f, 1.0f, 0.3f };
    dtQueryFilter filter;
    filter.setIncludeFlags(kNavFlagAllAreas);
    dtPolyRef ref = 0;
    float nearest[3] = {};
    nav.Surfaces()[0].query->findNearestPoly(center, extents, &filter, &ref, nearest);
    const float dx = nearest[0] - x;
    const float dz = nearest[2] - z;
    unsigned char area = 0;
    if (ref == 0 || dx * dx + dz * dz >= kOnMeshTolerance * kOnMeshTolerance
        || dtStatusFailed(nav.Surfaces()[0].store->NavMesh()->getPolyArea(ref, &area))) {
        return -1;
    }
    return area;
}

// Modifier の編集を tick で決める台本 (連続実行と復元後の実行が同じ操作を同じ tick に受けるように)。
// a と b は重なる 2 つの箱で、最初はどちらもエリア 0 (何も変えない)
struct ModifierScript {
    EntityID a = kNullEntity;
    EntityID b = kNullEntity;
    void Apply(Sim& sim, uint64_t tick)
    {
        World& world = sim.GetWorld();
        switch (tick) {
        case 30:
            world.GetComponent<NavMeshModifierComponent>(b)->area = 4; // 後から作った b を先に塗り替える
            break;
        case 80:
            world.GetComponent<NavMeshModifierComponent>(a)->area = 3;
            break;
        case 130:
            world.GetComponent<LocalTransform>(b)->position.x += 6.0f; // 動かす = 外して付け直す
            break;
        case 180:
            world.GetComponent<NavMeshModifierComponent>(a)->area = 1; // 歩行不可
            break;
        case 230:
            world.DestroyEntity(b);
            world.ApplyStructuralChanges();
            break;
        case 280:
            world.GetComponent<NavMeshModifierComponent>(a)->area = 5;
            break;
        default:
            break;
        }
    }
};

// 幅 4 m の谷 (x = -2..2 は床が無い) をはさんだ 2 つの床。谷を渡る経路は Link だけ。zShift は床の中心の z
EntityID BuildRavine(Scene& scene)
{
    AddBox(scene, "GroundA", -8.0f, -0.5f, 0.0f, 6.0f, 0.5f, 8.0f);
    AddBox(scene, "GroundB", 8.0f, -0.5f, 0.0f, 6.0f, 0.5f, 8.0f);
    return AddSurface(scene, 15.0f, 9.0f);
}

GameObject AddLinkObject(Scene& scene, const float* start, const float* end, int32_t traversal, bool bidirectional,
                         int32_t area = 2)
{
    GameObject go = scene.CreateGameObjectTracked("Link");
    auto* link = go.AddComponent<NavMeshLinkComponent>();
    link->start = { start[0], start[1], start[2] };
    link->end = { end[0], end[1], end[2] };
    link->traversal = traversal;
    link->bidirectional = bidirectional;
    link->area = area;
    return go;
}

EntityID AddLink(Scene& scene, const float* start, const float* end, int32_t traversal, bool bidirectional,
                 int32_t area = 2)
{
    return AddLinkObject(scene, start, end, traversal, bidirectional, area).Id();
}

bool FiniteAgent(World& world, EntityID e)
{
    const auto* lt = world.GetComponent<LocalTransform>(e);
    const auto* agent = world.GetComponent<NavMeshAgentComponent>(e);
    return lt != nullptr && agent != nullptr && std::isfinite(lt->position.x) && std::isfinite(lt->position.y)
        && std::isfinite(lt->position.z) && std::isfinite(agent->remainingDistance);
}

// 谷の Agent 1 体の観測
struct RavineRun {
    bool sawOnLink = false;
    int onLinkTicks = 0;
    float maxFeetY = -1.0e9f;
    int status = -1;
    bool partial = false;
    float x = 0.0f;
    float z = 0.0f;
};

// 谷を渡る Agent を ticks 回す。Link を 1 本置く (traversal < 0 なら置かない)。areaMask は Agent の歩けるエリア
RavineRun RunRavine(int32_t traversal, bool bidirectional, uint32_t areaMask, float startX, float destX, int ticks)
{
    Scene scene;
    const EntityID surface = BuildRavine(scene);
    const float linkStart[3] = { -3.0f, 0.0f, 0.0f };
    const float linkEnd[3] = { 3.0f, 0.0f, 0.0f };
    if (traversal >= 0) {
        AddLink(scene, linkStart, linkEnd, traversal, bidirectional);
    }
    const float dest[3] = { destX, 0.0f, 0.0f };
    const EntityID walker = AddAgent(scene, "Walker", startX, 0.0f, 0.0f, &dest[0], true);
    scene.GetWorld().GetComponent<NavMeshAgentComponent>(walker)->areaMask = areaMask;
    RavineRun run;
    if (!BakeSurface(scene, surface, kRavineGuid, nullptr)) {
        return run;
    }
    Sim sim(scene);
    World& world = sim.GetWorld();
    for (int i = 0; i < ticks; ++i) {
        sim.Step();
        const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
        const auto* lt = world.GetComponent<LocalTransform>(walker);
        if (agent->status == navagentstatus::kOnLink) {
            run.sawOnLink = true;
            ++run.onLinkTicks;
            run.maxFeetY = (std::max)(run.maxFeetY, lt->position.y - 0.9f);
        }
    }
    const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
    const auto* lt = world.GetComponent<LocalTransform>(walker);
    run.status = agent->status;
    run.partial = agent->pathPartial;
    run.x = lt->position.x;
    run.z = lt->position.z;
    return run;
}

// 谷が 2 本の Link (z = -4 の Jump と z = +4 の Manual) で渡れる庭の台本。Manual は tick 300 に完了を通知する
struct LinkScript {
    EntityID manualWalker = kNullEntity;
    void Apply(Sim& sim, uint64_t tick)
    {
        if (tick == 300) {
            auto* agent = sim.GetWorld().GetComponent<NavMeshAgentComponent>(manualWalker);
            if (agent != nullptr && agent->status == navagentstatus::kOnLink) {
                agent->linkComplete = true;
            }
        }
    }
};

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
        // 台の縁に最も近い届く点が複数あり得る (台の中心を狙うと四辺が同距離) ので、着く先ではなく「部分経路になる」「登らない」を見る。
        // 部分経路の終点の近くで前進が止まった Agent は、Moving のまま押し続けず Arrived + pathPartial になる (spec 4.1)
        ck.Check(curberAgent->pathPartial && curberAgent->status == navagentstatus::kArrived
                     && curberLt->position.y - 0.9f < 0.2f,
                 "maxClimb を 5 cm 超える台の上の目的地は経路にならず (部分経路)、台には登らず、縁で Arrived + pathPartial になる");

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

    // ---- 6. Obstacle: 経路上に置くと迂回し、外すと元の経路へ戻る (置いた tick のうちに TileCache が確定する) ----
    {
        // 床の上を (-8, 0, 0) から (8, 0, 0) へ歩く。x = 0 に z = -4..4 の壁。
        // mode 0 = 障害物なし / 1 = 最初からあり、tick 20 で外す / 2 = tick 20 で置く
        float crossingZ[3] = {};
        for (int mode = 0; mode < 3; ++mode) {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float dest[3] = { 8.0f, 0.0f, 0.0f };
            const EntityID walker = AddAgent(scene, "Walker", -8.0f, 0.0f, 0.0f, dest, true);
            EntityID wall = kNullEntity;
            if (mode == 1) {
                wall = AddBoxObstacle(scene, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f, 8.0f);
            }
            ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(障害物) 開けた床をベイクできる");
            Sim sim(scene);
            World& world = sim.GetWorld();
            bool crossed = false;
            for (int i = 0; i < 900; ++i) {
                if (i == 20 && mode == 1) {
                    world.GetComponent<NavMeshObstacleComponent>(wall)->carve = false;
                } else if (i == 20 && mode == 2) {
                    wall = AddBoxObstacle(scene, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f, 8.0f);
                    world.ApplyStructuralChanges();
                }
                sim.Step();
                if (i == 0 && mode == 1) {
                    ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 1 && !HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f),
                             "障害物を置いた tick のうちにナビメッシュが切り抜かれる (中心にポリゴンが無い)");
                }
                if (i == 20 && mode == 1) {
                    ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 0 && HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f),
                             "carve を倒した tick のうちに切り抜きが戻る (中心にポリゴンがある)");
                }
                if (i == 20 && mode == 2) {
                    ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 1 && !HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f),
                             "歩いている途中で置いた障害物も同じ tick のうちに切り抜く");
                }
                const auto* lt = world.GetComponent<LocalTransform>(walker);
                if (!crossed && lt->position.x >= 0.0f) {
                    crossed = true;
                    crossingZ[mode] = std::fabs(lt->position.z);
                }
            }
            const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
            const auto* lt = world.GetComponent<LocalTransform>(walker);
            MYE_LOG_INFO("  [obstacle] mode %d: crossed x=0 at |z| %.2f; final status %d at (%.2f, %.2f)", mode, crossingZ[mode],
                         agent->status, lt->position.x, lt->position.z);
            ck.Check(agent->status == navagentstatus::kArrived && !agent->pathPartial && lt->position.x > 7.0f,
                     mode == 0 ? "(障害物なし) 目的地へ着く" : mode == 1 ? "(外した後) 目的地へ着く" : "(置いた後) 迂回して目的地へ着く");
        }
        ck.Check(crossingZ[0] < 1.0f, "障害物が無ければ真っすぐ x = 0 を横切る (|z| < 1)");
        ck.Check(crossingZ[1] < 1.5f, "障害物を外すと元の経路 (真っすぐ) へ戻る (|z| < 1.5)");
        ck.Check(crossingZ[2] > 3.5f, "経路上に障害物を置くと壁の端を回り込む (|z| > 3.5)");
    }

    // ---- 6b. 表示 (NavDebugView): Play 中は NavSystem のナビメッシュから組み、Obstacle の切り抜きが映る。世代が変わったときだけ作り直す ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        const EntityID wall = AddBoxObstacle(scene, 0.0f, 1.0f, 0.0f, 4.0f, 2.0f, 4.0f);
        scene.GetWorld().GetComponent<NavMeshObstacleComponent>(wall)->carve = false;
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(表示) 開けた床をベイクできる");
        Sim sim(scene);
        World& world = sim.GetWorld();
        sim.Step();
        NavDebugView view;
        view.Refresh(world, &sim.nav);
        const int flatTriangles = view.GetStats().lastTriangles;
        ck.Check(view.GetStats().rebuildCount == 1 && view.GetStats().liveCount == 1 && view.GetStats().loadCount == 0,
                 "(表示) sim が読み込み済みの Surface は NavSystem から組む (.mnav を読み直さない)");
        view.Refresh(world, &sim.nav);
        sim.Step();
        view.Refresh(world, &sim.nav);
        ck.Check(view.GetStats().rebuildCount == 1, "(表示) ナビメッシュが変わらない間は作り直さない");
        world.GetComponent<NavMeshObstacleComponent>(wall)->carve = true;
        sim.Step();
        view.Refresh(world, &sim.nav);
        MYE_LOG_INFO("  [view] triangles: %d flat, %d with a 4 x 4 m hole", flatTriangles, view.GetStats().lastTriangles);
        ck.Check(view.GetStats().rebuildCount == 2 && view.GetStats().liveCount == 2 && view.GetStats().loadCount == 0
                     && view.GetStats().lastTriangles != flatTriangles,
                 "(表示) Obstacle で切り抜かれた tick の次の Refresh だけ作り直し、穴が映る");
        view.Refresh(world, &sim.nav);
        ck.Check(view.GetStats().rebuildCount == 2, "(表示) 切り抜いた後も、変わらなければ作り直さない");
        view.Refresh(world, nullptr);
        ck.Check(view.GetStats().rebuildCount == 3 && view.GetStats().loadCount == 1
                     && view.GetStats().lastTriangles == flatTriangles,
                 "(表示) 編集中 (NavSystem を渡さない) は .mnav から組み、切り抜き前の形に戻る");
    }

    // ---- 6d. 高さ: 段差の天面と 30 度の坂の上で、クエリの y と塗りの高さが歩行面と 0.1 m 以内 ----
    {
        constexpr float kHeightTolerance = 0.1f;
        constexpr float kFillLiftDrawn = 0.02f; // NavDebugDraw.cpp の kFillLift
        const float rampSlope = std::tan(kRampDeg * 3.14159265f / 180.0f);
        // 庭の歩行面の高さ。縁 (段差の側面・坂の端・台との重なり) の近くは扱わないので、その外だけ定義する
        const auto surfaceY = [&](float x, float& out) {
            if (x > -5.0f && x < -3.0f) { out = kStepHeight; return true; }
            if (x > 4.4f && x < 6.6f) { out = (x - 4.0f) * rampSlope; return true; }
            if (x > 7.9f && x < 10.2f) { out = 2.0f; return true; }
            if (x > -8.0f && x < -6.0f) { out = 0.0f; return true; }
            return false;
        };
        Scene scene;
        const Yard yard = BuildYard(scene);
        ck.Check(BakeSurface(scene, yard.surface, kYardGuid, nullptr), "(高さ) 庭をベイクできる");
        Sim sim(scene);
        World& world = sim.GetWorld();
        sim.Step();
        const NavSurfaceRuntime& surface = sim.nav.Surfaces()[0];

        float worstSample = 0.0f;
        float worstRaw = 0.0f;
        int sampleCount = 0;
        bool sampleOk = true;
        for (float x = -7.9f; x < 10.2f; x += 0.1f) {
            float expectY = 0.0f;
            if (!surfaceY(x, expectY)) {
                continue;
            }
            for (const float z : { -2.0f, 0.0f, 2.0f }) {
                const float pos[3] = { x, expectY + 0.4f, z };
                const float ext[3] = { 0.5f, 1.5f, 0.5f };
                float out[3] = {};
                if (!sim.nav.QuerySamplePosition(world, 0, pos, ext, 0xFFFFFFFFu, out)) {
                    sampleOk = false;
                    continue;
                }
                dtQueryFilter filter;
                filter.setIncludeFlags(kNavFlagAllAreas);
                dtPolyRef ref = 0;
                float raw[3] = {};
                surface.query->findNearestPoly(pos, ext, &filter, &ref, raw);
                worstSample = (std::max)(worstSample, std::fabs(out[1] - expectY));
                worstRaw = (std::max)(worstRaw, std::fabs(raw[1] - expectY));
                ++sampleCount;
            }
        }
        MYE_LOG_INFO("  [height] NavSamplePosition over %d points: worst |y - surface| = %.3f m (Detour polygon plane alone: %.3f m)",
                     sampleCount, worstSample, worstRaw);
        ck.Check(sampleOk && sampleCount > 200 && worstSample <= kHeightTolerance,
                 "(高さ) NavSamplePosition の y は段差の天面・坂・台の上で歩行面との差 0.1 m 以内");

        // 経路の角: 段差の天面から坂の上までの経路。始点と終点が段差の天面・坂の上にある
        {
            const float from[3] = { -4.0f, kStepHeight, 0.0f };
            const float to[3] = { 5.5f, 1.5f * rampSlope, 0.0f };
            float corners[32 * 3] = {};
            bool partial = false;
            const int n = sim.nav.QueryFindPath(world, 0, from, to, 0xFFFFFFFFu, corners, 32, &partial);
            float worst = 0.0f;
            int measured = 0;
            for (int i = 0; i < n; ++i) {
                float expectY = 0.0f;
                if (!surfaceY(corners[i * 3], expectY)) {
                    continue; // 縁の上の角は歩行面が決まらない
                }
                const float err = std::fabs(corners[i * 3 + 1] - expectY);
                MYE_LOG_INFO("  [height] path corner %d (%.2f, %.3f, %.2f) error %.3f", i, corners[i * 3], corners[i * 3 + 1], corners[i * 3 + 2], err);
                worst = (std::max)(worst, err);
                ++measured;
            }
            ck.Check(measured >= 2 && !partial && worst <= kHeightTolerance,
                     "(高さ) NavFindPath の角の y は歩行面との差 0.1 m 以内 (段差の天面から坂の上へ)");
        }

        // Raycast の当たり点: 段差の天面と坂の上を +z へ歩き、縁 (Surface の端・坂の幅) で止まる
        {
            bool hitsOk = true;
            float worst = 0.0f;
            for (const float x : { -4.5f, -3.5f, 4.8f, 5.6f, 6.4f }) {
                float expectY = 0.0f;
                surfaceY(x, expectY);
                const float from[3] = { x, expectY, 0.0f };
                const float to[3] = { x, expectY, 20.0f };
                NavRaycastResult hit;
                if (!sim.nav.QueryRaycast(world, 0, from, to, 0xFFFFFFFFu, hit) || !hit.hit) {
                    hitsOk = false;
                    continue;
                }
                worst = (std::max)(worst, std::fabs(hit.point[1] - expectY));
            }
            MYE_LOG_INFO("  [height] NavRaycast hit points: worst |y - surface| = %.3f m", worst);
            ck.Check(hitsOk && worst <= kHeightTolerance, "(高さ) NavRaycast の当たり点の y は歩行面との差 0.1 m 以内");
        }

        // ランダムな点: 段差の天面と坂の上に中心を置いた半径 1 m の円
        {
            Pcg32 rng;
            rng.Seed(12345u);
            float worst = 0.0f;
            int drawn = 0;
            for (const float cx : { -4.0f, 5.5f }) {
                float centerY = 0.0f;
                surfaceY(cx, centerY);
                const float center[3] = { cx, centerY, 0.0f };
                for (int i = 0; i < 40; ++i) {
                    float out[3] = {};
                    if (!sim.nav.QueryRandomPoint(world, 0, center, 1.0f, 0xFFFFFFFFu, rng, out)) {
                        continue;
                    }
                    float expectY = 0.0f;
                    if (surfaceY(out[0], expectY)) {
                        worst = (std::max)(worst, std::fabs(out[1] - expectY));
                        ++drawn;
                    }
                }
            }
            MYE_LOG_INFO("  [height] NavFindRandomPoint over %d points: worst |y - surface| = %.3f m", drawn, worst);
            ck.Check(drawn > 40 && worst <= kHeightTolerance, "(高さ) NavFindRandomPoint の y は歩行面との差 0.1 m 以内");
        }

        // 塗り: 歩行面の上の各点を覆う三角形のうち、塗りの高さが歩行面に最も近いものを探す。
        // 台の中 (箱の底面と地面が接する所) には到達できない床が取り残され、同じ (x, z) を地面の高さの三角形も覆う
        const auto checkFill = [&](NavDebugView& view, const char* source) {
            const std::vector<DebugFillVertex>& fill = view.FillVertices();
            float worst = 0.0f;
            int covered = 0;
            int uncovered = 0;
            int stepPoints = 0;
            for (float x = -7.9f; x < 10.2f; x += 0.25f) {
                float expectY = 0.0f;
                if (!surfaceY(x, expectY)) {
                    continue;
                }
                for (const float z : { -2.0f, 0.0f, 2.0f }) {
                    float best = 1e9f;
                    for (size_t i = 0; i + 2 < fill.size(); i += 3) {
                        const DebugFillVertex& a = fill[i];
                        const DebugFillVertex& b = fill[i + 1];
                        const DebugFillVertex& c = fill[i + 2];
                        const float det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
                        if (std::fabs(det) < 1e-9f) {
                            continue;
                        }
                        const float w0 = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / det;
                        const float w1 = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / det;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                            continue;
                        }
                        const float y = w0 * a.y + w1 * b.y + w2 * c.y - kFillLiftDrawn;
                        best = (std::min)(best, std::fabs(y - expectY));
                    }
                    if (best > 1e8f) {
                        ++uncovered;
                        continue;
                    }
                    ++covered;
                    stepPoints += expectY == kStepHeight ? 1 : 0;
                    worst = (std::max)(worst, best);
                }
            }
            MYE_LOG_INFO("  [height] fill (%s): %d triangles, %d points checked (%d on the step top, %d not covered), worst |y - surface| = %.3f m",
                         source, view.GetStats().lastTriangles, covered, stepPoints, uncovered, worst);
            ck.Check(uncovered == 0 && stepPoints >= 10 && worst <= kHeightTolerance,
                     "(高さ) 塗りは段差の天面・坂・台の上の各点を歩行面との差 0.1 m 以内の高さで覆う (天面が塗られる)");
        };
        NavDebugView liveView;
        liveView.Refresh(world, &sim.nav);
        checkFill(liveView, "Play 中: NavSystem のナビメッシュ");
        NavDebugView editView;
        editView.Refresh(world, nullptr);
        checkFill(editView, "編集中: .mnav");
    }

    // ---- 6c. 形と付ける Surface: y 回転した箱は実形のまま切り、Surface の範囲外の障害物は TileCache に入れない ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        const EntityID slab = AddBoxObstacle(scene, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f, 8.0f);
        constexpr float kQuarterPi = 0.78539816f;
        scene.GetWorld().GetComponent<LocalTransform>(slab)->rotation = { 0.0f, std::sin(kQuarterPi * 0.5f), 0.0f, std::cos(kQuarterPi * 0.5f) };
        const EntityID faraway = AddBoxObstacle(scene, 100.0f, 1.0f, 0.0f, 1.0f, 2.0f, 1.0f);
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(形) 開けた床をベイクできる");
        Sim sim(scene);
        sim.Step();
        const NavTileStore& store = *sim.nav.Surfaces()[0].store;
        ck.Check(store.ObstacleCount() == 1 && store.ObstacleAt(0).type == DT_OBSTACLE_ORIENTED_BOX
                     && std::fabs(std::fabs(store.ObstacleAt(0).yaw) - kQuarterPi) < 0.01f,
                 "(形) y 回転した箱は回転箱 (yaw 45 度) として入り、Surface の範囲外の障害物は入らない");
        {
            const NavObstacleSpec& o = store.ObstacleAt(0);
            MYE_LOG_INFO("  [shape] spec type %d v (%.2f %.2f %.2f | %.2f %.2f %.2f) yaw %.3f", o.type, o.v[0], o.v[1],
                         o.v[2], o.v[3], o.v[4], o.v[5], o.yaw);
        }
        const bool a = HasPolyAt(sim.nav, 2.0f, 0.0f, 2.0f);
        const bool b = HasPolyAt(sim.nav, 2.0f, 0.0f, -2.0f);
        MYE_LOG_INFO("  [shape] yaw %.3f: poly at (2,2) %d, (2,-2) %d, (-2,2) %d, (-2,-2) %d, (0,0) %d, (0,3.5) %d", store.ObstacleAt(0).yaw,
                     a ? 1 : 0, b ? 1 : 0, HasPolyAt(sim.nav, -2.0f, 0.0f, 2.0f) ? 1 : 0, HasPolyAt(sim.nav, -2.0f, 0.0f, -2.0f) ? 1 : 0,
                     HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f) ? 1 : 0, HasPolyAt(sim.nav, 0.0f, 0.0f, 3.5f) ? 1 : 0);
        ck.Check(a != b, "(形) 長い対角線の上だけが切り抜かれ、外接 AABB の隅 (反対の対角線) は残る");
        scene.GetWorld().GetComponent<LocalTransform>(faraway)->position.x = 3.0f;
        sim.Step();
        ck.Check(store.ObstacleCount() == 2, "(形) 範囲内へ動かした障害物は次の tick で入る");
        // x / z の傾きは外接 AABB で切る
        scene.GetWorld().GetComponent<LocalTransform>(slab)->rotation = { std::sin(0.1f), 0.0f, 0.0f, std::cos(0.1f) };
        sim.Step();
        bool hasAabb = false;
        for (int i = 0; i < store.ObstacleCount(); ++i) {
            hasAabb = hasAabb || store.ObstacleAt(i).type == DT_OBSTACLE_BOX;
        }
        ck.Check(hasAabb, "(形) x 軸まわりに傾いた箱は外接 AABB で切る");
    }

    {
        // 回転なしの回転箱 (yaw 0): 長辺 (z) の上だけが切り抜かれる
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        AddBoxObstacle(scene, 0.0f, 1.0f, 0.0f, 1.0f, 2.0f, 8.0f);
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(形) 開けた床をベイクできる");
        Sim sim(scene);
        sim.Step();
        const NavTileStore& store = *sim.nav.Surfaces()[0].store;
        MYE_LOG_INFO("  [shape] yaw 0: type %d; poly at (0,0) %d, (0,3) %d, (2,0) %d", store.ObstacleAt(0).type, HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f) ? 1 : 0,
                     HasPolyAt(sim.nav, 0.0f, 0.0f, 3.0f) ? 1 : 0, HasPolyAt(sim.nav, 2.0f, 0.0f, 0.0f) ? 1 : 0);
        ck.Check(!HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f) && !HasPolyAt(sim.nav, 0.0f, 0.0f, 3.0f) && HasPolyAt(sim.nav, 2.0f, 0.0f, 0.0f),
                 "(形) 回転なしの回転箱は長辺の上だけを切り抜く");
    }

    // ---- 7. Obstacle の SimSnapshot: 追加・移動・撤去の途中で撮って、空の NavSystem / 元の NavSystem へ復元して連続実行と一致 ----
    {
        Scene scene;
        const Yard yard = BuildYard(scene);
        const float toPlatform[3] = { 9.4f, yard.platformTop, 0.0f };
        const float toIsland[3] = { 0.0f, 3.0f, 8.0f };
        const float toWest[3] = { -10.0f, 0.0f, 0.0f };
        for (int i = 0; i < 4; ++i) {
            const float z = -7.0f + 4.0f * static_cast<float>(i);
            AddAgent(scene, "Walker", -9.0f, 0.0f, z, i == 0 ? toPlatform : i == 1 ? toIsland : toWest, true, 1 + i % 3);
        }
        BakeSurface(scene, yard.surface, kYardGuid, nullptr);

        Sim sim(scene);
        ObstacleScript script;
        SimRefs refs;
        refs.scene = &scene;
        refs.nav = &sim.nav;
        uint64_t tickRef = 0;
        refs.tickIndex = &tickRef;

        constexpr int kWarm = 120; // 箱 (40) と円柱 (90) が出た後、箱の移動 (140) の前
        constexpr int kAhead = 330;
        for (int i = 0; i < kWarm; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
        }
        tickRef = sim.tick;
        ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 2, "(復元) 箱と円柱の 2 つが TileCache に入っている");
        std::vector<std::byte> blob;
        ck.Check(CaptureSimSnapshot(refs, blob), "(復元) 障害物がある状態で撮影できる");

        std::vector<uint64_t> continuous;
        std::vector<int> continuousCounts;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            continuous.push_back(WorldHashOf(sim, &sim.nav));
            continuousCounts.push_back(sim.nav.Surfaces()[0].store->ObstacleCount());
        }
        ck.Check(continuousCounts.back() == 1, "(復元) 連続実行の終わりでは円柱だけが残る (箱は撤去済み)");

        // 空の NavSystem へ復元
        Sim restored(scene);
        restored.tick = kWarm;
        SimRefs refs2 = refs;
        refs2.nav = &restored.nav;
        uint64_t tick2 = 0;
        refs2.tickIndex = &tick2;
        ck.Check(RestoreSimSnapshot(refs2, blob.data(), blob.size()), "(復元) 空の NavSystem へ復元できる (戻り値 true)");
        const NavTileStore& restoredStore = *restored.nav.Surfaces()[0].store;
        const bool keysOk = restoredStore.ObstacleCount() == 2
            && restoredStore.ObstacleAt(0).key == NavObstacleKey(script.box) && restoredStore.ObstacleAt(1).key == NavObstacleKey(script.cylinder);
        ck.Check(keysOk, "(復元) 復元直後の store の障害物のキーが Obstacle コンポーネントの Entity と対応している");
        std::vector<std::byte> again;
        CaptureSimSnapshot(refs2, again);
        ck.Check(again == blob, "(復元) 復元直後の再撮影が元の blob とバイト一致 (障害物を含む)");
        bool same = true;
        bool countsSame = true;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(restored, restored.tick);
            restored.Step();
            if (i == 0) {
                ck.Check(restored.nav.Surfaces()[0].store->ObstacleCount() == 2,
                         "(復元) 復元直後の Update が障害物を二重に足さない");
            }
            same = same && WorldHashOf(restored, &restored.nav) == continuous[static_cast<size_t>(i)];
            countsSame = countsSame && restored.nav.Surfaces()[0].store->ObstacleCount() == continuousCounts[static_cast<size_t>(i)];
        }
        ck.Check(same, "(復元) 空の NavSystem から 330 tick (移動・撤去・carve の切り替えの Commit を含む) の毎 tick ハッシュが連続実行と一致");
        ck.Check(countsSame, "(復元) 毎 tick の障害物数が連続実行と同じ (消し忘れ・二重追加なし)");

        // 元の NavSystem (障害物が撤去された状態) へ巻き戻す。store の障害物が復元され、Update が整合させる
        ck.Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "(復元) 障害物が変わった後の元の NavSystem へ巻き戻せる");
        ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 2, "(復元) 巻き戻すと撤去済みの箱が TileCache へ戻る");
        sim.tick = kWarm;
        bool replay = true;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            replay = replay && WorldHashOf(sim, &sim.nav) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(replay, "(復元) 元の NavSystem で巻き戻して再実行しても連続実行と一致する");

        // 毎 tick 撮る -> 戻す -> 撮る が障害物の出入りの間も崩れない
        bool stable = true;
        sim.tick = kWarm;
        RestoreSimSnapshot(refs, blob.data(), blob.size());
        for (int i = 0; i < kAhead && stable; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            std::vector<std::byte> b1;
            std::vector<std::byte> b2;
            CaptureSimSnapshot(refs, b1);
            RestoreSimSnapshot(refs, b1.data(), b1.size());
            CaptureSimSnapshot(refs, b2);
            stable = b1 == b2;
        }
        ck.Check(stable, "(復元) 障害物の出入りの間も tick ごとの 撮影 -> 復元 -> 再撮影 が一致し続ける");
    }

    // ---- 8. TileCache の更新時間: 数百タイルのナビメッシュで、障害物を毎 tick 動かす ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 85.0f, 0.5f, 85.0f);
        const EntityID surface = AddSurface(scene, 86.0f, 86.0f);
        {
            auto* sf = scene.GetWorld().GetComponent<NavMeshSurfaceComponent>(surface);
            sf->autoCellSize = false; // セル 0.3 / タイル 32 セル = 9.6 m: 170 m 四方で 324 枚
            sf->cellSize = 0.3f;
            sf->cellHeight = 0.2f;
            sf->tileSize = 32;
        }
        constexpr int kMovers = 8;
        std::vector<EntityID> movers;
        for (int i = 0; i < kMovers; ++i) {
            movers.push_back(AddBoxObstacle(scene, -60.0f + 15.0f * static_cast<float>(i), 1.0f, -20.0f + 5.0f * static_cast<float>(i),
                                            2.0f, 2.0f, 2.0f));
        }
        const float dest[3] = { 30.0f, 0.0f, 30.0f };
        AddAgent(scene, "Walker", -30.0f, 0.0f, -30.0f, dest, true); // 載っている Agent がいる状態の時間を測る
        ck.Check(BakeSurface(scene, surface, kFieldGuid + 100, nullptr), "(計測) 170 m 四方の床をベイクできる");
        Sim sim(scene);
        sim.Step(); // 初回の追加
        const NavSurfaceRuntime& rt = sim.nav.Surfaces()[0];
        MYE_LOG_INFO("  [perf] obstacle bench: %d tiles, %d layers, %d polygons, %d obstacles", rt.tileCount, rt.layerCount,
                     rt.polyCount, rt.store->ObstacleCount());
        ck.Check(rt.tileCount >= 300 && rt.store->ObstacleCount() == kMovers, "(計測) 300 枚以上のタイルと 8 つの障害物");
        double sumUs = 0.0;
        double worstUs = 0.0;
        int measured = 0;
        bool allMoved = true;
        constexpr int kBenchTicks = 60;
        for (int i = 0; i < kBenchTicks; ++i) {
            for (const EntityID e : movers) {
                scene.GetWorld().GetComponent<LocalTransform>(e)->position.x += (i & 1) ? -0.2f : 0.2f; // 閾値 (5 cm) を超える動きを毎 tick
            }
            sim.Step();
            const double us = sim.nav.Stats().obstacleUs;
            allMoved = allMoved && sim.nav.Stats().obstacleChanges == 2 * kMovers;
            sumUs += us;
            worstUs = std::max(worstUs, us);
            ++measured;
        }
        ck.Check(allMoved, "(計測) 毎 tick 全部の障害物が外れて付く");
        MYE_LOG_INFO("  [perf] obstacle sync (8 obstacles moved every tick, %d tiles): avg %.0f us, worst %.0f us per tick",
                     rt.tileCount, sumUs / measured, worstUs);
        // 内訳: dtNavMesh の入れ直し (履歴依存を消す正規化) を止めた場合の時間。差が Commit の O(全タイル) の分
        rt.store->SetCanonicalize(false);
        double sumRawUs = 0.0;
        for (int i = 0; i < kBenchTicks; ++i) {
            for (const EntityID e : movers) {
                scene.GetWorld().GetComponent<LocalTransform>(e)->position.x += (i & 1) ? -0.2f : 0.2f;
            }
            sim.Step();
            sumRawUs += sim.nav.Stats().obstacleUs;
        }
        rt.store->SetCanonicalize(true);
        MYE_LOG_INFO("  [perf] ...of which the full tile re-insertion (canonicalize) takes about %.0f us per tick (%.0f us without it)",
                     (sumUs - sumRawUs) / kBenchTicks, sumRawUs / kBenchTicks);
        // 動かさない tick は TileCache に触らない
        sim.Step();
        ck.Check(sim.nav.Stats().obstacleChanges == 0 && sim.nav.Stats().obstacleUs == 0.0,
                 "(計測) 障害物が動かない tick は TileCache を触らない");
        // 閾値以下の動きは無視される
        const uint64_t before = rt.store->HashObstacles();
        scene.GetWorld().GetComponent<LocalTransform>(movers[0])->position.x += 0.02f;
        sim.Step();
        ck.Check(rt.store->HashObstacles() == before && sim.nav.Stats().obstacleChanges == 0,
                 "(計測) 閾値 (5 cm) 以下の動きは TileCache を作り直さない");
    }

    // ---- 9. 詰まり検出 (Stuck): NavMesh は繋がっているのに CC が登れない段差で、Moving のまま押し続けない ----
    {
        // maxClimb (0.3) を 1 セル未満だけ超える台。ボクセルの量子化で NavMesh は繋がり、CC (stepOffset 0.3) は登れない。
        // 地面とのボクセル境界のずれと台の高さを振って、Stuck になる組み合わせがあることを確かめる
        int stuckCases = 0;
        int partialCases = 0;
        for (const float yShift : { 0.0f, 0.0125f, 0.025f, 0.0375f }) {
            for (const float deckHeight : { 0.31f, 0.33f, 0.35f }) {
                Scene scene;
                AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
                AddBox(scene, "Deck", 4.0f, deckHeight * 0.5f, 0.0f, 2.0f, deckHeight * 0.5f, 12.0f); // 全幅の台 (x 2..6)
                const EntityID surface = AddSurface(scene, 13.0f, 13.0f, 45.0f, yShift);
                const float dest[3] = { 5.0f, deckHeight, 0.0f };
                const EntityID walker = AddAgent(scene, "DeckWalker", -6.0f, 0.0f, 0.0f, dest, true);
                ck.Check(BakeSurface(scene, surface, kYardGuid + 3, nullptr), "(詰まり) 台の庭をベイクできる");
                Sim sim(scene);
                for (int i = 0; i < 900; ++i) {
                    sim.Step();
                }
                const auto* agent = sim.GetWorld().GetComponent<NavMeshAgentComponent>(walker);
                const auto* lt = sim.GetWorld().GetComponent<LocalTransform>(walker);
                {
                    const dtCrowdAgent* crowdAgent = sim.nav.Surfaces()[0].crowd->getAgent(0);
                    const float* endCorner = crowdAgent->ncorners > 0 ? &crowdAgent->cornerVerts[(crowdAgent->ncorners - 1) * 3] : crowdAgent->npos;
                    MYE_LOG_INFO("  [stuck] yShift %.4f deck %.2f: status %d pathPartial %d at (%.2f, %.2f), remaining %.3f, path end x %.2f (corners %d, flags %d), crowd x %.3f vel %.3f",
                                 yShift, deckHeight, agent->status, agent->pathPartial ? 1 : 0, lt->position.x,
                                 lt->position.y - 0.9f, agent->remainingDistance, endCorner[0], crowdAgent->ncorners,
                                 crowdAgent->ncorners > 0 ? crowdAgent->cornerFlags[crowdAgent->ncorners - 1] : -1,
                                 crowdAgent->npos[0], crowdAgent->vel[0]);
                }
                // 完全な経路の途中 (台の上へ繋がっているのに登れない) は Stuck、部分経路の終点の近くは Arrived
                ck.Check(agent->status == (agent->pathPartial ? navagentstatus::kArrived : navagentstatus::kStuck),
                         "台の縁で Moving のまま押し続けない (完全な経路の途中なら Stuck、部分経路の終点付近なら Arrived + pathPartial)");
                partialCases += agent->pathPartial ? 1 : 0;
                if (agent->status == navagentstatus::kStuck) {
                    ++stuckCases;
                    // 止めない: 段差を登れず位置は進まないが、目的地が同じ間は Stuck のまま押し続ける
                    const float x0 = lt->position.x;
                    for (int i = 0; i < 30; ++i) {
                        sim.Step();
                    }
                    ck.Check(std::fabs(lt->position.x - x0) < 0.01f && agent->status == navagentstatus::kStuck,
                             "Stuck の Agent は登れない段差の前に留まり、目的地が同じ間は Stuck のまま");
                    // 目的地を変えると解除されて新しい目的地へ歩く
                    sim.GetWorld().GetComponent<NavMeshAgentComponent>(walker)->destination = { -6.0f, 0.0f, 4.0f };
                    for (int i = 0; i < 600; ++i) {
                        sim.Step();
                    }
                    const auto* back = sim.GetWorld().GetComponent<NavMeshAgentComponent>(walker);
                    ck.Check(back->status == navagentstatus::kArrived && std::fabs(lt->position.z - 4.0f) < 0.4f,
                             "目的地を変えると Stuck が解除されて新しい目的地へ着く");
                }
            }
        }
        MYE_LOG_INFO("  [stuck] %d of 12 combinations ended in Stuck, %d ended as a partial path (Arrived)", stuckCases, partialCases);
        ck.Check(stuckCases > 0 && partialCases > 0,
                 "量子化のずれで繋がる登れない台 (Stuck) と、繋がらない台 (部分経路で Arrived) の両方がある");

        // 経路の途中を塞いで押し合わせる: ベイク後に床の幅いっぱいの壁 (ナビメッシュには無い) を置く。経路は完全なまま前へ進めない
        {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float dest[3] = { 8.0f, 0.0f, 0.0f };
            const EntityID walker = AddAgent(scene, "Pusher", -8.0f, 0.0f, 0.0f, dest, true);
            ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(詰まり) 壁を置く前の床をベイクできる");
            const EntityID lateWall = AddBox(scene, "LateWall", 0.0f, 1.0f, 0.0f, 0.5f, 1.0f, 13.0f).Id();
            scene.GetWorld().ApplyStructuralChanges();
            Sim sim(scene);
            for (int i = 0; i < 600; ++i) {
                sim.Step();
            }
            const auto* agent = sim.GetWorld().GetComponent<NavMeshAgentComponent>(walker);
            MYE_LOG_INFO("  [stuck] pusher: status %d pathPartial %d remaining %.2f", agent->status, agent->pathPartial ? 1 : 0,
                         agent->remainingDistance);
            ck.Check(agent->status == navagentstatus::kStuck && !agent->pathPartial && agent->remainingDistance > 5.0f,
                     "完全な経路の途中を塞がれて押し合う Agent は Stuck (部分経路ではない、終点から遠い)");
            // 塞ぎを消すと、止まっていなかった Agent が Moving へ戻って到着する
            scene.GetWorld().DestroyEntity(lateWall);
            scene.GetWorld().ApplyStructuralChanges();
            bool movedAgain = false;
            for (int i = 0; i < 900; ++i) {
                sim.Step();
                movedAgain = movedAgain || agent->status == navagentstatus::kMoving;
            }
            ck.Check(movedAgain && agent->status == navagentstatus::kArrived,
                     "塞ぎを消すと Stuck -> Moving -> Arrived (止めずに押し続けていたので目的地を変えなくても進む)");
        }

        // 塞がれた列: 先頭だけが Stuck、すぐ後ろに接している後続は Moving のまま
        {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 0.6f); // 幅 1.2 m = 一列にしか並べない廊下
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float dest[3] = { 8.0f, 0.0f, 0.0f };
            const EntityID lead = AddAgent(scene, "Lead", -6.0f, 0.0f, 0.0f, dest, true);
            const EntityID mid = AddAgent(scene, "Mid", -6.8f, 0.0f, 0.0f, dest, true);
            const EntityID tail = AddAgent(scene, "Tail", -7.6f, 0.0f, 0.0f, dest, true);
            ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(塞がれた列) 壁を置く前の床をベイクできる");
            AddBox(scene, "LateWall", 0.0f, 1.0f, 0.0f, 0.5f, 1.0f, 13.0f);
            scene.GetWorld().ApplyStructuralChanges();
            Sim sim(scene);
            int maxStuckAtOnce = 0; // 同時に Stuck の Agent 数の最大 (先頭 1 体だけのはず)
            int tailStuckTicks = 0;
            int anyStuckTicks = 0;
            for (int i = 0; i < 900; ++i) {
                sim.Step();
                World& w = sim.GetWorld();
                int stuckNow = 0;
                for (const EntityID id : { lead, mid, tail }) {
                    stuckNow += w.GetComponent<NavMeshAgentComponent>(id)->status == navagentstatus::kStuck ? 1 : 0;
                }
                maxStuckAtOnce = (std::max)(maxStuckAtOnce, stuckNow);
                anyStuckTicks += stuckNow > 0 ? 1 : 0;
                tailStuckTicks += w.GetComponent<NavMeshAgentComponent>(tail)->status == navagentstatus::kStuck ? 1 : 0;
            }
            for (const EntityID id : { lead, mid, tail }) {
                const auto* a = sim.GetWorld().GetComponent<NavMeshAgentComponent>(id);
                const auto* t = sim.GetWorld().GetComponent<LocalTransform>(id);
                MYE_LOG_INFO("  [queue]   status %d remaining %.3f at (%.2f, %.2f)", a->status, a->remainingDistance, t->position.x, t->position.z);
            }
            MYE_LOG_INFO("  [queue] stuck ticks %d, max stuck at once %d, tail stuck ticks %d", anyStuckTicks, maxStuckAtOnce, tailStuckTicks);
            ck.Check(anyStuckTicks > 0, "塞がれた列では、前に仲間のいない先頭が Stuck になる");
            ck.Check(maxStuckAtOnce == 1, "塞がれた列で同時に Stuck になるのは先頭の 1 体だけ (後続は Moving のまま)");
            ck.Check(tailStuckTicks == 0, "塞がれた列の最後尾は Stuck にならない");
        }

        // 同じ目的地へ向かう N 体 (回避あり = 既定): 目的地の手前で渋滞しても全員がいずれ Arrived になる
        struct JamCase { int count; int avoidance; };
        for (const JamCase jam : { JamCase{ 2, 2 }, JamCase{ 4, 2 }, JamCase{ 8, 2 }, JamCase{ 8, 3 } }) {
            const int count = jam.count;
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float dest[3] = { 8.0f, 0.0f, 0.0f };
            std::vector<EntityID> crowdAgents;
            for (int i = 0; i < count; ++i) {
                const float z = (static_cast<float>(i) - 0.5f * static_cast<float>(count - 1)) * 0.9f;
                crowdAgents.push_back(AddAgent(scene, "Crowd", -8.0f, 0.0f, z, dest, true, jam.avoidance));
            }
            ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(渋滞) 開けた床をベイクできる");
            Sim sim(scene);
            int stuckAgentTicks = 0; // Stuck (= WARN の元) になった Agent の延べ数
            for (int i = 0; i < 1500; ++i) {
                sim.Step();
                for (const EntityID id : crowdAgents) {
                    stuckAgentTicks += sim.GetWorld().GetComponent<NavMeshAgentComponent>(id)->status == navagentstatus::kStuck ? 1 : 0;
                }
            }
            ck.Check(stuckAgentTicks == 0, "渋滞の後続は Stuck にならない (途中の Stuck / WARN が 0)");
            int arrivedCount = 0;
            for (const EntityID id : crowdAgents) {
                arrivedCount += sim.GetWorld().GetComponent<NavMeshAgentComponent>(id)->status == navagentstatus::kArrived ? 1 : 0;
            }
            MYE_LOG_INFO("  [jam] %d agents (avoidance %d): %d arrived, stuck agent-ticks %d", count, jam.avoidance, arrivedCount, stuckAgentTicks);
            for (const EntityID id : crowdAgents) {
                const auto* a = sim.GetWorld().GetComponent<NavMeshAgentComponent>(id);
                const auto* t = sim.GetWorld().GetComponent<LocalTransform>(id);
                MYE_LOG_INFO("  [jam]   status %d remaining %.3f at (%.2f, %.2f)", a->status, a->remainingDistance, t->position.x, t->position.z);
            }
            ck.Check(arrivedCount == count, "同じ目的地へ向かう N 体 (2 / 4 / 8、回避あり) は全員 Arrived になる (Stuck で恒久停止しない)");
        }

        // 渋滞の最中 (まだ着いていない) で撮って戻すと、連続実行と一致する (noProgressTicks / stuck を含む)
        {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float dest[3] = { 8.0f, 0.0f, 0.0f };
            for (int i = 0; i < 4; ++i) {
                AddAgent(scene, "Crowd", -8.0f, 0.0f, (static_cast<float>(i) - 1.5f) * 0.9f, dest, true);
            }
            ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(渋滞の復元) 開けた床をベイクできる");
            Sim sim(scene);
            SimRefs refs;
            refs.scene = &scene;
            refs.nav = &sim.nav;
            uint64_t tickRef = 0;
            refs.tickIndex = &tickRef;
            constexpr int kWarmTicks = 330; // 目的地の手前で止まり始める頃
            constexpr int kAheadTicks = 300;
            for (int i = 0; i < kWarmTicks; ++i) {
                sim.Step();
            }
            tickRef = sim.tick;
            std::vector<std::byte> blob;
            ck.Check(CaptureSimSnapshot(refs, blob), "(渋滞の復元) 渋滞の最中に撮影できる");
            std::vector<uint64_t> continuous;
            for (int i = 0; i < kAheadTicks; ++i) {
                sim.Step();
                continuous.push_back(WorldHashOf(sim, &sim.nav));
            }
            ck.Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "(渋滞の復元) 巻き戻せる");
            bool same = true;
            for (int i = 0; i < kAheadTicks; ++i) {
                sim.Step();
                same = same && WorldHashOf(sim, &sim.nav) == continuous[static_cast<size_t>(i)];
            }
            ck.Check(same, "渋滞の最中から復元して進めた毎 tick のハッシュが連続実行と一致する");
        }
    }

    // ---- 10. Modifier: エリアのコストと areaMask で経路が変わる。実行時に足す・消すと同じ tick で塗り直される ----
    {
        // 床の上を (-8, 0, 0) から (8, 0, 0) へ歩く。x = 0 の帯 (幅 2 m、z = -4..4) に Modifier。
        // mode 0 = Modifier なし / 1 = エリア 3 のコスト 20 / 2 = コスト 1 (突っ切る) / 3 = コスト 1 だが areaMask がエリア 3 を除く /
        // 4 = コスト 20 で tick 20 に Modifier を消す / 5 = エリア 1 (歩行不可)
        float crossingZ[6] = {};
        for (int mode = 0; mode < 6; ++mode) {
            Scene scene;
            AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
            const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
            const float dest[3] = { 8.0f, 0.0f, 0.0f };
            const EntityID walker = AddAgent(scene, "Walker", -8.0f, 0.0f, 0.0f, dest, true);
            EntityID zone = kNullEntity;
            if (mode != 0) {
                zone = AddModifier(scene, 0.0f, 1.0f, 0.0f, 2.0f, 2.0f, 8.0f, mode == 5 ? 1 : 3);
            }
            World& world = scene.GetWorld();
            world.GetComponent<NavMeshSurfaceComponent>(surface)->areaCosts[3] = (mode == 1 || mode == 4) ? 20.0f : 1.0f;
            if (mode == 3) {
                world.GetComponent<NavMeshAgentComponent>(walker)->areaMask = 0xFFFFFFFFu & ~(1u << 3);
            }
            ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(Modifier) 開けた床をベイクできる");
            Sim sim(scene);
            bool crossed = false;
            for (int i = 0; i < 900; ++i) {
                if (i == 20 && mode == 4) {
                    world.DestroyEntity(zone);
                    world.ApplyStructuralChanges();
                }
                sim.Step();
                if (i == 0 && mode != 0 && mode != 5) {
                    ck.Check(AreaAt(sim.nav, 0.0f, 0.0f, 0.0f) == 3 && AreaAt(sim.nav, -4.0f, 0.0f, 0.0f) == 0,
                             "Modifier のある最初の tick のうちに、箱の中だけがエリア 3 になる");
                }
                if (i == 0 && mode == 5) {
                    ck.Check(!HasPolyAt(sim.nav, 0.0f, 0.0f, 0.0f) && HasPolyAt(sim.nav, -4.0f, 0.0f, 0.0f),
                             "エリア 1 (歩行不可) の Modifier の中にはポリゴンが無い (外には残る)");
                }
                if (i == 20 && mode == 4) {
                    ck.Check(AreaAt(sim.nav, 0.0f, 0.0f, 0.0f) == 0 && sim.nav.Surfaces()[0].store->ObstacleCount() == 0,
                             "Modifier を消した tick のうちにエリアが元に戻る");
                }
                const auto* lt = world.GetComponent<LocalTransform>(walker);
                if (!crossed && lt->position.x >= 0.0f) {
                    crossed = true;
                    crossingZ[mode] = std::fabs(lt->position.z);
                }
            }
            const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
            const auto* lt = world.GetComponent<LocalTransform>(walker);
            MYE_LOG_INFO("  [modifier] mode %d: crossed x=0 at |z| %.2f; final status %d at (%.2f, %.2f)", mode, crossingZ[mode],
                         agent->status, lt->position.x, lt->position.z);
            ck.Check(agent->status == navagentstatus::kArrived && !agent->pathPartial && lt->position.x > 7.0f,
                     "(Modifier) 目的地へ着く");
        }
        ck.Check(crossingZ[0] < 1.0f, "Modifier が無ければ真っすぐ x = 0 を横切る (|z| < 1)");
        ck.Check(crossingZ[1] > 3.5f, "コスト 20 の Modifier の帯は避けて端を回り込む (|z| > 3.5)");
        ck.Check(crossingZ[2] < 1.5f, "コストを 1 に下げると帯を突っ切る (|z| < 1.5)");
        ck.Check(crossingZ[3] > 3.5f, "コスト 1 でも areaMask がエリア 3 を除けば帯を通らず回り込む (|z| > 3.5)");
        ck.Check(crossingZ[4] < 2.0f, "Modifier を消すと元の経路 (真っすぐ) へ戻る (|z| < 2)");
        ck.Check(crossingZ[5] > 3.5f, "エリア 1 (歩行不可) の帯は通れず回り込む (|z| > 3.5)");
    }

    // ---- 10b. 重なる Modifier は entity キーの大きい方が勝つ (store へ入れた順に依らない) / 動かす・消すで塗り直される ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        const EntityID a = AddModifier(scene, 0.0f, 1.0f, 0.0f, 4.0f, 2.0f, 4.0f, 0); // x = -2..2
        const EntityID b = AddModifier(scene, 1.0f, 1.0f, 0.0f, 4.0f, 2.0f, 4.0f, 0); // x = -1..3 (キーが大きい)
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(重なり) 開けた床をベイクできる");
        Sim sim(scene);
        World& world = sim.GetWorld();
        sim.Step();
        ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 2 && AreaAt(sim.nav, 0.5f, 0.0f, 0.0f) == 0,
                 "(重なり) エリア 0 の Modifier は何も変えない (どちらも TileCache には入る)");
        world.GetComponent<NavMeshModifierComponent>(b)->area = 4; // b を先に store へ入れ直す
        sim.Step();
        world.GetComponent<NavMeshModifierComponent>(a)->area = 3; // a は後
        sim.Step();
        ck.Check(AreaAt(sim.nav, -1.5f, 0.0f, 0.0f) == 3 && AreaAt(sim.nav, 0.5f, 0.0f, 0.0f) == 4
                     && AreaAt(sim.nav, 2.5f, 0.0f, 0.0f) == 4,
                 "(重なり) 重なった所は後から store へ入った a ではなくキーの大きい b (エリア 4) になる");
        world.GetComponent<LocalTransform>(b)->position.x += 10.0f;
        sim.Step();
        ck.Check(AreaAt(sim.nav, 0.5f, 0.0f, 0.0f) == 3 && AreaAt(sim.nav, 2.5f, 0.0f, 0.0f) == 0
                     && AreaAt(sim.nav, 11.5f, 0.0f, 0.0f) == 4,
                 "(重なり) b を動かした tick のうちに、元の場所は a のエリア 3 / b だけだった所は 0 に戻り、移動先がエリア 4 になる");
        world.DestroyEntity(a);
        world.ApplyStructuralChanges();
        sim.Step();
        ck.Check(AreaAt(sim.nav, 0.5f, 0.0f, 0.0f) == 0 && sim.nav.Surfaces()[0].store->ObstacleCount() == 1,
                 "(重なり) a を消した tick のうちに 0 に戻る");
        // Surface の範囲外の Modifier は TileCache に入れない
        AddModifier(scene, 100.0f, 1.0f, 0.0f, 2.0f, 2.0f, 2.0f, 3);
        world.ApplyStructuralChanges();
        sim.Step();
        ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 1, "(重なり) Surface の範囲外の Modifier は TileCache に入らない");
    }

    // ---- 10c. 同時に違う areaMask の Agent が居ても、それぞれの mask で経路を取る ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        AddModifier(scene, 0.0f, 1.0f, 0.0f, 2.0f, 2.0f, 8.0f, 3);
        const float destFree[3] = { 8.0f, 0.0f, -2.0f };
        const float destShy[3] = { 8.0f, 0.0f, 2.0f };
        const float destOutside[3] = { 8.0f, 0.0f, 7.0f };
        const EntityID free = AddAgent(scene, "Free", -8.0f, 0.0f, -2.0f, destFree, true);
        const EntityID shy = AddAgent(scene, "Shy", -8.0f, 0.0f, 2.0f, destShy, true);
        const EntityID shy2 = AddAgent(scene, "Shy2", -8.0f, 0.0f, 7.0f, destOutside, true);
        World& world = scene.GetWorld();
        world.GetComponent<NavMeshSurfaceComponent>(surface)->areaCosts[3] = 1.0f;
        world.GetComponent<NavMeshAgentComponent>(shy)->areaMask = 0xFFFFFFFFu & ~(1u << 3);
        world.GetComponent<NavMeshAgentComponent>(shy2)->areaMask = 0xFFFFu & ~(1u << 3); // 上位ビットが違っても同じ集合 (filter は 1 つ)
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(mask) 開けた床をベイクできる");
        Sim sim(scene);
        float crossFree = -1.0f;
        float crossShy = -1.0f;
        for (int i = 0; i < 900; ++i) {
            sim.Step();
            const auto* f = world.GetComponent<LocalTransform>(free);
            const auto* s = world.GetComponent<LocalTransform>(shy);
            if (crossFree < 0.0f && f->position.x >= 0.0f) {
                crossFree = std::fabs(f->position.z);
            }
            if (crossShy < 0.0f && s->position.x >= 0.0f) {
                crossShy = std::fabs(s->position.z);
            }
        }
        MYE_LOG_INFO("  [mask] Free crossed x=0 at |z| %.2f, Shy at |z| %.2f", crossFree, crossShy);
        ck.Check(world.GetComponent<NavMeshAgentComponent>(free)->status == navagentstatus::kArrived
                     && world.GetComponent<NavMeshAgentComponent>(shy)->status == navagentstatus::kArrived
                     && world.GetComponent<NavMeshAgentComponent>(shy2)->status == navagentstatus::kArrived,
                 "(mask) mask の違う Agent がどれも着く");
        ck.Check(crossShy > 3.5f, "(mask) エリア 3 を除いた Agent は帯を避ける");
        ck.Check(crossFree < 3.0f, "(mask) 同じ Surface の全エリア可の Agent は帯を突っ切る (filter を使い回していない)");
    }

    // ---- 10d. Modifier の SimSnapshot: 追加・移動・歩行不可・消去の途中で撮り、空の NavSystem / 元の NavSystem へ復元して連続実行と一致 ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        ModifierScript script;
        script.a = AddModifier(scene, 0.0f, 1.0f, 0.0f, 4.0f, 2.0f, 8.0f, 0);
        script.b = AddModifier(scene, 1.0f, 1.0f, 0.0f, 4.0f, 2.0f, 8.0f, 0);
        const float dests[3][3] = { { 8.0f, 0.0f, 0.0f }, { 8.0f, 0.0f, 4.0f }, { 8.0f, 0.0f, -4.0f } };
        for (int i = 0; i < 3; ++i) {
            const EntityID walker = AddAgent(scene, "Walker", -9.0f, 0.0f, -4.0f + 4.0f * static_cast<float>(i), dests[i], true, 1 + i);
            if (i == 1) {
                scene.GetWorld().GetComponent<NavMeshAgentComponent>(walker)->areaMask = 0xFFFFFFFFu & ~(1u << 3);
            }
        }
        scene.GetWorld().GetComponent<NavMeshSurfaceComponent>(surface)->areaCosts[3] = 6.0f;
        scene.GetWorld().GetComponent<NavMeshSurfaceComponent>(surface)->areaCosts[4] = 6.0f;
        scene.GetWorld().GetComponent<NavMeshSurfaceComponent>(surface)->areaCosts[5] = 3.0f;
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(Modifier 復元) 開けた床をベイクできる");

        Sim sim(scene);
        SimRefs refs;
        refs.scene = &scene;
        refs.nav = &sim.nav;
        uint64_t tickRef = 0;
        refs.tickIndex = &tickRef;

        constexpr int kWarm = 100; // b が 4 (30)、a が 3 (80) になった後、b の移動 (130) の前
        constexpr int kAhead = 280;
        for (int i = 0; i < kWarm; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
        }
        tickRef = sim.tick;
        ck.Check(sim.nav.Surfaces()[0].store->ObstacleCount() == 2 && AreaAt(sim.nav, 0.5f, 0.0f, 0.0f) == 4,
                 "(Modifier 復元) 2 つの Modifier が TileCache に入り、重なりは b のエリア 4");
        std::vector<std::byte> blob;
        ck.Check(CaptureSimSnapshot(refs, blob), "(Modifier 復元) Modifier がある状態で撮影できる");

        std::vector<uint64_t> continuous;
        std::vector<int> continuousCounts;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            continuous.push_back(WorldHashOf(sim, &sim.nav));
            continuousCounts.push_back(sim.nav.Surfaces()[0].store->ObstacleCount());
        }
        ck.Check(continuousCounts.back() == 1 && AreaAt(sim.nav, 0.0f, 0.0f, 0.0f) == 5,
                 "(Modifier 復元) 連続実行の終わりでは a だけが残り、エリア 5 になっている");

        Sim restored(scene);
        restored.tick = kWarm;
        SimRefs refs2 = refs;
        refs2.nav = &restored.nav;
        uint64_t tick2 = 0;
        refs2.tickIndex = &tick2;
        ck.Check(RestoreSimSnapshot(refs2, blob.data(), blob.size()), "(Modifier 復元) 空の NavSystem へ復元できる (戻り値 true)");
        const NavTileStore& restoredStore = *restored.nav.Surfaces()[0].store;
        ck.Check(restoredStore.ObstacleCount() == 2 && restoredStore.ObstacleAt(0).key == NavModifierKey(script.a)
                     && restoredStore.ObstacleAt(1).key == NavModifierKey(script.b) && AreaAt(restored.nav, 0.5f, 0.0f, 0.0f) == 4,
                 "(Modifier 復元) 復元直後の store の Modifier のキーとエリアが元と同じ");
        std::vector<std::byte> again;
        CaptureSimSnapshot(refs2, again);
        ck.Check(again == blob, "(Modifier 復元) 復元直後の再撮影が元の blob とバイト一致");
        bool same = true;
        bool countsSame = true;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(restored, restored.tick);
            restored.Step();
            if (i == 0) {
                ck.Check(restored.nav.Surfaces()[0].store->ObstacleCount() == 2,
                         "(Modifier 復元) 復元直後の Update が Modifier を二重に足さない");
            }
            same = same && WorldHashOf(restored, &restored.nav) == continuous[static_cast<size_t>(i)];
            countsSame = countsSame && restored.nav.Surfaces()[0].store->ObstacleCount() == continuousCounts[static_cast<size_t>(i)];
        }
        ck.Check(same, "(Modifier 復元) 空の NavSystem から 280 tick (移動・歩行不可・消去を含む) の毎 tick ハッシュが連続実行と一致");
        ck.Check(countsSame, "(Modifier 復元) 毎 tick の Modifier 数が連続実行と同じ (消し忘れ・二重追加なし)");

        ck.Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "(Modifier 復元) Modifier が変わった後の元の NavSystem へ巻き戻せる");
        sim.tick = kWarm;
        bool replay = true;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            replay = replay && WorldHashOf(sim, &sim.nav) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(replay, "(Modifier 復元) 元の NavSystem で巻き戻して再実行しても連続実行と一致する");

        bool stable = true;
        sim.tick = kWarm;
        RestoreSimSnapshot(refs, blob.data(), blob.size());
        for (int i = 0; i < kAhead && stable; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            std::vector<std::byte> b1;
            std::vector<std::byte> b2;
            CaptureSimSnapshot(refs, b1);
            RestoreSimSnapshot(refs, b1.data(), b1.size());
            CaptureSimSnapshot(refs, b2);
            stable = b1 == b2;
        }
        ck.Check(stable, "(Modifier 復元) Modifier の編集の間も tick ごとの 撮影 -> 復元 -> 再撮影 が一致し続ける");
    }

    // ---- 10e. 編集中の表示 (NavDebugView、NavSystem を渡さない): .mnav に Modifier を重ねた姿を、エリア色で映す ----
    {
        Scene scene;
        AddBox(scene, "Ground", 0.0f, -0.5f, 0.0f, 12.0f, 0.5f, 12.0f);
        const EntityID surface = AddSurface(scene, 13.0f, 13.0f);
        ck.Check(BakeSurface(scene, surface, kOpenGuid, nullptr), "(Modifier 表示) 開けた床をベイクできる");
        World& world = scene.GetWorld();
        NavDebugView view;
        view.Refresh(world, nullptr);
        const int flatTriangles = view.GetStats().lastTriangles;
        const EntityID zone = AddModifier(scene, 0.0f, 1.0f, 0.0f, 4.0f, 2.0f, 4.0f, 3);
        world.ApplyStructuralChanges();
        view.Refresh(world, nullptr);
        bool hasAreaColor = false;
        for (const DebugFillVertex& v : view.FillVertices()) {
            hasAreaColor = hasAreaColor || (std::fabs(v.r - 120.0f / 255.0f) < 0.02f && std::fabs(v.g - 220.0f / 255.0f) < 0.02f
                                            && std::fabs(v.b - 70.0f / 255.0f) < 0.02f);
        }
        ck.Check(view.GetStats().rebuildCount == 2 && view.GetStats().lastTriangles != flatTriangles && hasAreaColor,
                 "(Modifier 表示) 編集中も Modifier の範囲がエリア 3 の色で映る (作り直しは Modifier を足したフレームだけ)");
        view.Refresh(world, nullptr);
        ck.Check(view.GetStats().rebuildCount == 2, "(Modifier 表示) 変わらなければ作り直さない");
        world.GetComponent<LocalTransform>(zone)->position.x += 3.0f;
        view.Refresh(world, nullptr);
        ck.Check(view.GetStats().rebuildCount == 3, "(Modifier 表示) Modifier を動かすと作り直す");
        world.DestroyEntity(zone);
        world.ApplyStructuralChanges();
        view.Refresh(world, nullptr);
        ck.Check(view.GetStats().rebuildCount == 4 && view.GetStats().lastTriangles == flatTriangles,
                 "(Modifier 表示) Modifier を消すと元の形に戻る");
    }

    // ---- 11. Link: Linear / Jump / Manual で谷を渡り、片方向は逆向きに使わず、areaMask で絞れる ----
    {
        constexpr int kRunTicks = 900;
        const RavineRun noLink = RunRavine(-1, true, 0xFFFFFFFFu, -10.0f, 10.0f, kRunTicks);
        ck.Check(!noLink.sawOnLink && noLink.partial && noLink.x < -2.0f,
                 "(Link) Link が無ければ谷は渡れず、部分経路で手前の縁に着く");

        const RavineRun linear = RunRavine(navlinktraversal::kLinear, true, 0xFFFFFFFFu, -10.0f, 10.0f, kRunTicks);
        MYE_LOG_INFO("  [link] linear: status %d onLink %d ticks, x %.2f", linear.status, linear.onLinkTicks, linear.x);
        ck.Check(linear.sawOnLink && linear.status == navagentstatus::kArrived && !linear.partial
                     && std::fabs(linear.x - 10.0f) < 0.4f,
                 "(Link) Linear: 谷の向こうの目的地へ渡って着く (OnLink を経て Arrived、部分経路ではない)");
        ck.Check(linear.onLinkTicks >= 90 && linear.onLinkTicks <= 160,
                 "(Link) Linear: 渡る時間が 6 m / 3.5 m/s に近い (入口への近づきを含めて 90..160 tick)");
        ck.Check(linear.maxFeetY < 0.2f, "(Link) Linear: 直線で渡り、持ち上がらない");

        const RavineRun jump = RunRavine(navlinktraversal::kJump, true, 0xFFFFFFFFu, -10.0f, 10.0f, kRunTicks);
        MYE_LOG_INFO("  [link] jump: status %d onLink %d ticks, peak feet y %.2f", jump.status, jump.onLinkTicks, jump.maxFeetY);
        ck.Check(jump.sawOnLink && jump.status == navagentstatus::kArrived && std::fabs(jump.x - 10.0f) < 0.4f,
                 "(Link) Jump: 谷の向こうの目的地へ渡って着く");
        ck.Check(jump.maxFeetY > 0.9f && jump.maxFeetY < 1.2f, "(Link) Jump: 弧の頂点が jumpHeight (1 m) まで持ち上がる");

        const RavineRun back = RunRavine(navlinktraversal::kLinear, true, 0xFFFFFFFFu, 10.0f, -10.0f, kRunTicks);
        ck.Check(back.sawOnLink && back.status == navagentstatus::kArrived && std::fabs(back.x - (-10.0f)) < 0.4f,
                 "(Link) 双方向の Link は出口から入口へも渡れる");
        const RavineRun oneWay = RunRavine(navlinktraversal::kLinear, false, 0xFFFFFFFFu, 10.0f, -10.0f, kRunTicks);
        ck.Check(!oneWay.sawOnLink && oneWay.partial && oneWay.x > 2.0f,
                 "(Link) 片方向の Link は逆向きには使われない (出口側の Agent は渡れず、手前の縁で部分経路になる)");
        const RavineRun oneWayForward = RunRavine(navlinktraversal::kLinear, false, 0xFFFFFFFFu, -10.0f, 10.0f, kRunTicks);
        ck.Check(oneWayForward.sawOnLink && oneWayForward.status == navagentstatus::kArrived,
                 "(Link) 片方向の Link も順方向には渡れる");

        const RavineRun masked = RunRavine(navlinktraversal::kLinear, true, 0xFFFFFFFFu & ~(1u << 2), -10.0f, 10.0f, kRunTicks);
        ck.Check(!masked.sawOnLink && masked.partial && masked.x < -2.0f,
                 "(Link) areaMask が Link のエリア (2) を外している Agent は Link を使わない");

        // Manual: 入口で止まり、完了の通知まで動かない。通知すると出口へ移って歩き続ける
        Scene scene;
        const EntityID surface = BuildRavine(scene);
        const float linkStart[3] = { -3.0f, 0.0f, 0.0f };
        const float linkEnd[3] = { 3.0f, 0.0f, 0.0f };
        AddLink(scene, linkStart, linkEnd, navlinktraversal::kManual, true);
        const float dest[3] = { 10.0f, 0.0f, 0.0f };
        const EntityID walker = AddAgent(scene, "Walker", -10.0f, 0.0f, 0.0f, &dest[0], true);
        ck.Check(BakeSurface(scene, surface, kRavineGuid, nullptr), "(Link) 谷をベイクできる");
        Sim sim(scene);
        World& world = sim.GetWorld();
        for (int i = 0; i < 400; ++i) {
            sim.Step();
        }
        const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
        const float heldX = world.GetComponent<LocalTransform>(walker)->position.x;
        ck.Check(agent->status == navagentstatus::kOnLink && std::fabs(heldX - (-3.0f)) < 0.3f
                     && std::fabs(agent->linkStart.x - (-3.0f)) < 0.3f && std::fabs(agent->linkEnd.x - 3.0f) < 0.3f,
                 "(Link) Manual: 入口で止まって OnLink になり、linkStart / linkEnd を公開する");
        for (int i = 0; i < 300; ++i) {
            sim.Step();
        }
        ck.Check(world.GetComponent<NavMeshAgentComponent>(walker)->status == navagentstatus::kOnLink
                     && std::fabs(world.GetComponent<LocalTransform>(walker)->position.x - heldX) < 0.05f,
                 "(Link) Manual: 完了の通知が無い間は 300 tick たっても動かない");
        world.GetComponent<NavMeshAgentComponent>(walker)->linkComplete = true;
        for (int i = 0; i < 4; ++i) {
            sim.Step();
        }
        ck.Check(!world.GetComponent<NavMeshAgentComponent>(walker)->linkComplete
                     && world.GetComponent<LocalTransform>(walker)->position.x > 2.0f,
                 "(Link) Manual: 完了を通知すると出口へ移り、通知のフラグは NavSystem が戻す");
        for (int i = 0; i < 400; ++i) {
            sim.Step();
        }
        agent = world.GetComponent<NavMeshAgentComponent>(walker);
        ck.Check(agent->status == navagentstatus::kArrived && !agent->pathPartial
                     && std::fabs(world.GetComponent<LocalTransform>(walker)->position.x - 10.0f) < 0.4f,
                 "(Link) Manual: 渡り終えたあと目的地まで歩いて Arrived");
    }

    // ---- 11b. Link の実行時の編集: 足す・動かす・消すと同じ tick に TileCache が変わり、保存した状態へ復元できる ----
    {
        Scene scene;
        const EntityID surface = BuildRavine(scene);
        ck.Check(BakeSurface(scene, surface, kRavineGuid, nullptr), "(Link 編集) 谷をベイクできる");
        World& world = scene.GetWorld();
        Sim sim(scene);
        sim.Step();
        const NavTileStore& store = *sim.nav.Surfaces()[0].store;
        const uint64_t meshBefore = store.HashNavMesh(false);
        ck.Check(store.LinkCount() == 0 && store.ConnectedLinkCount() == 0, "(Link 編集) 最初は Link が無い");

        const float linkStart[3] = { -3.0f, 0.0f, 0.0f };
        const float linkEnd[3] = { 3.0f, 0.0f, 0.0f };
        const EntityID link = AddLink(scene, linkStart, linkEnd, navlinktraversal::kLinear, true);
        world.ApplyStructuralChanges();
        sim.Step();
        ck.Check(store.LinkCount() == 1 && store.ConnectedLinkCount() == 1 && store.HashNavMesh(false) != meshBefore,
                 "(Link 編集) Link を足した tick のうちに TileCache へ入る (入口のあるタイルが Off-Mesh 接続を持つ)");
        ck.Check(sim.nav.Stats().linkChanges == 1, "(Link 編集) その tick の linkChanges が 1");
        sim.Step();
        ck.Check(sim.nav.Stats().linkChanges == 0, "(Link 編集) 変わらない tick は何もしない");

        world.GetComponent<LocalTransform>(link)->position.z += 3.0f;
        sim.Step();
        ck.Check(store.LinkCount() == 1 && std::fabs(store.LinkAt(0).start[2] - 3.0f) < 0.01f && store.ConnectedLinkCount() == 1,
                 "(Link 編集) Link を動かすと入口が新しい位置へ付け直される");
        world.GetComponent<LocalTransform>(link)->position.z += 0.02f; // 閾値以内の揺れ
        sim.Step();
        ck.Check(sim.nav.Stats().linkChanges == 0, "(Link 編集) 閾値以内の揺れでは作り直さない");

        world.DestroyEntity(link);
        world.ApplyStructuralChanges();
        sim.Step();
        ck.Check(store.LinkCount() == 0 && store.ConnectedLinkCount() == 0 && store.HashNavMesh(false) == meshBefore,
                 "(Link 編集) Link を消すと元のナビメッシュに戻る");

        // 歩行面の無い所に入口がある Link は dtNavMesh に残らない (警告して無視される)
        const float farStart[3] = { 0.0f, 0.0f, 0.0f };
        AddLink(scene, farStart, linkEnd, navlinktraversal::kLinear, true);
        world.ApplyStructuralChanges();
        sim.Step();
        ck.Check(store.LinkCount() == 1 && store.ConnectedLinkCount() == 0,
                 "(Link 編集) 谷の真ん中 (床が無い) に入口がある Link は接続されず無視される");
    }

    // ---- 11c. Link の SimSnapshot: 渡りの途中 (Jump の空中と Manual の待機) で撮って、空の NavSystem / 元の NavSystem へ復元して連続実行と一致 ----
    {
        Scene scene;
        const EntityID surface = BuildRavine(scene);
        const float jumpStart[3] = { -3.0f, 0.0f, -4.0f };
        const float jumpEnd[3] = { 3.0f, 0.0f, -4.0f };
        const float manualStart[3] = { -3.0f, 0.0f, 4.0f };
        const float manualEnd[3] = { 3.0f, 0.0f, 4.0f };
        AddLink(scene, jumpStart, jumpEnd, navlinktraversal::kJump, true);
        AddLink(scene, manualStart, manualEnd, navlinktraversal::kManual, true);
        const float jumpDest[3] = { 10.0f, 0.0f, -4.0f };
        const float manualDest[3] = { 10.0f, 0.0f, 4.0f };
        const EntityID jumper = AddAgent(scene, "Jumper", -10.0f, 0.0f, -4.0f, &jumpDest[0], true, 1);
        LinkScript script;
        script.manualWalker = AddAgent(scene, "Waiter", -10.0f, 0.0f, 4.0f, &manualDest[0], true, 1);
        ck.Check(BakeSurface(scene, surface, kRavineGuid, nullptr), "(Link 復元) 谷をベイクできる");

        Sim sim(scene);
        SimRefs refs;
        refs.scene = &scene;
        refs.nav = &sim.nav;
        uint64_t tickRef = 0;
        refs.tickIndex = &tickRef;

        // Jumper が空中に出て 20 tick たつまで進める
        int airborne = 0;
        for (int i = 0; i < 400 && airborne < 20; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            const auto* a = sim.GetWorld().GetComponent<NavMeshAgentComponent>(jumper);
            airborne = a->status == navagentstatus::kOnLink ? airborne + 1 : 0;
        }
        const int warm = static_cast<int>(sim.tick);
        tickRef = sim.tick;
        constexpr int kAhead = 420;
        const auto* waiter = sim.GetWorld().GetComponent<NavMeshAgentComponent>(script.manualWalker);
        ck.Check(airborne >= 20 && waiter->status == navagentstatus::kOnLink && warm < 300,
                 "(Link 復元) 撮影の時点で Jumper は渡りの途中、Waiter は Manual の入口で待っている");
        std::vector<std::byte> blob;
        ck.Check(CaptureSimSnapshot(refs, blob), "(Link 復元) 渡りの途中で撮影できる");

        std::vector<uint64_t> continuous;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            continuous.push_back(WorldHashOf(sim, &sim.nav));
        }
        ck.Check(sim.GetWorld().GetComponent<NavMeshAgentComponent>(jumper)->status == navagentstatus::kArrived
                     && sim.GetWorld().GetComponent<NavMeshAgentComponent>(script.manualWalker)->status == navagentstatus::kArrived,
                 "(Link 復元) 連続実行の終わりでは 2 体とも谷を渡って Arrived");

        Sim restored(scene);
        restored.tick = static_cast<uint64_t>(warm);
        SimRefs refs2 = refs;
        refs2.nav = &restored.nav;
        uint64_t tick2 = 0;
        refs2.tickIndex = &tick2;
        ck.Check(RestoreSimSnapshot(refs2, blob.data(), blob.size()), "(Link 復元) 空の NavSystem へ復元できる (戻り値 true)");
        ck.Check(restored.nav.Surfaces()[0].store->LinkCount() == 2, "(Link 復元) 復元直後の store が Link を 2 本持つ");
        std::vector<std::byte> again;
        CaptureSimSnapshot(refs2, again);
        ck.Check(again == blob, "(Link 復元) 復元直後の再撮影が元の blob とバイト一致");
        bool same = true;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(restored, restored.tick);
            restored.Step();
            same = same && WorldHashOf(restored, &restored.nav) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(same, "(Link 復元) 空の NavSystem から 420 tick (着地・Manual の完了・歩行の再開) の毎 tick ハッシュが連続実行と一致");

        ck.Check(RestoreSimSnapshot(refs, blob.data(), blob.size()), "(Link 復元) 渡り終えた後の元の NavSystem へ巻き戻せる");
        sim.tick = static_cast<uint64_t>(warm);
        bool replay = true;
        for (int i = 0; i < kAhead; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            replay = replay && WorldHashOf(sim, &sim.nav) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(replay, "(Link 復元) 元の NavSystem で巻き戻して再実行しても連続実行と一致する");

        bool stable = true;
        sim.tick = static_cast<uint64_t>(warm);
        RestoreSimSnapshot(refs, blob.data(), blob.size());
        for (int i = 0; i < kAhead && stable; ++i) {
            script.Apply(sim, sim.tick);
            sim.Step();
            std::vector<std::byte> b1;
            std::vector<std::byte> b2;
            CaptureSimSnapshot(refs, b1);
            RestoreSimSnapshot(refs, b1.data(), b1.size());
            CaptureSimSnapshot(refs, b2);
            stable = b1 == b2;
        }
        ck.Check(stable, "(Link 復元) 渡りの間も tick ごとの 撮影 -> 復元 -> 再撮影 が一致し続ける");
    }

    // ---- 11d. 渡っている途中の Link の削除・移動・非アクティブ化・Surface の読み直し: 保存した出口まで渡り切ってから歩行へ戻る ----
    {
        enum class Edit { Destroy, Move, Deactivate, Reload };
        const char* const editNames[] = { "削除", "移動", "非アクティブ化", "Surface の読み直し" };
        const char* const modeNames[] = { "Linear", "Jump", "Manual" };
        constexpr uint64_t kRavineGuid2 = 0x4E41564147454E35ull;
        for (int32_t mode = navlinktraversal::kLinear; mode <= navlinktraversal::kManual; ++mode) {
            for (int e = 0; e < 4; ++e) {
                const Edit edit = static_cast<Edit>(e);
                char label[128];
                std::snprintf(label, sizeof(label), "(Link 途中変更) %s の渡りの途中で Link を%s", modeNames[mode], editNames[e]);
                Scene scene;
                const EntityID surface = BuildRavine(scene);
                const float linkStart[3] = { -3.0f, 0.0f, 0.0f };
                const float linkEnd[3] = { 3.0f, 0.0f, 0.0f };
                GameObject linkObject = AddLinkObject(scene, linkStart, linkEnd, mode, true);
                const EntityID link = linkObject.Id();
                const float dest[3] = { 10.0f, 0.0f, 0.0f };
                const EntityID walker = AddAgent(scene, "Walker", -10.0f, 0.0f, 0.0f, &dest[0], true, 1);
                if (!BakeSurface(scene, surface, kRavineGuid, nullptr)) {
                    ck.Check(false, "(Link 途中変更) 谷をベイクできる");
                    continue;
                }
                Sim sim(scene);
                World& world = sim.GetWorld();
                SimRefs refs;
                refs.scene = &scene;
                refs.nav = &sim.nav;
                uint64_t tickRef = 0;
                refs.tickIndex = &tickRef;

                int onLink = 0;
                for (int i = 0; i < 900 && onLink < 30; ++i) {
                    sim.Step();
                    onLink = world.GetComponent<NavMeshAgentComponent>(walker)->status == navagentstatus::kOnLink ? onLink + 1 : 0;
                }
                bool setupOk = onLink >= 30;
                switch (edit) {
                case Edit::Destroy:
                    world.DestroyEntity(link);
                    world.ApplyStructuralChanges();
                    break;
                case Edit::Move:
                    world.GetComponent<LocalTransform>(link)->position.z += 3.0f;
                    break;
                case Edit::Deactivate:
                    linkObject.AddComponent<ActiveComponent>()->enabled = false;
                    world.ApplyStructuralChanges();
                    break;
                case Edit::Reload:
                    setupOk = BakeSurface(scene, surface, kRavineGuid2, nullptr) && setupOk;
                    break;
                }
                sim.Step();
                const uint64_t completeTick = sim.tick + 20;
                const auto completeScript = [&](Sim& target) {
                    if (target.tick == completeTick) {
                        auto* a = target.GetWorld().GetComponent<NavMeshAgentComponent>(walker);
                        if (a != nullptr && a->status == navagentstatus::kOnLink) {
                            a->linkComplete = true;
                        }
                    }
                };
                const uint64_t warm = sim.tick;
                tickRef = sim.tick;
                std::vector<std::byte> blob;
                const bool captured = edit == Edit::Reload || CaptureSimSnapshot(refs, blob);

                constexpr int kAhead = 500;
                std::vector<uint64_t> continuous;
                bool finite = true;
                for (int i = 0; i < kAhead; ++i) {
                    completeScript(sim);
                    sim.Step();
                    finite = finite && FiniteAgent(world, walker);
                    continuous.push_back(WorldHashOf(sim, &sim.nav));
                }
                const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
                const auto* lt = world.GetComponent<LocalTransform>(walker);
                bool resultOk = false;
                if (edit == Edit::Reload) {
                    // 読み直しで渡りの状態は捨てられる。落ちず、NaN を出さず、渡り中のままにならない
                    resultOk = agent->status != navagentstatus::kOnLink;
                } else {
                    resultOk = agent->status == navagentstatus::kArrived && std::fabs(lt->position.x - 10.0f) < 0.5f;
                }
                ck.Check(setupOk && captured && finite && resultOk, label);

                if (edit != Edit::Reload) {
                    Sim restored(scene);
                    restored.tick = warm;
                    SimRefs refs2 = refs;
                    refs2.nav = &restored.nav;
                    uint64_t tick2 = 0;
                    refs2.tickIndex = &tick2;
                    bool same = RestoreSimSnapshot(refs2, blob.data(), blob.size());
                    for (int i = 0; same && i < kAhead; ++i) {
                        completeScript(restored);
                        restored.Step();
                        same = same && WorldHashOf(restored, &restored.nav) == continuous[static_cast<size_t>(i)];
                    }
                    char restoreLabel[160];
                    std::snprintf(restoreLabel, sizeof(restoreLabel), "%s: その直後に撮った状態の復元が連続実行と一致", label);
                    ck.Check(same, restoreLabel);
                }
            }
        }
    }

    // ---- 11e. つながらない Link の警告: 出口が 2 タイル以上離れた Link と、歩行面に入口が無い Link は WARN を状態が変わった tick に 1 回数える ----
    {
        Scene scene;
        const EntityID surface = BuildRavine(scene);
        ck.Check(BakeSurface(scene, surface, kRavineGuid, nullptr), "(Link 警告) 谷をベイクできる");
        World& world = scene.GetWorld();
        Sim sim(scene);
        const float nearStart[3] = { -3.0f, 0.0f, 0.0f };
        const float nearEnd[3] = { 3.0f, 0.0f, 0.0f };
        AddLink(scene, nearStart, nearEnd, navlinktraversal::kLinear, true);
        world.ApplyStructuralChanges();
        for (int i = 0; i < 3; ++i) {
            sim.Step();
        }
        ck.Check(sim.nav.Stats().linkDisconnected == 0 && sim.nav.Stats().linkWarnings == 0,
                 "(Link 警告) 隣のタイルまでの出口の Link はつながり、警告しない");

        const float farStart[3] = { -10.0f, 0.0f, 0.0f };
        const float farEnd[3] = { 10.0f, 0.0f, 0.0f };
        const EntityID farLink = AddLink(scene, farStart, farEnd, navlinktraversal::kLinear, true);
        world.ApplyStructuralChanges();
        for (int i = 0; i < 5; ++i) {
            sim.Step();
        }
        ck.Check(sim.nav.Surfaces()[0].store->LinkCount() == 2 && sim.nav.Surfaces()[0].store->ConnectedLinkCount() == 1
                     && sim.nav.Stats().linkDisconnected == 1 && sim.nav.Stats().linkWarnings == 1,
                 "(Link 警告) 出口が 3 タイル離れた Link はつながらず、警告は 1 回だけ (毎 tick は出さない)");
        std::vector<NavLinkSpec> specs;
        NavCollectLinkSpecs(world, specs);
        NavLinkSpec farSpec;
        for (const NavLinkSpec& spec : specs) {
            if (spec.key == NavLinkKey(farLink)) {
                farSpec = spec;
            }
        }
        ck.Check(specs.size() == 2 && NavCheckLinkPlacement(world, farSpec) == NavLinkPlacement::ExitTooFar
                     && NavCheckLinkPlacement(world, specs[0].key == farSpec.key ? specs[1] : specs[0]) == NavLinkPlacement::Ok,
                 "(Link 警告) 静的な検査 (Inspector 用) が 2 タイル以上離れた出口を見つけ、近い Link は通す");

        const float holeStart[3] = { 0.0f, 0.0f, 0.0f }; // 谷の真ん中 (床が無い)
        AddLink(scene, holeStart, nearEnd, navlinktraversal::kLinear, true);
        world.ApplyStructuralChanges();
        for (int i = 0; i < 5; ++i) {
            sim.Step();
        }
        ck.Check(sim.nav.Stats().linkDisconnected == 2 && sim.nav.Stats().linkWarnings == 2,
                 "(Link 警告) 入口に歩行面が無い Link を足すと、数が変わった tick に 1 回だけ警告が増える");

        const float outsideStart[3] = { 50.0f, 0.0f, 0.0f };
        const EntityID outside = AddLink(scene, outsideStart, nearEnd, navlinktraversal::kLinear, true);
        world.ApplyStructuralChanges();
        NavCollectLinkSpecs(world, specs);
        NavLinkSpec outsideSpec;
        for (const NavLinkSpec& spec : specs) {
            if (spec.key == NavLinkKey(outside)) {
                outsideSpec = spec;
            }
        }
        ck.Check(NavCheckLinkPlacement(world, outsideSpec) == NavLinkPlacement::NoSurface,
                 "(Link 警告) 静的な検査が、どの Surface にも入らない入口を見つける");
    }

    // ---- 11f. 親を持つ Agent の渡り: 親を動かして回した状態でも、渡り切った位置がワールドで正しい ----
    for (int rotated = 0; rotated < 2; ++rotated) {
        Scene scene;
        const EntityID surface = BuildRavine(scene);
        const float linkStart[3] = { -3.0f, 0.0f, 0.0f };
        const float linkEnd[3] = { 3.0f, 0.0f, 0.0f };
        AddLink(scene, linkStart, linkEnd, navlinktraversal::kJump, true);
        GameObject rig = scene.CreateGameObjectTracked("Rig");
        rig.SetLocalPosition(2.0f, 0.0f, -1.0f);
        if (rotated != 0) {
            rig.GetComponent<LocalTransform>()->rotation = { 0.0f, 0.70710678f, 0.0f, 0.70710678f }; // y 軸まわり 90 度
        }
        GameObject child = scene.CreateGameObjectTracked("ChildWalker");
        child.SetParent(rig);
        // ワールド (-10, 0.9, 0) = 回転 90 度 (x' = z, z' = -x) と平行移動 (2, 0, -1) を戻したローカル座標
        if (rotated != 0) {
            child.SetLocalPosition(-1.0f, 0.9f, -12.0f);
        } else {
            child.SetLocalPosition(-12.0f, 0.9f, 1.0f); // 回さないときは平行移動 (2, 0, -1) を引いただけ
        }
        child.AddComponent<CharacterControllerComponent>();
        auto* na = child.AddComponent<NavMeshAgentComponent>();
        na->destination = { 10.0f, 0.0f, 0.0f };
        na->hasDestination = true;
        na->avoidanceQuality = 1;
        Sim sim(scene);
        World& world = sim.GetWorld();
        // Sim の TransformSystem に階層を組ませてからベイクする (ベイクの TransformSystem が階層の dirty を消すため)
        world.ApplyStructuralChanges();
        sim.transforms.Update(world);
        ck.Check(BakeSurface(scene, surface, kRavineGuid, nullptr), "(Link 親付き) 谷をベイクできる");
        bool sawOnLink = false;
        for (int i = 0; i < 1000; ++i) {
            sim.Step();
            sawOnLink = sawOnLink || world.GetComponent<NavMeshAgentComponent>(child.Id())->status == navagentstatus::kOnLink;
        }
        const auto* wm = world.GetComponent<WorldMatrixComponent>(child.Id());
        const auto* agent = world.GetComponent<NavMeshAgentComponent>(child.Id());
        MYE_LOG_INFO("  [link] parented walker (rotated %d): status %d world (%.2f, %.2f, %.2f)", rotated, agent->status, wm->value.m[3][0],
                     wm->value.m[3][1], wm->value.m[3][2]);
        ck.Check(sawOnLink && agent->status == navagentstatus::kArrived && std::fabs(wm->value.m[3][0] - 10.0f) < 0.5f
                     && std::fabs(wm->value.m[3][2]) < 0.5f && std::fabs(wm->value.m[3][1] - 0.9f) < 0.3f,
                 rotated != 0 ? "(Link 親付き) 平行移動と 90 度回転をした親の子の Agent が谷を渡り、ワールドの目的地に着く" : "(Link 親付き) 平行移動した親の子の Agent が谷を渡り、ワールドの目的地に着く");
    }

    // ---- 12. スクリプト API (ABI v24): EngineApi のテーブル越しに Nav* を呼ぶ ----
    {
        Scene scene;
        const Yard yard = BuildYard(scene);
        const EntityID walker = AddAgent(scene, "Walker", -9.0f, 0.0f, -9.0f, nullptr, true);
        ck.Check(BakeSurface(scene, yard.surface, kApiGuid, nullptr), "(API) 庭をベイクできる");
        Sim sim(scene);
        World& world = sim.GetWorld();
        ScriptApiContext sctx;
        sctx.scene = &scene;
        sctx.nav = &sim.nav;
        MyeEngineApi api;
        BuildEngineApi(api, &sctx);
        const MyeEntityId walkerId = ToShared(walker);
        const MyeVec3 from = { -9.0f, 0.0f, -9.0f };
        MyeVec3 corners[32] = {};
        int32_t partial = -1;

        ck.Check(api.NavFindPath(api.engine, 0, from, { -9.0f, 0.0f, -5.0f }, 0xFFFFFFFFu, corners, 32, &partial) == 0 && partial == 0,
                 "(API) NavSystem の最初の Update より前 (Surface が未読み込み) は NavFindPath が 0 を返す");
        MyeVec3 sampled = {};
        ck.Check(api.NavSamplePosition(api.engine, 0, from, { 1.0f, 2.0f, 1.0f }, 0xFFFFFFFFu, &sampled) == 0,
                 "(API) 同じく NavSamplePosition が 0 を返す");
        for (int i = 0; i < 3; ++i) {
            sim.Step();
        }

        // 経路
        int32_t n = api.NavFindPath(api.engine, 0, from, { -9.0f, 0.0f, -5.0f }, 0xFFFFFFFFu, corners, 32, &partial);
        ck.Check(n == 2 && partial == 0 && std::fabs(corners[0].x - from.x) < 0.1f && std::fabs(corners[1].z - (-5.0f)) < 0.05f
                     && std::fabs(corners[1].x - (-9.0f)) < 0.05f,
                 "(API) NavFindPath: 遮るもののない直線は角 2 つ (始点と終点)、partial = 0");
        n = api.NavFindPath(api.engine, 0, from, { 10.0f, 0.0f, -9.0f }, 0xFFFFFFFFu, corners, 32, &partial);
        float pathLength = 0.0f;
        for (int i = 1; i < n; ++i) {
            const float dx = corners[i].x - corners[i - 1].x;
            const float dz = corners[i].z - corners[i - 1].z;
            pathLength += std::sqrt(dx * dx + dz * dz);
        }
        MYE_LOG_INFO("  [api] path around the deck: %d corners, length %.2f m", static_cast<int>(n), pathLength);
        ck.Check(n >= 3 && partial == 0 && pathLength > 19.2f && std::fabs(corners[n - 1].x - 10.0f) < 0.05f,
                 "(API) NavFindPath: 低い台 (登れない高さ) をはさむ経路は迂回する (角 3 つ以上、直線距離 19 m より長い)");
        MyeVec3 firstOnly[1] = {};
        ck.Check(api.NavFindPath(api.engine, 0, from, { 10.0f, 0.0f, -9.0f }, 0xFFFFFFFFu, firstOnly, 1, nullptr) == 1,
                 "(API) NavFindPath: maxCorners を超える分は切り捨てる (null の outPartial も受ける)");
        n = api.NavFindPath(api.engine, 0, from, { 0.0f, 3.0f, 8.0f }, 0xFFFFFFFFu, corners, 32, &partial);
        ck.Check(n >= 1 && partial == 1, "(API) NavFindPath: 孤島の上 (届かない目的地) は届く限りの最寄りまで返し partial = 1");
        ck.Check(api.NavFindPath(api.engine, 0, from, { 50.0f, 0.0f, 0.0f }, 0xFFFFFFFFu, corners, 32, &partial) == 0 && partial == 0,
                 "(API) NavFindPath: 目的地の近くにナビメッシュが無ければ 0");
        ck.Check(api.NavFindPath(api.engine, 7, from, { -9.0f, 0.0f, -5.0f }, 0xFFFFFFFFu, corners, 32, &partial) == 0,
                 "(API) NavFindPath: agentTypeId の Surface が無ければ 0");
        ck.Check(api.NavFindPath(api.engine, 0, from, { -9.0f, 0.0f, -5.0f }, 0u, corners, 32, &partial) == 0
                     && api.NavFindPath(api.engine, 0, from, { -9.0f, 0.0f, -5.0f }, 0xFFFFFFFFu, nullptr, 32, &partial) == 0
                     && api.NavFindPath(api.engine, 0, from, { -9.0f, 0.0f, -5.0f }, 0xFFFFFFFFu, corners, 0, &partial) == 0
                     && api.NavFindPath(api.engine, 0, from, { NAN, 0.0f, 0.0f }, 0xFFFFFFFFu, corners, 32, &partial) == 0,
                 "(API) NavFindPath: areaMask 0 / 出力なし / maxCorners 0 / 非有限の座標は 0");

        // 最寄り点
        ck.Check(api.NavSamplePosition(api.engine, 0, { 0.0f, 5.0f, 0.0f }, { 1.0f, 6.0f, 1.0f }, 0xFFFFFFFFu, &sampled) == 1
                     && std::fabs(sampled.x) < 1.01f && std::fabs(sampled.y) < 0.3f && std::fabs(sampled.z) < 1.01f,
                 "(API) NavSamplePosition: 空中の点の最寄りは床の上");
        ck.Check(api.NavSamplePosition(api.engine, 0, { 100.0f, 0.0f, 100.0f }, { 1.0f, 2.0f, 1.0f }, 0xFFFFFFFFu, &sampled) == 0
                     && api.NavSamplePosition(api.engine, 0, { 0.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 1.0f }, 0xFFFFFFFFu, nullptr) == 0,
                 "(API) NavSamplePosition: 範囲内にナビメッシュが無い / 出力なしは 0");

        // レイ
        MyeNavRaycastHit hit = {};
        const int rayOk = api.NavRaycast(api.engine, 0, from, { 10.0f, 0.0f, -9.0f }, 0xFFFFFFFFu, &hit);
        MYE_LOG_INFO("  [api] raycast: ok %d hit %d point (%.2f, %.2f, %.2f) dist %.2f normal (%.2f, %.2f, %.2f)", rayOk, hit.hit,
                     hit.point.x, hit.point.y, hit.point.z, hit.distance, hit.normal.x, hit.normal.y, hit.normal.z);
        ck.Check(rayOk == 1 && hit.hit == 1 && hit.point.x > 3.5f && hit.point.x < 5.0f && hit.distance > 12.0f && hit.distance < 14.5f
                     && std::fabs(hit.normal.x) > 0.5f,
                 "(API) NavRaycast: 低い台の手前で止まり、止まった点・距離・法線を返す");
        ck.Check(api.NavRaycast(api.engine, 0, from, { -9.0f, 0.0f, -5.0f }, 0xFFFFFFFFu, &hit) == 1 && hit.hit == 0
                     && std::fabs(hit.point.z - (-5.0f)) < 0.001f && std::fabs(hit.distance - 4.0f) < 0.3f,
                 "(API) NavRaycast: 遮るものが無ければ hit = 0 で point は終点");
        ck.Check(api.NavRaycast(api.engine, 0, { 50.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, 0xFFFFFFFFu, &hit) == 0,
                 "(API) NavRaycast: 始点がナビメッシュに乗らなければ 0");

        // ランダム点 (World の RNG)
        const MyeVec3 center = { -9.0f, 0.0f, 5.0f };
        const Pcg32 saved = world.Rng();
        MyeVec3 p1 = {};
        MyeVec3 p2 = {};
        const int r1 = api.NavFindRandomPoint(api.engine, 0, center, 3.0f, 0xFFFFFFFFu, &p1);
        const Pcg32 afterOne = world.Rng();
        world.Rng() = saved;
        const int r2 = api.NavFindRandomPoint(api.engine, 0, center, 3.0f, 0xFFFFFFFFu, &p2);
        ck.Check(r1 == 1 && r2 == 1 && p1.x == p2.x && p1.y == p2.y && p1.z == p2.z,
                 "(API) NavFindRandomPoint: World の RNG を戻せば同じ点 (決定論)");
        Pcg32 cmpA = saved;
        Pcg32 cmpB = afterOne;
        ck.Check(cmpA.NextU32() != cmpB.NextU32(), "(API) NavFindRandomPoint: World の RNG を進める");
        bool allInside = true;
        for (int i = 0; i < 40; ++i) {
            MyeVec3 p = {};
            allInside = allInside && api.NavFindRandomPoint(api.engine, 0, center, 3.0f, 0xFFFFFFFFu, &p) == 1;
            const float dx = p.x - center.x;
            const float dz = p.z - center.z;
            allInside = allInside && std::sqrt(dx * dx + dz * dz) <= 3.0f + 0.1f && std::fabs(p.y) < 0.3f;
        }
        ck.Check(allInside, "(API) NavFindRandomPoint: 40 回とも半径内のナビメッシュ上の点");
        const Pcg32 beforeFail = world.Rng();
        MyeVec3 unused = {};
        const int failed = api.NavFindRandomPoint(api.engine, 0, { 100.0f, 0.0f, 100.0f }, 3.0f, 0xFFFFFFFFu, &unused)
                         + api.NavFindRandomPoint(api.engine, 0, center, 0.0f, 0xFFFFFFFFu, &unused)
                         + api.NavFindRandomPoint(api.engine, 7, center, 3.0f, 0xFFFFFFFFu, &unused);
        Pcg32 cmpC = beforeFail;
        Pcg32 cmpD = world.Rng();
        ck.Check(failed == 0 && cmpC.NextU32() == cmpD.NextU32(),
                 "(API) NavFindRandomPoint: 失敗 (範囲外 / 半径 0 / Surface なし) は 0 を返し RNG を引かない");

        // Agent の制御
        const MyeVec3 target = { -9.0f, 0.0f, -4.0f };
        ck.Check(api.NavSetDestination(api.engine, walkerId, target) == 1, "(API) NavSetDestination: Agent に目的地を書ける");
        for (int i = 0; i < 6; ++i) {
            sim.Step();
        }
        MyeNavAgentState state = {};
        const auto* agent = world.GetComponent<NavMeshAgentComponent>(walker);
        ck.Check(api.NavGetAgentState(api.engine, walkerId, &state) == 1 && state.status == navagentstatus::kMoving
                     && state.status == agent->status && state.remainingDistance > 1.0f
                     && state.remainingDistance == agent->remainingDistance && state.pathPartial == 0,
                 "(API) NavGetAgentState: 歩き出した Agent は Moving と残り距離を返す (コンポーネントの値と一致)");
        for (int i = 0; i < 400; ++i) {
            sim.Step();
        }
        ck.Check(api.NavGetAgentState(api.engine, walkerId, &state) == 1 && state.status == navagentstatus::kArrived,
                 "(API) NavGetAgentState: 着いたら Arrived");
        ck.Check(api.NavSetDestination(api.engine, walkerId, { -9.0f, 0.0f, -9.0f }) == 1 && api.NavStop(api.engine, walkerId) == 1,
                 "(API) NavStop: 目的地を外せる");
        sim.Step();
        sim.Step();
        ck.Check(!agent->hasDestination && agent->status == navagentstatus::kIdle && api.NavGetAgentState(api.engine, walkerId, &state) == 1
                     && state.status == navagentstatus::kIdle,
                 "(API) NavStop: 次の tick で Idle になる");
        const MyeEntityId surfaceId = ToShared(yard.surface);
        ck.Check(api.NavSetDestination(api.engine, surfaceId, target) == 0 && api.NavStop(api.engine, surfaceId) == 0
                     && api.NavGetAgentState(api.engine, surfaceId, &state) == 0 && api.NavCompleteLink(api.engine, surfaceId) == 0
                     && api.NavGetAgentState(api.engine, MyeEntityId{}, &state) == 0,
                 "(API) NavMeshAgent を持たないエンティティ / null id には 0 を返す");
        ck.Check(api.NavCompleteLink(api.engine, walkerId) == 0 && !agent->linkComplete,
                 "(API) NavCompleteLink: Link を渡っていない Agent には何も書かず 0");

        // nav を繋いでいないテーブルは Nav* のクエリが 0
        ScriptApiContext bare;
        bare.scene = &scene;
        MyeEngineApi bareApi;
        BuildEngineApi(bareApi, &bare);
        ck.Check(bareApi.NavFindPath(bareApi.engine, 0, from, target, 0xFFFFFFFFu, corners, 32, &partial) == 0
                     && bareApi.NavRaycast(bareApi.engine, 0, from, target, 0xFFFFFFFFu, &hit) == 0
                     && bareApi.NavFindRandomPoint(bareApi.engine, 0, center, 3.0f, 0xFFFFFFFFu, &unused) == 0
                     && bareApi.NavCompleteLink(bareApi.engine, walkerId) == 0,
                 "(API) NavSystem を繋いでいない構成ではクエリ系が 0 を返す");
    }

    // ---- 12b. スクリプト API: Manual の Link の完了通知 (NavCompleteLink) ----
    {
        Scene scene;
        const EntityID surface = BuildRavine(scene);
        const float linkStart[3] = { -3.0f, 0.0f, 0.0f };
        const float linkEnd[3] = { 3.0f, 0.0f, 0.0f };
        AddLink(scene, linkStart, linkEnd, navlinktraversal::kManual, true);
        const float dest[3] = { 10.0f, 0.0f, 0.0f };
        const EntityID walker = AddAgent(scene, "Walker", -10.0f, 0.0f, 0.0f, &dest[0], true);
        ck.Check(BakeSurface(scene, surface, kRavineGuid, nullptr), "(API Link) 谷をベイクできる");
        Sim sim(scene);
        World& world = sim.GetWorld();
        ScriptApiContext sctx;
        sctx.scene = &scene;
        sctx.nav = &sim.nav;
        MyeEngineApi api;
        BuildEngineApi(api, &sctx);
        const MyeEntityId walkerId = ToShared(walker);
        sim.Step();
        ck.Check(api.NavCompleteLink(api.engine, walkerId) == 0 && !world.GetComponent<NavMeshAgentComponent>(walker)->linkComplete,
                 "(API Link) 入口へ近づく前の通知は無視する (次の Link に持ち越さない)");
        for (int i = 0; i < 400; ++i) {
            sim.Step();
        }
        MyeNavAgentState state = {};
        ck.Check(api.NavGetAgentState(api.engine, walkerId, &state) == 1 && state.status == navagentstatus::kOnLink,
                 "(API Link) Manual の入口で止まっている間、NavGetAgentState は OnLink");
        ck.Check(api.NavCompleteLink(api.engine, walkerId) == 1, "(API Link) NavCompleteLink: 止まっている Agent の通知は受け付ける");
        for (int i = 0; i < 4; ++i) {
            sim.Step();
        }
        ck.Check(world.GetComponent<LocalTransform>(walker)->position.x > 2.0f
                     && !world.GetComponent<NavMeshAgentComponent>(walker)->linkComplete,
                 "(API Link) 通知すると出口へ移る");
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
