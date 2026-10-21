//====================================================================================
//                          Camera2DFollowSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          Camera2DFollow (2D カメラ追従) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Rendering/Camera2DFollowSelfTest.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/Camera2DFollow.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"

namespace mye {
namespace {

// Debug / Release で同じ値になること (M94v で採取)。動く対象を 300 tick 追わせた HashWorld
constexpr uint64_t kExpectedScenarioHash = 0xD5147EA06255DFC2ull;

constexpr uint32_t kTileFloor = 1;

TilePalette MakePalette()
{
    TilePalette p;
    p.Add("floor", AssetID{ 301 })->collider = 1;
    return p;
}

// x0..x1 x y0..y1 のセルを埋める
void FillRect(TilemapLayer& layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x) {
            layer.SetCell(x, y, tilecell::Make(kTileFloor, false, false, 0));
        }
    }
}

// 衝突レイヤー 1 枚 (x 0..9, y 0..2) と、遠くにセルがある衝突しないレイヤー
TilemapAsset MakeBoundsMap(uint64_t paletteHash, bool secondCollisionLayer)
{
    TilemapAsset m;
    m.palette = paletteHash;
    TilemapLayer ground;
    ground.name = "Ground";
    FillRect(ground, 0, 0, 9, 2);
    m.layers.push_back(std::move(ground));
    TilemapLayer fore;
    fore.name = "Fore";
    fore.collision = false;
    FillRect(fore, 50, 40, 50, 40);
    m.layers.push_back(std::move(fore));
    if (secondCollisionLayer) {
        TilemapLayer extra;
        extra.name = "Extra";
        FillRect(extra, 12, 5, 12, 5);
        m.layers.push_back(std::move(extra));
    }
    return m;
}

// カメラ 1 台と追従対象 1 体の世界。target / camera は LocalTransform を直接書く
struct FollowSim {
    Scene scene;
    TilemapSystem tiles;
    TransformSystem transforms;
    EntityID target = kNullEntity;
    EntityID camera = kNullEntity;

    FollowSim(const FollowSim&) = delete;
    FollowSim& operator=(const FollowSim&) = delete;
    FollowSim()
    {
        GameObject t = scene.CreateGameObjectTracked("Target");
        target = t.Id();
        GameObject c = scene.CreateGameObjectTracked("Camera");
        c.SetLocalPosition(0.0f, 0.0f, -10.0f);
        auto* cam = c.AddComponent<CameraComponent>();
        cam->orthographic = true;
        cam->orthoSize = 4.0f;
        auto* f = c.AddComponent<Camera2DFollowComponent>();
        f->target = target;
        f->damping = { 1.0f, 1.0f };
        camera = c.Id();
        GetWorld().ApplyStructuralChanges();
        transforms.Update(GetWorld());
    }

    World& GetWorld() { return scene.GetWorld(); }
    Camera2DFollowComponent& Follow() { return *GetWorld().GetComponent<Camera2DFollowComponent>(camera); }
    const LocalTransform& CamPose() { return *GetWorld().GetComponent<LocalTransform>(camera); }
    void SetTarget(float x, float y)
    {
        LocalTransform* lt = GetWorld().GetComponent<LocalTransform>(target);
        lt->position.x = x;
        lt->position.y = y;
    }
    void Step(int ticks = 1)
    {
        for (int i = 0; i < ticks; ++i) {
            UpdateCamera2DFollow(GetWorld(), &tiles);
            transforms.Update(GetWorld());
        }
    }
    SimRefs Refs()
    {
        SimRefs refs;
        refs.scene = &scene;
        refs.tilemap = &tiles;
        return refs;
    }
    uint64_t Hash() { return HashWorld(GetWorld(), Refs().HashSources()); }
};

// 対象の動き (整数だけの三角波)。float の三角関数を使わない
void MoveTarget(FollowSim& sim, int tick)
{
    const int phase = tick % 120;
    const int tri = phase < 60 ? phase : 120 - phase; // 0..60..0
    sim.SetTarget(static_cast<float>(tick) * 0.125f, static_cast<float>(tri) * 0.25f - 4.0f);
}

} // namespace

bool RunCamera2DFollowSelfTest()
{
    MYE_LOG_INFO("==== Camera2DFollow self test ====");
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
        check(Camera2DFollowComponent::sTypeId == 92, "Camera2DFollow is TypeId 92 (after GridAgent2D 91)");
        check(PixelPerfectCamera2DComponent::sTypeId == 93, "PixelPerfectCamera2D is TypeId 93");
        const ComponentDesc& follow = ComponentRegistry::Get().Desc(Camera2DFollowComponent::sTypeId);
        const ComponentDesc& pixel = ComponentRegistry::Get().Desc(PixelPerfectCamera2DComponent::sTypeId);
        check((follow.flags & kComponentNoHash) == 0, "Camera2DFollow is hashed (it decides the camera position)");
        check((pixel.flags & kComponentNoHash) != 0, "PixelPerfectCamera2D is NoHash (render only)");
    }

    // ---- デッドゾーン内は動かない / 外れると damping で追う ----
    {
        FollowSim sim;
        sim.Follow().deadZone = { 2.0f, 1.0f };
        sim.Follow().damping = { 0.5f, 0.5f };
        sim.SetTarget(1.5f, 0.5f);
        sim.Step(10);
        check(sim.CamPose().position.x == 0.0f && sim.CamPose().position.y == 0.0f,
              "follow: a target inside the dead zone does not move the camera (exactly)");
        sim.SetTarget(5.0f, 0.0f);
        sim.Step(1);
        const float x1 = sim.CamPose().position.x;
        sim.Step(1);
        const float x2 = sim.CamPose().position.x;
        sim.Step(1);
        const float x3 = sim.CamPose().position.x;
        sim.Step(20);
        MYE_LOG_INFO("  [follow] dead zone 2, damping 0.5, target x = 5: camera x per tick %.4f %.4f %.4f ... %.4f", static_cast<double>(x1),
                     static_cast<double>(x2), static_cast<double>(x3), static_cast<double>(sim.CamPose().position.x));
        check(x1 == 2.5f && x2 == 3.75f, "follow: leaving the dead zone closes half the gap per tick (2.5, 3.75)");
        check(x3 == 3.75f && sim.CamPose().position.x == 3.75f,
              "follow: once the target is back inside the dead zone (distance 1.25 <= 2) the camera stops at 3.75");
        check(sim.CamPose().position.y == 0.0f, "follow: the vertical dead zone (1) keeps y untouched");
    }

    // ---- damping の数値と軸ごとの独立 ----
    {
        FollowSim sim;
        sim.Follow().damping = { 0.25f, 1.0f };
        sim.SetTarget(8.0f, 3.0f);
        sim.Step(1);
        const float a = sim.CamPose().position.x;
        sim.Step(1);
        const float b = sim.CamPose().position.x;
        sim.Step(1);
        const float c = sim.CamPose().position.x;
        check(a == 2.0f && b == 3.5f && c == 4.625f, "follow: damping 0.25 covers a quarter of the remaining distance (2, 3.5, 4.625)");
        check(sim.CamPose().position.y == 3.0f, "follow: damping y = 1 snaps y on the first tick");
        sim.Step(80);
        check(std::fabs(sim.CamPose().position.x - 8.0f) < 1.0e-3f, "follow: x converges to the target (|error| < 1e-3 after 83 ticks)");
        check(sim.CamPose().position.z == -10.0f, "follow: z is never touched");
        sim.Follow().damping = { 5.0f, -1.0f };
        sim.SetTarget(20.0f, 20.0f);
        sim.Step(1);
        check(sim.CamPose().position.x == 20.0f && sim.CamPose().position.y == 3.0f,
              "follow: damping is clamped to 0..1 (5 -> snap, -1 -> no move)");
    }

    // ---- オフセットと先読み ----
    {
        FollowSim sim;
        sim.Follow().offset = { 1.0f, 2.0f };
        sim.SetTarget(3.0f, 4.0f);
        sim.Step(1);
        check(sim.CamPose().position.x == 4.0f && sim.CamPose().position.y == 6.0f, "follow: offset is added to the target position");

        FollowSim ahead;
        ahead.Follow().lookAheadX = 2.0f;
        auto* rb = ahead.GetWorld().AddComponent<RigidbodyComponent>(ahead.target);
        rb->velocity.x = 3.0f;
        ahead.Step(1);
        const float right = ahead.CamPose().position.x;
        ahead.GetWorld().GetComponent<RigidbodyComponent>(ahead.target)->velocity.x = -0.5f;
        ahead.Step(1);
        const float left = ahead.CamPose().position.x;
        ahead.GetWorld().GetComponent<RigidbodyComponent>(ahead.target)->velocity.x = 0.0f;
        ahead.Step(1);
        const float still = ahead.CamPose().position.x;
        check(right == 2.0f && left == -2.0f && still == 0.0f, "follow: lookAheadX is sign(Rigidbody.velocity.x) x distance (+2, -2, 0)");

        FollowSim cc;
        cc.Follow().lookAheadX = 1.5f;
        cc.GetWorld().AddComponent<CharacterControllerComponent>(cc.target)->velocity.x = 2.0f;
        cc.Step(1);
        check(cc.CamPose().position.x == 1.5f, "follow: without a Rigidbody the CharacterController velocity gives the look-ahead sign");
    }

    // ---- 親子の連鎖 (対象の位置は LocalTransform の連鎖で合成) ----
    {
        FollowSim sim;
        GameObject parent = sim.scene.CreateGameObjectTracked("Parent");
        parent.SetLocalPosition(10.0f, 5.0f, 0.0f);
        sim.GetWorld().ApplyStructuralChanges();
        GameObject child = sim.scene.CreateGameObjectTracked("Child");
        child.SetLocalPosition(1.0f, 2.0f, 0.0f);
        child.SetParent(parent);
        sim.GetWorld().ApplyStructuralChanges();
        sim.Follow().target = child.Id();
        // WorldMatrix は更新しない (前 tick の値が古くても連鎖から読む = ラグ無し)
        sim.Step(1);
        check(sim.CamPose().position.x == 11.0f && sim.CamPose().position.y == 7.0f,
              "follow: a child target is followed at the composed world position (11, 7)");
    }

    // ---- 対象なし / 自分自身 / 死んだ対象は何もしない ----
    {
        FollowSim sim;
        sim.SetTarget(5.0f, 5.0f);
        sim.Follow().target = kNullEntity;
        sim.Step(3);
        check(sim.CamPose().position.x == 0.0f, "follow: no target -> the camera stays");
        sim.Follow().target = sim.camera;
        sim.Step(3);
        check(sim.CamPose().position.x == 0.0f, "follow: following itself does nothing");
        sim.Follow().target = sim.target;
        sim.GetWorld().DestroyEntity(sim.target);
        sim.GetWorld().ApplyStructuralChanges();
        sim.Step(3);
        check(sim.CamPose().position.x == 0.0f, "follow: a destroyed target does nothing");
    }

    // ---- 手書きの境界 ----
    {
        FollowSim sim; // orthoSize 4、boundsAspect 2 -> 半幅 8 / 半高 4
        sim.Follow().useBounds = true;
        sim.Follow().boundsMin = { -10.0f, -10.0f };
        sim.Follow().boundsMax = { 10.0f, 10.0f };
        sim.Follow().boundsAspect = 2.0f;
        sim.SetTarget(50.0f, 50.0f);
        sim.Step(1);
        MYE_LOG_INFO("  [follow] bounds +-10, view half (8, 4): camera (%.3f, %.3f)", static_cast<double>(sim.CamPose().position.x),
                     static_cast<double>(sim.CamPose().position.y));
        check(sim.CamPose().position.x == 2.0f && sim.CamPose().position.y == 6.0f,
              "follow: bounds clamp the view edge, not the camera center (x <= 10 - 8, y <= 10 - 4)");
        sim.SetTarget(-50.0f, -50.0f);
        sim.Step(1);
        check(sim.CamPose().position.x == -2.0f && sim.CamPose().position.y == -6.0f, "follow: the lower bounds clamp the same way");
        sim.Follow().boundsMin = { -3.0f, -2.0f };
        sim.Follow().boundsMax = { 3.0f, 2.0f };
        sim.Step(1);
        check(sim.CamPose().position.x == 0.0f && sim.CamPose().position.y == 0.0f,
              "follow: bounds smaller than the view center the camera");
        sim.Follow().useBounds = false;
        sim.Step(1);
        check(sim.CamPose().position.x == -50.0f, "follow: useBounds off ignores the bounds");
    }

    // ---- Tilemap の衝突レイヤーの境界 ----
    {
        TilePaletteLibrary palettes;
        TilemapLibrary maps;
        TilePaletteLibrary* previousPalettes = tilepalette::Library();
        TilemapLibrary* previousMaps = tilemap::Library();
        tilepalette::Install(&palettes);
        tilemap::Install(&maps);
        const uint64_t paletteHash = palettes.Register(L"C:\\sel\\camfollow\\cam.tilepalette.json", MakePalette());
        const AssetID oneLayer{ maps.Register(L"C:\\sel\\camfollow\\one.tilemap.json", MakeBoundsMap(paletteHash, false)) };
        const AssetID twoLayers{ maps.Register(L"C:\\sel\\camfollow\\two.tilemap.json", MakeBoundsMap(paletteHash, true)) };
        check(paletteHash != 0 && oneLayer.value != 0 && twoLayers.value != 0, "follow: tilemap test assets registered");

        const auto runWith = [&](AssetID mapId, float originX, float originY, float& outX, float& outY) {
            FollowSim sim;
            sim.Follow().useBounds = true;
            sim.Follow().boundsAspect = 1.0f;
            sim.GetWorld().GetComponent<CameraComponent>(sim.camera)->orthoSize = 1.0f;
            GameObject map = sim.scene.CreateGameObjectTracked("Map");
            map.SetLocalPosition(originX, originY, 0.0f);
            map.AddComponent<TilemapComponent>()->tilemap = mapId;
            sim.GetWorld().ApplyStructuralChanges();
            sim.transforms.Update(sim.GetWorld());
            sim.Follow().boundsFromTilemap = map.Id();
            sim.SetTarget(1000.0f, 1000.0f);
            sim.Step(1);
            outX = sim.CamPose().position.x;
            outY = sim.CamPose().position.y;
        };
        float x = 0.0f, y = 0.0f;
        runWith(oneLayer, 0.0f, 0.0f, x, y);
        MYE_LOG_INFO("  [follow] tilemap bounds [0,10]x[0,3], view half 1: camera (%.3f, %.3f)", static_cast<double>(x), static_cast<double>(y));
        check(x == 9.0f && y == 2.0f, "follow: boundsFromTilemap is the box around the collision layer (the non-collision layer far away is ignored)");
        runWith(twoLayers, 0.0f, 0.0f, x, y);
        check(x == 12.0f && y == 5.0f, "follow: two collision layers give the union of their bounds");
        runWith(oneLayer, 100.0f, 50.0f, x, y);
        check(x == 109.0f && y == 52.0f, "follow: the Tilemap entity's position offsets the bounds");

        // 空の Tilemap エンティティ (null) なら手書きの値に戻る
        FollowSim fallback;
        fallback.Follow().useBounds = true;
        fallback.Follow().boundsAspect = 1.0f;
        fallback.Follow().boundsMin = { 0.0f, 0.0f };
        fallback.Follow().boundsMax = { 20.0f, 20.0f };
        fallback.GetWorld().GetComponent<CameraComponent>(fallback.camera)->orthoSize = 1.0f;
        fallback.SetTarget(1000.0f, 1000.0f);
        fallback.Step(1);
        check(fallback.CamPose().position.x == 19.0f && fallback.CamPose().position.y == 19.0f,
              "follow: without a boundsFromTilemap the hand-written box is used");

        // GetCollisionBounds 単体: 衝突レイヤーの和をセル座標で返す
        {
            FollowSim sim;
            GameObject map = sim.scene.CreateGameObjectTracked("Map");
            map.AddComponent<TilemapComponent>()->tilemap = twoLayers;
            sim.GetWorld().ApplyStructuralChanges();
            int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            const bool ok = sim.tiles.GetCollisionBounds(sim.GetWorld(), map.Id(), x0, y0, x1, y1);
            check(ok && x0 == 0 && y0 == 0 && x1 == 12 && y1 == 5, "tilemap: GetCollisionBounds is the union of the collision layers (0,0)-(12,5)");
        }

        tilemap::Install(previousMaps);
        tilepalette::Install(previousPalettes);
    }

    // ---- 2 ワールド並走: 毎 tick の HashWorld が一致し、焼いた値と合う ----
    {
        FollowSim a;
        FollowSim b;
        for (FollowSim* s : { &a, &b }) {
            s->Follow().deadZone = { 1.0f, 0.5f };
            s->Follow().damping = { 0.2f, 0.1f };
            s->Follow().lookAheadX = 1.0f;
            s->Follow().offset = { 0.5f, 0.25f };
            s->Follow().useBounds = true;
            s->Follow().boundsMin = { -20.0f, -20.0f };
            s->Follow().boundsMax = { 20.0f, 20.0f };
            auto* rb = s->GetWorld().AddComponent<RigidbodyComponent>(s->target);
            rb->velocity.x = 1.0f;
        }
        bool same = true;
        for (int tick = 0; tick < 300 && same; ++tick) {
            MoveTarget(a, tick);
            MoveTarget(b, tick);
            a.Step(1);
            b.Step(1);
            same = a.Hash() == b.Hash();
        }
        check(same, "follow: two worlds stay hash-identical every tick");
        const uint64_t scenarioHash = a.Hash();
        MYE_LOG_INFO("  [follow] scenario hash 0x%016llX, camera (%.4f, %.4f)", static_cast<unsigned long long>(scenarioHash),
                     static_cast<double>(a.CamPose().position.x), static_cast<double>(a.CamPose().position.y));
        check(kExpectedScenarioHash == 0 || scenarioHash == kExpectedScenarioHash,
              "follow: scenario WorldHash matches the baked value (Debug == Release)");

        // スナップショット往復: カメラの位置は LocalTransform にあるので、途中で戻しても続きが一致する
        std::vector<std::byte> blob;
        FollowSim c;
        c.Follow() = a.Follow();
        c.GetWorld().AddComponent<RigidbodyComponent>(c.target)->velocity.x = 1.0f;
        for (int tick = 0; tick < 100; ++tick) {
            MoveTarget(c, tick);
            c.Step(1);
        }
        const bool captured = CaptureSimSnapshot(c.Refs(), blob);
        for (int tick = 100; tick < 200; ++tick) {
            MoveTarget(c, tick);
            c.Step(1);
        }
        FollowSim d;
        d.Follow() = a.Follow();
        d.GetWorld().AddComponent<RigidbodyComponent>(d.target)->velocity.x = 1.0f;
        const bool restored = captured && RestoreSimSnapshot(d.Refs(), blob.data(), blob.size());
        for (int tick = 100; tick < 200; ++tick) {
            MoveTarget(d, tick);
            d.Step(1);
        }
        check(captured && restored, "follow: SimSnapshot capture / restore succeed");
        check(c.Hash() == d.Hash(), "follow: after a snapshot restore the camera continues identically");
    }

    MYE_LOG_INFO("==== Camera2DFollow self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
