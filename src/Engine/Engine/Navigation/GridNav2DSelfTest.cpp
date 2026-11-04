//====================================================================================
//                          GridNav2DSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          GridAgent2D (タイルマップ上のグリッド A*) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Navigation/GridNav2DSelfTest.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Navigation/GridNav2D.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr uint32_t kTileFloor = 1;
constexpr uint32_t kTilePlatform = 2; // 一方通行

// Debug / Release で同じ値になること (M94u で採取)。シナリオ A を 600 tick 回した HashWorld
constexpr uint64_t kExpectedScenarioHash = 0xB044B926F1F69A1Bull;

TilePalette MakePalette()
{
    TilePalette p;
    p.Add("floor", AssetID{ 201 })->collider = 1;
    TileDef* platform = p.Add("platform", AssetID{ 202 });
    platform->collider = 1;
    platform->oneWay = true;
    return p;
}

void FillColumn(TilemapLayer& layer, int32_t x, int32_t yFrom, int32_t yTo, uint32_t tile)
{
    for (int32_t y = yFrom; y <= yTo; ++y) {
        layer.SetCell(x, y, tilecell::Make(tile, false, false, 0));
    }
}

// 10 x 10 の面。床の天面は y = 1 (x = 0..4)。x = 3 に穴 (1 マス)、x >= 5 は段差 2 の高台 (天面 y = 3)
TilemapAsset MakeStepMap(uint64_t paletteHash)
{
    TilemapAsset m;
    m.palette = paletteHash;
    TilemapLayer ground;
    ground.name = "Ground";
    for (int32_t x = 0; x <= 4; ++x) {
        if (x != 3) {
            FillColumn(ground, x, 0, 0, kTileFloor);
        }
    }
    for (int32_t x = 5; x <= 9; ++x) {
        FillColumn(ground, x, 0, 2, kTileFloor);
    }
    m.layers.push_back(std::move(ground));
    return m;
}

// 到達不能: 床 (x = 0..9) と、跳べない高さ (6 セル上) に浮いた島
TilemapAsset MakeIslandMap(uint64_t paletteHash)
{
    TilemapAsset m;
    m.palette = paletteHash;
    TilemapLayer ground;
    ground.name = "Ground";
    for (int32_t x = 0; x <= 9; ++x) {
        FillColumn(ground, x, 0, 0, kTileFloor);
    }
    for (int32_t x = 6; x <= 8; ++x) {
        FillColumn(ground, x, 6, 6, kTileFloor);
    }
    m.layers.push_back(std::move(ground));
    return m;
}

// 展開上限 (4096) を超える一本道の床 (x = 0..kWideFloor-1) と、届かない島。届く全セルを数え終える前に打ち切られる
constexpr int32_t kWideFloor = 5000;
TilemapAsset MakeWideMap(uint64_t paletteHash)
{
    TilemapAsset m;
    m.palette = paletteHash;
    TilemapLayer ground;
    ground.name = "Ground";
    for (int32_t x = 0; x < kWideFloor; ++x) {
        FillColumn(ground, x, 0, 0, kTileFloor);
    }
    FillColumn(ground, 2500, 8, 8, kTileFloor);
    m.layers.push_back(std::move(ground));
    return m;
}

// 物理 + Transform + Tilemap + GridNav を束ねた sim 1 本ぶん
struct NavSim {
    Scene scene;
    PhysicsSystem phys;
    TransformSystem transforms;
    TilemapSystem tiles;
    GridNavSystem nav;
    EntityID mapEntity = kNullEntity;
    EntityID agent = kNullEntity;

    NavSim()
    {
        phys.AddStaticShapeSource(&tiles);
    }
    NavSim(const NavSim&) = delete;
    NavSim& operator=(const NavSim&) = delete;

    World& GetWorld() { return scene.GetWorld(); }
    void Build(AssetID mapId, float agentX, float agentY, float goalX, float goalY, bool withCc = true, bool plane2D = true)
    {
        GameObject env = scene.CreateGameObjectTracked("Env");
        env.AddComponent<PhysicsEnvironmentComponent>();
        GameObject map = scene.CreateGameObjectTracked("Map");
        map.AddComponent<TilemapComponent>()->tilemap = mapId;
        mapEntity = map.Id();
        GameObject go = scene.CreateGameObjectTracked("Agent");
        go.SetLocalPosition(agentX, agentY, 0.0f);
        if (withCc) {
            go.AddComponent<CharacterControllerComponent>()->plane2D = plane2D;
        }
        auto* ga = go.AddComponent<GridAgent2DComponent>();
        ga->tilemap = mapEntity;
        ga->destination = { goalX, goalY, 0.0f };
        ga->hasDestination = true;
        agent = go.Id();
        GetWorld().ApplyStructuralChanges();
        transforms.Update(GetWorld());
    }
    void Step(int ticks)
    {
        for (int i = 0; i < ticks; ++i) {
            nav.Update(GetWorld(), &tiles, kDt);
            phys.Update(GetWorld(), kDt);
            transforms.Update(GetWorld());
        }
    }
    GridAgent2DComponent& Agent() { return *GetWorld().GetComponent<GridAgent2DComponent>(agent); }
    const LocalTransform& Pose() { return *GetWorld().GetComponent<LocalTransform>(agent); }
    SimRefs Refs()
    {
        SimRefs refs;
        refs.scene = &scene;
        refs.tilemap = &tiles;
        return refs;
    }
    uint64_t Hash() { return HashWorld(GetWorld(), Refs().HashSources()); }
};

// status が target になるまで最大 maxTicks 回す。着いた tick を返す (着かなければ -1)
int StepUntilStatus(NavSim& sim, int32_t target, int maxTicks)
{
    for (int tick = 1; tick <= maxTicks; ++tick) {
        sim.Step(1);
        if (sim.Agent().status == target) {
            return tick;
        }
    }
    return -1;
}

} // namespace

bool RunGridNav2DSelfTest()
{
    MYE_LOG_INFO("==== GridNav2D self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    RegisterBuiltinComponents(); // 多重呼び出しは無害
    {
        check(GridAgent2DComponent::sTypeId == 91, "GridAgent2D is TypeId 91 (after Effector2D 90)");
        const ComponentDesc& d = ComponentRegistry::Get().Desc(GridAgent2DComponent::sTypeId);
        check((d.flags & kComponentNoHash) == 0, "GridAgent2D is hashed (sim output)");
    }

    TilePaletteLibrary palettes;
    TilemapLibrary maps;
    TilePaletteLibrary* previousPalettes = tilepalette::Library();
    TilemapLibrary* previousMaps = tilemap::Library();
    tilepalette::Install(&palettes);
    tilemap::Install(&maps);
    const uint64_t paletteHash = palettes.Register(L"C:\\sel\\gridnav\\grid.tilepalette.json", MakePalette());
    const AssetID stepMap{ maps.Register(L"C:\\sel\\gridnav\\step.tilemap.json", MakeStepMap(paletteHash)) };
    const AssetID islandMap{ maps.Register(L"C:\\sel\\gridnav\\island.tilemap.json", MakeIslandMap(paletteHash)) };
    const AssetID wideMap{ maps.Register(L"C:\\sel\\gridnav\\wide.tilemap.json", MakeWideMap(paletteHash)) };
    check(paletteHash != 0 && stepMap.value != 0 && islandMap.value != 0 && wideMap.value != 0,
          "gridnav: test assets registered");

    // ---- シナリオ A: 穴を跳び、段差 2 を跳び上がって高台の端へ着く ----
    // 足元は床の天面 y = 1。CC の高さ 1.8 なので中心は 1.9
    uint64_t scenarioHash = 0;
    {
        NavSim sim;
        sim.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f);
        const int arrivedAt = StepUntilStatus(sim, navagentstatus::kArrived, 900);
        MYE_LOG_INFO("  [gridnav] scenario A: arrived at tick %d, pos (%.3f, %.3f), status %d", arrivedAt,
                     static_cast<double>(sim.Pose().position.x), static_cast<double>(sim.Pose().position.y),
                     sim.Agent().status);
        check(arrivedAt > 0, "gridnav: a 10x10 face with a 1-wide hole and a 2-high step is crossed (status Arrived)");
        check(sim.Pose().position.x > 9.0f && std::fabs(sim.Pose().position.y - 3.9f) < 0.1f,
              "gridnav: the agent ends on the far ledge (feet on y = 3)");
        sim.Step(60);
        check(sim.Agent().status == navagentstatus::kArrived, "gridnav: Arrived is stable while the goal does not change");
        sim.Step(600 - 60 - arrivedAt);
        scenarioHash = sim.Hash();

        // 目的地を取り下げると止まって Idle
        sim.Agent().hasDestination = false;
        sim.Step(1);
        check(sim.Agent().status == navagentstatus::kIdle && sim.GetWorld().GetComponent<CharacterControllerComponent>(sim.agent)->moveInput.x == 0.0f,
              "gridnav: clearing the destination stops the agent (Idle, moveInput.x = 0)");
    }
    MYE_LOG_INFO("  [gridnav] scenario A hash 0x%016llX", static_cast<unsigned long long>(scenarioHash));
    check(kExpectedScenarioHash == 0 || scenarioHash == kExpectedScenarioHash,
          "gridnav: scenario A WorldHash matches the baked value (Debug == Release)");

    // ---- 到達不能は NoPath ----
    {
        NavSim sim;
        sim.Build(islandMap, 0.5f, 1.9f, 7.5f, 7.5f);
        const int tick = StepUntilStatus(sim, navagentstatus::kNoPath, 30);
        check(tick > 0, "gridnav: a goal on an unreachable island is NoPath");
        sim.Step(30);
        check(std::fabs(sim.Pose().position.x - 0.5f) < 0.05f, "gridnav: NoPath does not walk");
    }

    // ---- 展開上限: 広い面では NoPath にせず Moving のまま届く限り近づく ----
    {
        NavSim sim;
        sim.Build(wideMap, 0.5f, 1.9f, 2500.5f, 9.5f);
        sim.Step(1);
        check(sim.Agent().status == navagentstatus::kMoving,
              "gridnav: more than 4096 reachable cells do not decide NoPath (status Moving)");
        sim.Step(120);
        check(sim.Pose().position.x > 3.0f && sim.Agent().status != navagentstatus::kNoPath,
              "gridnav: the agent keeps approaching the island while the search is cut off");
    }

    // ---- Inactive: CC が無い / plane2D でない / Tilemap が無い ----
    {
        NavSim noCc;
        noCc.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f, /*withCc*/ false);
        noCc.Step(2);
        check(noCc.Agent().status == navagentstatus::kInactive, "gridnav: no CharacterController -> Inactive");
        NavSim flat;
        flat.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f, true, /*plane2D*/ false);
        flat.Step(2);
        check(flat.Agent().status == navagentstatus::kInactive, "gridnav: a 3D CharacterController -> Inactive");
        NavSim noMap;
        noMap.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f);
        noMap.Agent().tilemap = kNullEntity;
        noMap.Step(2);
        check(noMap.Agent().status == navagentstatus::kInactive, "gridnav: no Tilemap entity -> Inactive");
    }

    // ---- 2 ワールド並走: 毎 tick の HashWorld が一致 ----
    {
        NavSim a;
        NavSim b;
        a.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f);
        b.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f);
        bool same = true;
        for (int tick = 0; tick < 400 && same; ++tick) {
            a.Step(1);
            b.Step(1);
            same = a.Hash() == b.Hash();
            if (tick == 150) { // 途中で目的地を変えても並走は崩れない
                a.Agent().destination = { 0.5f, 1.5f, 0.0f };
                b.Agent().destination = { 0.5f, 1.5f, 0.0f };
            }
        }
        check(same, "gridnav: two worlds stay hash-identical every tick (incl. a mid-run goal change)");
        check(a.Pose().position.x < 4.0f, "gridnav: the agent turned back after the goal moved");
    }

    // ---- スナップショット往復: 途中で撮って別 sim へ戻しても続きが一致する ----
    {
        NavSim a;
        a.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f);
        a.Step(120);
        std::vector<std::byte> blob;
        const bool captured = CaptureSimSnapshot(a.Refs(), blob);
        a.Step(300);
        const uint64_t hashA = a.Hash();

        NavSim b;
        b.Build(stepMap, 0.5f, 1.9f, 9.5f, 3.5f);
        const bool restored = captured && RestoreSimSnapshot(b.Refs(), blob.data(), blob.size());
        b.Step(300);
        check(captured && restored, "gridnav: SimSnapshot capture / restore succeed");
        check(b.Hash() == hashA, "gridnav: after a snapshot restore the walk continues identically (state lives in the component)");
    }

    tilemap::Install(previousMaps);
    tilepalette::Install(previousPalettes);
    MYE_LOG_INFO("==== GridNav2D self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
