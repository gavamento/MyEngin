//====================================================================================
//                          FractureBenchmark.cpp
//  MyEngin/ 秋田蓮音                                                     09/26/2026
//                                          破壊物理のヘッドレス性能計測の実装
//====================================================================================
#include "Engine/Engine/Physics/FractureBenchmark.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/FractureBuilder.h"
#include "Engine/Engine/FractureSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/FractureBake.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Physics/FractureMesh.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"

using namespace DirectX;

namespace mye {
namespace {

// ---- 単位立方体 (半径 1、CCW 外向き)。FractureSelfTest.cpp の MakeBox と同じ構成
//      (非公開ヘルパはファイルごとに複製する既存の流儀、DemoSkinArm 等と同じ) ----
int32_t AddVert(FractureMesh& m, float x, float y, float z, float nx, float ny, float nz)
{
    FractureVertex v;
    v.position = { x, y, z };
    v.normal = { nx, ny, nz };
    v.uv = { 0.0f, 0.0f };
    m.verts.push_back(v);
    return static_cast<int32_t>(m.verts.size()) - 1;
}

void Tri(FractureMesh& m, int32_t a, int32_t b, int32_t c)
{
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
}

FractureMesh MakeBenchBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, -1, -1, -1), AddVert(m, hx, -hy, -hz, 1, -1, -1),
        AddVert(m, hx, hy, -hz, 1, 1, -1),     AddVert(m, -hx, hy, -hz, -1, 1, -1),
        AddVert(m, -hx, -hy, hz, -1, -1, 1),   AddVert(m, hx, -hy, hz, 1, -1, 1),
        AddVert(m, hx, hy, hz, 1, 1, 1),       AddVert(m, -hx, hy, hz, -1, 1, 1),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    quad(p[4], p[5], p[6], p[7]); // +Z
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 名前ごとの平均・最大 (ms)。prof::FrameScopes() の 1 tick 分を Record() で積む
struct ScopeStat {
    double sumMs = 0.0;
    double maxMs = 0.0;
    int64_t ticks = 0;

    void Add(double ms)
    {
        sumMs += ms;
        maxMs = (std::max)(maxMs, ms);
        ++ticks;
    }
};

struct PhaseStats {
    std::map<std::string, ScopeStat> byName;

    void Record()
    {
        for (const prof::ScopeRecord& r : prof::FrameScopes()) {
            byName[r.name].Add(static_cast<double>(r.ms));
        }
    }

    void Log(const char* label) const
    {
        for (const auto& kv : byName) {
            const ScopeStat& s = kv.second;
            const double avg = s.ticks > 0 ? s.sumMs / static_cast<double>(s.ticks) : 0.0;
            MYE_LOG_INFO("    [%-11s] %-16s avg %7.4f ms / max %7.4f ms (ticks=%lld)", label,
                        kv.first.c_str(), avg, s.maxMs, static_cast<long long>(s.ticks));
        }
    }
};

// --fracture-demo と同じ配置 (落下して転がる箱 + 水平発射の球) を objectCount 個ぶん
// X 方向に並べる。pieceCount 破片へ Voronoi 分割済みの bake を objectCount 体すべてで共有する
// (焼きの再現性はビルド時の 1 回で十分 — このシーンでは焼き時間ではなく物理側を測る)
void RunOneBench(const FractureBakeResult& bake, int32_t pieceCount, int32_t objectCount)
{
    RenderResources resources;
    ConvexColliderLibrary colliders;
    colliders.Init(&resources);
    FractureLibrary lib;
    lib.Init(&resources, &colliders);
    fracturelib::Install(&lib);
    convexcol::Install(&colliders);

    char prefix[128];
    std::snprintf(prefix, sizeof(prefix), "fracture-bench://n%d_c%d", pieceCount, objectCount);
    const FractureAssetHandle* handle
        = lib.RegisterBaked(prefix, bake, HashStr("fracture-bench://src"), 1, pieceCount, 0, 32);
    if (handle == nullptr) {
        MYE_LOG_ERROR("[fracture-bench] register failed for pieceCount=%d", pieceCount);
        fracturelib::Install(nullptr);
        convexcol::Install(nullptr);
        return;
    }

    Scene s;
    World& w = s.GetWorld();
    constexpr float kSpacing = 4.0f;

    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(static_cast<float>(objectCount - 1) * kSpacing * 0.5f, -0.5f, 0.0f);
    floor.SetLocalScale(static_cast<float>(objectCount) * kSpacing + 8.0f, 1.0f, 24.0f);
    auto* floorCol = floor.AddComponent<ColliderComponent>();
    floorCol->shape = collidershape::kBox;
    floorCol->halfExtents = { 0.5f, 0.5f, 0.5f };

    // ★DestructibleComponent* は保存しない — 次の反復の AddComponent がアーキタイプを
    //   動かすとダングリングになる (World の契約、TickRunner が毎 tick GetComponent で
    //   引き直しているのと同じ理由)。EntityID だけ保存して tick ごとに引き直す
    std::vector<EntityID> destructibleEntities;
    for (int32_t i = 0; i < objectCount; ++i) {
        const float x = static_cast<float>(i) * kSpacing;
        char name[32];
        std::snprintf(name, sizeof(name), "Box%d", i);
        GameObject box = s.CreateGameObject(name);
        box.SetLocalPosition(x, 4.0f, 0.0f);
        box.SetLocalRotationEuler(22.0f, 17.0f, 0.0f); // --fracture-demo と同じ転がる初期姿勢
        auto* rb = box.AddComponent<RigidbodyComponent>();
        rb->mass = 8.0f;
        auto* d = box.AddComponent<DestructibleComponent>();
        d->strength = 200.0f; // --fracture-demo と同じ理由 (実インパルスは子形状に分散する)
        d->fractureAsset = AssetID{ HashStr(prefix) };
        BuildFracturePieces(w, box.Id(), *handle);
        destructibleEntities.push_back(box.Id());

        char ballName[32];
        std::snprintf(ballName, sizeof(ballName), "Ball%d", i);
        GameObject ball = s.CreateGameObject(ballName);
        ball.SetLocalPosition(x, 0.55f, -20.0f);
        auto* bcol = ball.AddComponent<ColliderComponent>();
        bcol->shape = collidershape::kSphere;
        bcol->radius = 0.5f;
        auto* brb = ball.AddComponent<RigidbodyComponent>();
        brb->mass = 20.0f;
        brb->gravityScale = 0.0f;
        brb->velocity = { 0.0f, 0.0f, 30.0f };
    }
    w.ApplyStructuralChanges();

    PhysicsSystem phys;
    FractureSystem fsys;
    constexpr float kDt = 1.0f / 60.0f;
    constexpr int kTicks = 150;
    constexpr int kBreakWindowTicks = 10;

    PhaseStats before, atBreak, after;
    int32_t breakTick = -1;
    for (int t = 0; t < kTicks; ++t) {
        prof::BeginFrame();
        const bool anyD = AnyDestructibles(w);
        std::vector<ShapeImpulse> impulses;
        phys.Update(w, kDt, nullptr, nullptr, anyD ? &impulses : nullptr);
        if (anyD) {
            fsys.Update(w, kDt, impulses);
        }
        w.ApplyStructuralChanges();

        if (breakTick < 0) {
            for (const EntityID e : destructibleEntities) {
                const auto* d = w.GetComponent<DestructibleComponent>(e);
                if (d != nullptr && d->broken) {
                    breakTick = t;
                    break;
                }
            }
        }
        if (breakTick < 0) {
            before.Record();
        } else if (t < breakTick + kBreakWindowTicks) {
            atBreak.Record();
        } else {
            after.Record();
        }
    }

    MYE_LOG_INFO("[fracture-bench] pieceCount=%d objects=%d break-tick=%d", pieceCount, objectCount,
                breakTick);
    before.Log("before-break");
    atBreak.Log("at-break");
    after.Log("after-break");

    fracturelib::Install(nullptr);
    convexcol::Install(nullptr);
}

} // namespace

int RunFractureBenchmark()
{
    const int32_t kPieceCounts[] = { 16, 32, 64, 128, 256 };
    const int32_t kObjectCounts[] = { 1, 8 };

    MYE_LOG_INFO("[fracture-bench] ==== start ====");
    for (const int32_t pieceCount : kPieceCounts) {
        FractureBakeInput in;
        in.sourceMesh = MakeBenchBox(1.0f, 1.0f, 1.0f);
        in.seed = 1;
        in.pieceCount = pieceCount;
        FractureBakeResult bake;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = BakeFracture(in, bake);
        const auto t1 = std::chrono::steady_clock::now();
        const double bakeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (!ok || !bake.success) {
            MYE_LOG_ERROR("[fracture-bench] bake failed for pieceCount=%d: %s", pieceCount,
                         bake.failReason.c_str());
            continue;
        }
        MYE_LOG_INFO("[fracture-bench] bake pieceCount=%d actual=%zu pieces = %.3f ms", pieceCount,
                    bake.pieces.size(), bakeMs);
        for (const int32_t objectCount : kObjectCounts) {
            RunOneBench(bake, pieceCount, objectCount);
        }
    }
    MYE_LOG_INFO("[fracture-bench] ==== done ====");
    return 0;
}

} // namespace mye
