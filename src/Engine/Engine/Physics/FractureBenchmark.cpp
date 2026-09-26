//====================================================================================
//                          FractureBenchmark.cpp
//  MyEngin/ 秋田蓮音                                                     09/26/2026
//                                          破壊物理のヘッドレス性能計測の実装
//====================================================================================
#include "Engine/Engine/Physics/FractureBenchmark.h"

#include <chrono>
#include <cmath>
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

// 蓋のない箱 (+Z 面を欠く)。ボクセル化 (openMeshMode=1) の焼き時間計測用
// (FractureSelfTest.cpp の MakeOpenBox と同じ構成。非公開ヘルパはファイルごとに複製する
// 既存の流儀)
FractureMesh MakeBenchOpenBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, 0, 0, 0), AddVert(m, hx, -hy, -hz, 0, 0, 0),
        AddVert(m, hx, hy, -hz, 0, 0, 0),   AddVert(m, -hx, hy, -hz, 0, 0, 0),
        AddVert(m, -hx, -hy, hz, 0, 0, 0),  AddVert(m, hx, -hy, hz, 0, 0, 0),
        AddVert(m, hx, hy, hz, 0, 0, 0),    AddVert(m, -hx, hy, hz, 0, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    // +Z を作らない (穴)
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 1 枚の平面 (quad)。ボクセル化の焼き時間計測用 (FractureSelfTest.cpp の MakePlaneQuad と同じ)
FractureMesh MakeBenchPlaneQuad(float half)
{
    FractureMesh m;
    const int32_t a = AddVert(m, -half, 0, -half, 0, 1, 0);
    const int32_t b = AddVert(m, half, 0, -half, 0, 1, 0);
    const int32_t c = AddVert(m, half, 0, half, 0, 1, 0);
    const int32_t d = AddVert(m, -half, 0, half, 0, 1, 0);
    Tri(m, a, b, c);
    Tri(m, a, c, d);
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
        fsys.ApplyDeferredLocals(w);

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

// ---- strength の既定値決定 (M80p) ----
// 質量 1kg・破片 16・一辺 1m の箱を dropHeight [m] から重力落下させ、ticks tick 動かして
// 割れたかを返す。dropHeight==0 は「床に置いたまま」(既定の質量 1kg・一辺 1m の Destructible
// で 3 基準を測る、spec §2)
bool RunStrengthDropScenario(const FractureBakeResult& bake, float strength, float dropHeight, int ticks)
{
    RenderResources resources;
    ConvexColliderLibrary colliders;
    colliders.Init(&resources);
    FractureLibrary lib;
    lib.Init(&resources, &colliders);
    fracturelib::Install(&lib);
    convexcol::Install(&colliders);
    const FractureAssetHandle* handle = lib.RegisterBaked(
        "fracture-bench://calib", bake, HashStr("fracture-bench://calib_src"), 1, 16, 0, 32);

    Scene s;
    World& w = s.GetWorld();
    // --fracture-demo と同じく PhysicsEnvironment を置く (置かないと substeps=1 固定・
    // スリープ無効のまま — 存在ゲートの規約、Components.h の PhysicsEnvironmentComponent
    // 参照。置かない計測は「起こりえない設定」を測ることになり基準にならない)
    s.CreateGameObject("Environment").AddComponent<PhysicsEnvironmentComponent>();
    GameObject floor = s.CreateGameObject("Floor");
    floor.SetLocalPosition(0.0f, -0.5f, 0.0f);
    floor.SetLocalScale(24.0f, 1.0f, 24.0f);
    auto* floorCol = floor.AddComponent<ColliderComponent>();
    floorCol->shape = collidershape::kBox;
    floorCol->halfExtents = { 0.5f, 0.5f, 0.5f };

    GameObject box = s.CreateGameObject("CalibBox");
    box.SetLocalPosition(0.0f, dropHeight + 0.5f, 0.0f); // 箱の底面が床から dropHeight だけ上
    auto* rb = box.AddComponent<RigidbodyComponent>();
    rb->mass = 1.0f;
    auto* d = box.AddComponent<DestructibleComponent>();
    d->strength = strength;
    d->fractureAsset = AssetID{ HashStr("fracture-bench://calib") };
    if (handle != nullptr) {
        BuildFracturePieces(w, box.Id(), *handle);
    }
    w.ApplyStructuralChanges();

    PhysicsSystem phys;
    FractureSystem fsys;
    constexpr float kDt = 1.0f / 60.0f;
    bool broken = false;
    for (int t = 0; t < ticks && !broken; ++t) {
        std::vector<ShapeImpulse> impulses;
        phys.Update(w, kDt, nullptr, nullptr, &impulses);
        fsys.Update(w, kDt, impulses);
        w.ApplyStructuralChanges();
        fsys.ApplyDeferredLocals(w);
        const auto* dd = w.GetComponent<DestructibleComponent>(box.Id());
        broken = dd != nullptr && dd->broken;
    }

    fracturelib::Install(nullptr);
    convexcol::Install(nullptr);
    return broken;
}

// [lo,hi] の間で pred が非増加 (pred(lo)==true, pred(hi)==false) と仮定し、対数二分探索で
// 「pred が true であり続ける最大の S」を返す (iters 回で相対誤差 2^-iters まで狭める)
template <typename Pred>
float BisectUpperBound(float lo, float hi, int iters, Pred pred)
{
    for (int i = 0; i < iters; ++i) {
        const float mid = std::sqrt(lo * hi);
        if (pred(mid)) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

// [lo,hi] の間で pred が非減少 (pred(lo)==false, pred(hi)==true) と仮定し、対数二分探索で
// 「pred が true になる最小の S」を返す
template <typename Pred>
float BisectLowerBound(float lo, float hi, int iters, Pred pred)
{
    for (int i = 0; i < iters; ++i) {
        const float mid = std::sqrt(lo * hi);
        if (pred(mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

// DestructibleComponent.strength の既定値を実測で決める。基準は spec §2 の 3 つ
// (質量 1kg・破片 16・一辺 1m の箱が、3m 落下で割れる/1m 落下で割れない/床に 600 tick 置いても
// 割れない)。対数二分探索で各基準の境界を求め、重なる範囲があればその対数中央値を返す。
// 合否判定はしない — 呼び出し側 (SelfTest) が採用した既定値で 3 基準を再検算する
void RunFractureStrengthCalibration()
{
    FractureBakeInput in;
    in.sourceMesh = MakeBenchBox(0.5f, 0.5f, 0.5f); // 一辺 1m (半径 0.5)
    in.seed = 1;
    in.pieceCount = 16;
    FractureBakeResult bake;
    if (!BakeFracture(in, bake) || !bake.success) {
        MYE_LOG_ERROR("[fracture-bench] calibration bake failed: %s", bake.failReason.c_str());
        return;
    }

    constexpr float kLo = 1.0f;
    constexpr float kHi = 1.0e7f;
    constexpr int kIters = 24; // 相対誤差 2^-24 ≈ 6e-8 まで (対数スケールなので十分細かい)

    // (a) 3m 落下は割れる: 割れ続ける最大の S。kLo でも割れないなら基準 (a) を満たす S が
    // 無い (-1)。kHi でも割れるなら探索範囲内では上限なし (kHi をそのまま返す)
    const bool breaksAtLo3m = RunStrengthDropScenario(bake, kLo, 3.0f, 180);
    const bool breaksAtHi3m = RunStrengthDropScenario(bake, kHi, 3.0f, 180);
    MYE_LOG_INFO("[fracture-bench] calibration: 3m drop breaks at S=%.0f -> %d, S=%.0f -> %d", kLo,
                breaksAtLo3m ? 1 : 0, kHi, breaksAtHi3m ? 1 : 0);
    const float sAMax = !breaksAtLo3m ? -1.0f
        : breaksAtHi3m ? kHi
        : BisectUpperBound(kLo, kHi, kIters,
                            [&](float S) { return RunStrengthDropScenario(bake, S, 3.0f, 180); });

    // (c) 1m 落下では割れない: 割れなくなる最小の S。kLo で既に割れないなら基準 (c) は
    // 下限なしで満たされる (0)。kHi でも割れるなら基準 (c) を満たす S が無い (-1)
    const bool breaksAtLo1m = RunStrengthDropScenario(bake, kLo, 1.0f, 150);
    const bool breaksAtHi1m = RunStrengthDropScenario(bake, kHi, 1.0f, 150);
    MYE_LOG_INFO("[fracture-bench] calibration: 1m drop breaks at S=%.0f -> %d, S=%.0f -> %d", kLo,
                breaksAtLo1m ? 1 : 0, kHi, breaksAtHi1m ? 1 : 0);
    const float sCMin = !breaksAtLo1m ? 0.0f
        : breaksAtHi1m ? -1.0f
        : BisectLowerBound(kLo, kHi, kIters,
                            [&](float S) { return !RunStrengthDropScenario(bake, S, 1.0f, 150); });

    // (b) 床に 600 tick 置いても割れない: (c) と同じ形の境界
    const bool breaksAtLo0m = RunStrengthDropScenario(bake, kLo, 0.0f, 600);
    const bool breaksAtHi0m = RunStrengthDropScenario(bake, kHi, 0.0f, 600);
    MYE_LOG_INFO("[fracture-bench] calibration: resting 600 ticks breaks at S=%.0f -> %d, S=%.0f -> %d",
                kLo, breaksAtLo0m ? 1 : 0, kHi, breaksAtHi0m ? 1 : 0);
    const float sBMin = !breaksAtLo0m ? 0.0f
        : breaksAtHi0m ? -1.0f
        : BisectLowerBound(kLo, kHi, kIters,
                            [&](float S) { return !RunStrengthDropScenario(bake, S, 0.0f, 600); });

    MYE_LOG_INFO("[fracture-bench] calibration: sAMax=%.1f sCMin=%.1f sBMin=%.1f", sAMax, sCMin, sBMin);
    const float sLoWindow = (std::max)(sCMin, sBMin);
    if (sAMax >= 0.0f && sCMin >= 0.0f && sBMin >= 0.0f && sLoWindow < sAMax) {
        const float recommended = std::sqrt((std::max)(sLoWindow, 1.0f) * sAMax);
        MYE_LOG_INFO("[fracture-bench] calibration: valid range [%.1f, %.1f], recommended (log-mid) = %.1f",
                    sLoWindow, sAMax, recommended);
    } else {
        MYE_LOG_ERROR("[fracture-bench] calibration: no S satisfies all 3 criteria "
                     "(sAMax=%.1f, sCMin=%.1f, sBMin=%.1f)",
                     sAMax, sCMin, sBMin);
    }
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

    // ---- ボクセル化 (開いたメッシュ) の焼き時間: 解像度 32/48/64 × 開いた箱/平面。
    //      voxelResolution の既定値・推奨解像度を決めるための表 (sub-04/sub-14 の実測の
    //      再現・更新用)。Debug --selftest には正しさの被覆だけを残し (pieceCount を
    //      落として軽量化)、この表 (pieceCount=16 の実測値) は --fracture-bench (Release)
    //      に一本化した (sub-11/sub-12)。合否判定はしない (計測専用) ----
    {
        auto bakeOpenMesh = [](const char* label, const FractureMesh& mesh, uint32_t seed,
                              int32_t pieceCount, int32_t resolution) {
            FractureBakeInput in;
            in.sourceMesh = mesh;
            in.seed = seed;
            in.pieceCount = pieceCount;
            in.openMeshMode = 1;
            in.voxelResolution = resolution;
            FractureBakeResult r;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = BakeFracture(in, r);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            MYE_LOG_INFO(
                "[fracture-bench] voxelize bake: %s seed=%u pieceCount=%d res=%d ok=%d pieces=%d = %.2f ms",
                label, seed, pieceCount, resolution, ok && r.success ? 1 : 0,
                ok && r.success ? static_cast<int>(r.pieces.size()) : -1, ms);
            if (!ok || !r.success) {
                MYE_LOG_ERROR("[fracture-bench] voxelize bake: %s res=%d fail reason: %s", label,
                             resolution, r.failReason.c_str());
            }
        };
        for (const int32_t res : { 32, 48, 64 }) {
            bakeOpenMesh("open box", MakeBenchOpenBox(1.0f, 1.0f, 1.0f), 11, 16, res);
            bakeOpenMesh("plane quad", MakeBenchPlaneQuad(1.0f), 11, 16, res);
        }
        // 一辺 1m の開いた箱・16 破片・seed=1 で解像度ごとの成否を記録する (bench.md 参照)
        for (const int32_t res : { 64, 72, 80, 96, 128 }) {
            bakeOpenMesh("open box 1m", MakeBenchOpenBox(0.5f, 0.5f, 0.5f), 1, 16, res);
        }
    }

    RunFractureStrengthCalibration();

    MYE_LOG_INFO("[fracture-bench] ==== done ====");
    return 0;
}

} // namespace mye
