//====================================================================================
//                          DisturbanceFieldSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Disturbance/DisturbanceFieldSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Disturbance/DisturbanceRender.h"
#include "Engine/Engine/Disturbance/DisturbanceState.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Shared/EngineAPI.h"
#include "Engine/Renderer/Passes/DisturbanceFieldPass.h"
#include "Engine/Renderer/Passes/DisturbanceVolumePass.h"
#include "Engine/Renderer/Pipeline/RenderTypes.h"

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
           && a.shape == b.shape && a.cellsY == b.cellsY && SameBits(a.flowY, b.flowY)
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

// 2 ワールドを同じ場面・同じ入力で ticks 並走し、毎 tick のハッシュが一致したかを返す。finalHash は最後の tick のハッシュ
bool RunTwoWorlds(int ticks, uint64_t& finalHash)
{
    auto a = std::make_unique<Sim>();
    auto b = std::make_unique<Sim>();
    const Scenario sa = BuildScenario(*a);
    const Scenario sb = BuildScenario(*b);
    a->Flush();
    b->Flush();
    bool same = true;
    finalHash = 0;
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
    return same;
}

// ---- 流れのソルバの検査用 ----

float FlowXAt(const DisturbanceSlot& s, int x, int z)
{
    return (x < 0 || x >= s.cellsX || z < 0 || z >= s.cellsZ) ? 0.0f : s.flowX[static_cast<size_t>(x + s.cellsX * z)];
}

float FlowZAt(const DisturbanceSlot& s, int x, int z)
{
    return (x < 0 || x >= s.cellsX || z < 0 || z >= s.cellsZ) ? 0.0f : s.flowZ[static_cast<size_t>(x + s.cellsX * z)];
}

// 中心差分の発散 (格子外の流れは 0 として扱う。風の無い場面用)。単位は [m/s] (マスの大きさを掛けた形)
float DivergenceAt(const DisturbanceSlot& s, int x, int z)
{
    return ((FlowXAt(s, x + 1, z) - FlowXAt(s, x - 1, z)) + (FlowZAt(s, x, z + 1) - FlowZAt(s, x, z - 1))) * 0.5f;
}

float MaxDivergence(const DisturbanceSlot& s)
{
    float m = 0.0f;
    for (int z = 0; z < s.cellsZ; ++z) {
        for (int x = 0; x < s.cellsX; ++x) {
            m = std::max(m, std::fabs(DivergenceAt(s, x, z)));
        }
    }
    return m;
}

// 中心差分の回転 ∂vz/∂x − ∂vx/∂z (マスの大きさを掛けた形)
float CurlAt(const DisturbanceSlot& s, int x, int z)
{
    return ((FlowZAt(s, x + 1, z) - FlowZAt(s, x - 1, z)) - (FlowXAt(s, x, z + 1) - FlowXAt(s, x, z - 1))) * 0.5f;
}

// ワールド座標 w のマス番号 (原点セルを引いた x / z)
int CellIndexOf(const DisturbanceSlot& s, float world, int axis)
{
    return static_cast<int>(std::floor(world / s.cellSize)) - s.originCell[axis];
}

// 濃さの重心の x [m] (濃さの総和が 0 なら 0)
float DensityCentroidX(const DisturbanceSlot& s)
{
    double mass = 0.0;
    double moment = 0.0;
    for (int z = 0; z < s.cellsZ; ++z) {
        for (int x = 0; x < s.cellsX; ++x) {
            const double rho = static_cast<double>(s.density[static_cast<size_t>(x + s.cellsX * z)]);
            mass += rho;
            moment += rho * (static_cast<double>(s.originCell[0] + x) + 0.5) * static_cast<double>(s.cellSize);
        }
    }
    return mass > 0.0 ? static_cast<float>(moment / mass) : 0.0f;
}

// 動く源 (位置の差分) を z 方向へ歩かせて ticks 進める。tick ごとに z を 0.05 m (= 3 m/s) 進める。
// 戻り値は最後の tick の源の z
float WalkSource(Sim& sim, EntityID source, float startZ, int ticks)
{
    float z = startZ;
    for (int t = 0; t < ticks; ++t) {
        z = startZ + 0.05f * static_cast<float>(t);
        TransformOf(sim, source)->position.z = z;
        sim.Step();
    }
    return z;
}

// 風の中の濃さの塊の重心の移動量 [m]。restDensity 0 の場の風上寄りへ濃さを足し、ticks 進める
struct WindRun {
    float shift = 0.0f;
    float maxFlowX = 0.0f;
    bool rest = false; // 塊を足さずに進めたとき、全マスが静止状態のままか
};

WindRun RunWind(float windScale, int ticks, bool withBlob)
{
    auto sim = std::make_unique<Sim>();
    GameObject envGo = sim->scene.CreateGameObjectTracked("Environment");
    envGo.AddComponent<PhysicsEnvironmentComponent>()->windVelocity = { 2.0f, 0.0f, 0.0f };
    // 128 x 32 マス x 0.25 m = 32 m x 8 m。塊は風上寄り (x = -10 m) に置く
    const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 128, 32, kDisturbanceChannelDensity);
    sim->Flush();
    auto* field = FieldOf(*sim, f);
    field->restDensity = 0.0f;
    field->densityKeep = 1.0f;
    field->windScale = windScale;
    sim->Step();
    WindRun r;
    if (withBlob) {
        FieldImpulse blob = MakeImpulse(-10.0f, 0.2f, 0.0f, 1.5f, 0.0f, 0.0f, 0.0f);
        blob.density = 0.9f;
        DisturbanceQueueImpulse(sim->scene, blob);
    }
    sim->Step();
    const float before = DensityCentroidX(sim->D().Slots()[0]);
    sim->Step(ticks);
    const DisturbanceSlot& s = sim->D().Slots()[0];
    r.shift = withBlob ? DensityCentroidX(s) - before : 0.0f;
    for (float v : s.flowX) {
        r.maxFlowX = std::max(r.maxFlowX, std::fabs(v));
    }
    r.rest = IsRestSlot(s, 0.0f);
    return r;
}

// 流れのソルバの断言 (acc 3 の再表明は decay の項、ここは acc 13 / 16)。--field-bench も同じものを走らせて数値を出す
void SolverChecks(const std::function<void(bool, const char*)>& check)
{
    // ---- 投影: 源が通った直後の発散が 10% 以下 / 源の左右で回転の符号が逆 (acc 13) ----
    {
        // 1 tick 目で prevPos を書き、2 tick 目で 3 m/s の速さの差分が出る。投影の有無だけが違う 2 本を並べる
        const auto divergence = [&](int iterations, float& flowPeak) {
            auto sim = std::make_unique<Sim>();
            const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 64, 64, kDisturbanceChannelDensity);
            const EntityID src = AddSource(*sim, "Walker", 0.0f, 0.0f, -0.05f);
            sim->Flush();
            FieldOf(*sim, f)->pressureIterations = iterations;
            SourceOf(*sim, src)->radius = 1.5f;
            sim->Step();
            TransformOf(*sim, src)->position.z = 0.0f;
            sim->Step();
            flowPeak = MaxFlow(sim->D().Slots()[0]);
            return MaxDivergence(sim->D().Slots()[0]);
        };
        float peakBefore = 0.0f, peakAfter = 0.0f;
        const float before = divergence(0, peakBefore);
        const float after = divergence(20, peakAfter);
        MYE_LOG_INFO("  [disturbance] divergence just after a source passes: %.4g before projection, %.4g after 20 iterations "
                     "(ratio %.3f); peak flow %.3g -> %.3g",
                     static_cast<double>(before), static_cast<double>(after), static_cast<double>(before > 0.0f ? after / before : 0.0f),
                     static_cast<double>(peakBefore), static_cast<double>(peakAfter));
        for (const int iterations : { 10, 40, 64 }) {
            float peak = 0.0f;
            const float d = divergence(iterations, peak);
            MYE_LOG_INFO("  [disturbance] divergence after %d iterations: %.4g (ratio %.3f)", iterations, static_cast<double>(d),
                         static_cast<double>(before > 0.0f ? d / before : 0.0f));
        }
        check(before > 0.05f && after <= 0.1f * before, "solver: 20 Jacobi iterations cut the max divergence to 10% or less");
        check(peakAfter > 0.2f * peakBefore, "solver: the projection keeps the stirred flow (it does not erase it)");
    }
    {
        // 放射状のインパルス (爆発) は発散源: その step の投影で消されず、後の step の投影と減衰で消える
        struct RadialRun {
            float peakFirst = 0.0f;
            float peakLater = 0.0f;
            bool outward = false;
        };
        const auto radialRun = [&](int iterations) {
            auto sim = std::make_unique<Sim>();
            const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 64, 64, kDisturbanceChannelDensity);
            sim->Flush();
            FieldOf(*sim, f)->pressureIterations = iterations;
            sim->Step();
            FieldImpulse radial = MakeImpulse(0.0f, 0.2f, 0.0f, 1.5f, 0.0f, 0.0f, 3.0f);
            radial.shape = kFieldImpulseRadial;
            DisturbanceQueueImpulse(sim->scene, radial);
            sim->Step();
            RadialRun r;
            const DisturbanceSlot& s0 = sim->D().Slots()[0];
            r.peakFirst = MaxFlow(s0);
            // 中心 (マス 32, 32 の角) の右 0.5 m は +x、左 0.5 m は -x、上 0.5 m は +z
            const int cx = 32, cz = 32;
            r.outward = FlowXAt(s0, cx + 2, cz) > 0.0f && FlowXAt(s0, cx - 3, cz) < 0.0f && FlowZAt(s0, cx, cz + 2) > 0.0f
                        && FlowZAt(s0, cx, cz - 3) < 0.0f;
            sim->Step(30);
            r.peakLater = MaxFlow(sim->D().Slots()[0]);
            return r;
        };
        const RadialRun off = radialRun(0);
        const RadialRun on = radialRun(20);
        MYE_LOG_INFO("  [disturbance] radial impulse: peak flow %.3g (0 iterations) / %.3g (20); 30 ticks later %.3g / %.3g",
                     static_cast<double>(off.peakFirst), static_cast<double>(on.peakFirst), static_cast<double>(off.peakLater),
                     static_cast<double>(on.peakLater));
        check(on.peakFirst >= 0.8f * off.peakFirst && on.outward, "solver: a radial impulse keeps its outward flow through the projection of its own step");
        check(on.peakLater < 0.5f * on.peakFirst, "solver: the radial flow dies out in the following ticks (projection and decay)");
    }
    {
        auto sim = std::make_unique<Sim>();
        const EntityID f = AddField(*sim, 0.0f, 0.0f, 0.0f, 64, 64, kDisturbanceChannelDensity);
        const EntityID src = AddSource(*sim, "Walker", 0.0f, 0.0f, -4.0f);
        sim->Flush();
        (void)f;
        SourceOf(*sim, src)->radius = 1.5f;
        const float z = WalkSource(*sim, src, -4.0f, 40);
        const DisturbanceSlot& s = sim->D().Slots()[0];
        // 経路 (x = 0) の左右 0.75 m、源の少し後ろ
        const int zi = CellIndexOf(s, z - 0.75f, 2);
        const float curlLeft = CurlAt(s, CellIndexOf(s, -0.75f, 0), zi);
        const float curlRight = CurlAt(s, CellIndexOf(s, 0.75f, 0), zi);
        MYE_LOG_INFO("  [disturbance] curl beside the path: left %.4g, right %.4g", static_cast<double>(curlLeft),
                     static_cast<double>(curlRight));
        check(curlLeft * curlRight < 0.0f && std::fabs(curlLeft) > 0.02f && std::fabs(curlRight) > 0.02f,
              "solver: the sign of the curl is opposite on the two sides of a moving source");
    }

    // ---- 風 (acc 16) ----
    {
        constexpr int kTicks = 600;
        // 流れは目標速度 w へ velocityKeep で寄る: v(t) = w (1 - keep^t)。重心の移動量の理論値は w * (T - sum keep^t) / 60
        double rampSum = 0.0;
        double keepPow = 1.0;
        for (int t = 1; t <= kTicks; ++t) {
            keepPow *= 0.95;
            rampSum += keepPow;
        }
        const float expected = static_cast<float>(2.0 * (static_cast<double>(kTicks) - rampSum) / 60.0);
        const WindRun full = RunWind(1.0f, kTicks, true);
        const WindRun off = RunWind(0.0f, kTicks, true);
        const WindRun half = RunWind(0.5f, kTicks, true);
        MYE_LOG_INFO("  [disturbance] wind (2, 0, 0) m/s over %d ticks: centroid moved %.4g m (theory %.4g); windScale 0: %.4g; 0.5: %.4g",
                     kTicks, static_cast<double>(full.shift), static_cast<double>(expected), static_cast<double>(off.shift),
                     static_cast<double>(half.shift));
        check(std::fabs(full.shift - expected) <= 0.2f * expected, "wind: a dense puff drifts +x by the theoretical distance (within 20%)");
        check(std::fabs(off.shift) < 1.0e-3f && off.maxFlowX == 0.0f, "wind: windScale = 0 leaves the fog and the flow alone");
        check(std::fabs(half.shift - 0.5f * expected) <= 0.2f * 0.5f * expected, "wind: windScale scales the drift");
        check(std::fabs(full.maxFlowX - 2.0f) < 0.05f, "wind: the flow settles at the wind speed");
        // 塊を足さず、風も 0 の場: 全マスが静止状態のまま (ビット一致)
        const WindRun calm = RunWind(0.0f, 60, false);
        check(calm.rest, "wind: with no wind and no source the field stays the exact rest state");
    }
}

} // namespace

namespace {

// ABI v35 (M96d): FieldAddImpulse / FieldSample。C ABI の表を通して積み・読み、内部の関数と一致することを見る
void RunDisturbanceAbiTests(const std::function<void(bool, const char*)>& check)
{
    constexpr int kSide = 32;
    constexpr float kCell = 0.25f;
    // 1 本の場 (32 x 32、濃さのみ) と、その sim に繋いだ ABI の表
    struct Rig {
        Sim sim;
        EntityID field = kNullEntity;
        ScriptApiContext ctx;
        MyeEngineApi api = {};
        void Init()
        {
            field = AddField(sim, 0.0f, 0.0f, 0.0f, kSide, kSide, kDisturbanceChannelDensity);
            sim.Flush();
            sim.Step();
            ctx.scene = &sim.scene;
            BuildEngineApi(api, &ctx);
        }
        // マス (ix, iz) の中心 (ワールド)。y は層の中
        MyeVec3 CellCenter(int ix, int iz) const
        {
            const DisturbanceSlot& s = sim.scene.Disturbance().Slots()[0];
            return { (static_cast<float>(s.originCell[0] + ix) + 0.5f) * kCell, 0.2f,
                     (static_cast<float>(s.originCell[2] + iz) + 0.5f) * kCell };
        }
        size_t Index(int ix, int iz) const
        {
            return static_cast<size_t>(ix) + static_cast<size_t>(kSide) * static_cast<size_t>(iz);
        }
    };

    auto makeImpulse = [](int32_t shape, MyeVec3 pos, MyeVec3 dir, float radius, float speed) {
        MyeFieldImpulse f = {};
        f.pos = pos;
        f.dir = dir;
        f.radius = radius;
        f.speed = speed;
        f.coneDeg = 25.0f;
        f.shape = shape;
        return f;
    };

    // ---- 表の充填・拒否 ----
    {
        auto rig = std::make_unique<Rig>();
        rig->Init();
        const MyeEngineApi& api = rig->api;
        void* const eng = rig->api.engine;
        check(api.version == MYE_API_VERSION && api.FieldAddImpulse != nullptr && api.FieldSample != nullptr,
              "abi: the v35 field slots are filled in");
        const MyeVec3 c = rig->CellCenter(16, 16);
        const MyeFieldImpulse good = makeImpulse(0, c, { 1.0f, 0.0f, 0.0f }, 1.0f, 2.0f);
        MyeFieldImpulse nan = good;
        nan.speed = std::numeric_limits<float>::quiet_NaN();
        MyeFieldImpulse inf = good;
        inf.pos.x = std::numeric_limits<float>::infinity();
        MyeFieldImpulse zeroRadius = good;
        zeroRadius.radius = 0.0f;
        MyeFieldImpulse nanRadius = good;
        nanRadius.radius = std::numeric_limits<float>::quiet_NaN();
        MyeFieldImpulse badShape = good;
        badShape.shape = 3;
        MyeFieldImpulse negShape = good;
        negShape.shape = -1;
        check(api.FieldAddImpulse(eng, nullptr) == 0 && api.FieldAddImpulse(eng, &nan) == 0
                  && api.FieldAddImpulse(eng, &inf) == 0 && api.FieldAddImpulse(eng, &zeroRadius) == 0
                  && api.FieldAddImpulse(eng, &nanRadius) == 0 && api.FieldAddImpulse(eng, &badShape) == 0
                  && api.FieldAddImpulse(eng, &negShape) == 0 && rig->sim.D().Queue().empty(),
              "abi: null / non-finite / radius <= 0 / unknown shape return 0 and queue nothing");
        check(api.FieldAddImpulse(eng, &good) == 1 && rig->sim.D().Queue().size() == 1, "abi: a valid impulse is queued");
        // キューは 256 で満杯。超えたら 0
        bool allQueued = true;
        for (uint32_t i = 1; i < kMaxDisturbanceQueue; ++i) {
            allQueued = allQueued && api.FieldAddImpulse(eng, &good) == 1;
        }
        check(allQueued && api.FieldAddImpulse(eng, &good) == 0, "abi: the 257th impulse is refused");
        // C# レーンからは積めない (読みは許す)
        rig->sim.Step();
        rig->ctx.computeLane = compute::CallerKind::CSharp;
        check(api.FieldAddImpulse(eng, &good) == 0 && rig->sim.D().Queue().empty(),
              "abi: the managed lane cannot queue an impulse");
        MyeFieldSample smp = {};
        check(api.FieldSample(eng, MyeEntityId{}, c, &smp) == 1, "abi: the managed lane can read a sample");
    }

    // ---- 3 形の向き ----
    {
        auto rig = std::make_unique<Rig>();
        rig->Init();
        const DisturbanceSlot& s = rig->sim.D().Slots()[0];
        MyeFieldImpulse radial = makeImpulse(1, rig->CellCenter(16, 16), { 0.0f, 0.0f, 0.0f }, 1.5f, 3.0f);
        rig->api.FieldAddImpulse(rig->api.engine, &radial);
        rig->sim.Step();
        check(s.flowX[rig->Index(18, 16)] > 0.0f && s.flowX[rig->Index(14, 16)] < 0.0f
                  && s.flowZ[rig->Index(16, 18)] > 0.0f && s.flowZ[rig->Index(16, 14)] < 0.0f,
              "abi: radial pushes outward on all four sides");
        check(std::fabs(s.flowX[rig->Index(16, 16)]) < 1.0e-4f && std::fabs(s.flowZ[rig->Index(16, 16)]) < 1.0e-4f,
              "abi: radial leaves the center cell at rest");

        auto rigC = std::make_unique<Rig>();
        rigC->Init();
        const DisturbanceSlot& sc = rigC->sim.D().Slots()[0];
        MyeFieldImpulse cone = makeImpulse(2, rigC->CellCenter(16, 16), { 1.0f, 0.0f, 0.0f }, 3.0f, 3.0f);
        rigC->api.FieldAddImpulse(rigC->api.engine, &cone);
        rigC->sim.Step();
        const float inside = sc.flowX[rigC->Index(19, 16)];
        const float outside = std::fabs(sc.flowX[rigC->Index(16, 22)]);
        check(inside > 0.0f && outside < inside * 0.5f, "abi: a cone pushes along its axis and leaves the far side nearly alone");

        // Point の dir は正規化しない: 長さ 2 の dir は長さ 1 の dir の 2 倍の流れ
        auto rigA = std::make_unique<Rig>();
        rigA->Init();
        auto rigB = std::make_unique<Rig>();
        rigB->Init();
        MyeFieldImpulse one = makeImpulse(0, rigA->CellCenter(16, 16), { 1.0f, 0.0f, 0.0f }, 1.0f, 1.0f);
        MyeFieldImpulse two = one;
        two.dir = { 2.0f, 0.0f, 0.0f };
        rigA->api.FieldAddImpulse(rigA->api.engine, &one);
        rigB->api.FieldAddImpulse(rigB->api.engine, &two);
        rigA->sim.Step();
        rigB->sim.Step();
        const float fa = rigA->sim.D().Slots()[0].flowX[rigA->Index(16, 16)];
        const float fb = rigB->sim.D().Slots()[0].flowX[rigB->Index(16, 16)];
        check(fa > 0.0f && std::fabs(fb - 2.0f * fa) <= 1.0e-4f * fb,
              "abi: a point impulse is not normalized (dir length scales the speed)");
    }

    // ---- 積む時点と効く step ----
    {
        auto rig = std::make_unique<Rig>();
        rig->Init();
        const DisturbanceSlot& s = rig->sim.D().Slots()[0];
        const MyeFieldImpulse imp = makeImpulse(0, rig->CellCenter(16, 16), { 1.0f, 0.0f, 0.0f }, 1.0f, 2.0f);
        // Update (step の前) で積む: 同じ tick の step で効く
        rig->api.FieldAddImpulse(rig->api.engine, &imp);
        rig->sim.Step();
        check(rig->sim.D().Queue().empty() && s.flowX[rig->Index(16, 16)] > 0.0f,
              "timing: an impulse queued before the step acts in that step");
        // LateUpdate (step の後) で積む: その tick の step では効かず、次の step で効く
        auto late = std::make_unique<Rig>();
        late->Init();
        late->sim.Step();
        const DisturbanceSlot& ls = late->sim.D().Slots()[0];
        late->api.FieldAddImpulse(late->api.engine, &imp);
        check(late->sim.D().Queue().size() == 1 && ls.flowX[late->Index(16, 16)] == 0.0f,
              "timing: an impulse queued after the step has not acted yet");
        late->sim.Step();
        check(late->sim.D().Queue().empty() && ls.flowX[late->Index(16, 16)] > 0.0f, "timing: it acts in the next step");
    }

    // ---- FieldSample は内部の読み取りと一致 ----
    {
        auto rig = std::make_unique<Rig>();
        rig->Init();
        void* const eng = rig->api.engine;
        MyeFieldImpulse imp = makeImpulse(0, rig->CellCenter(16, 16), { 1.0f, 0.0f, 0.0f }, 1.0f, 2.0f);
        imp.densityDelta = -0.5f;
        rig->api.FieldAddImpulse(eng, &imp);
        rig->sim.Step(3);
        const MyeVec3 neighbour = rig->CellCenter(17, 16);
        const MyeVec3 probe = { neighbour.x + 0.05f, 0.3f, neighbour.z };
        FieldSampleOut inner;
        const bool innerHit =
            DisturbanceSampleAt(rig->sim.D(), rig->sim.W(), kNullEntity, { probe.x, probe.y, probe.z }, inner);
        MyeFieldSample abi = {};
        check(innerHit && rig->api.FieldSample(eng, MyeEntityId{}, probe, &abi) == 1 && abi.velocity.x == inner.flow[0]
                  && abi.velocity.y == inner.flow[1] && abi.velocity.z == inner.flow[2] && abi.density == inner.density
                  && abi.trail == inner.trail && abi.ripple == inner.ripple && abi.field.index == inner.field.index
                  && abi.field.generation == inner.field.generation && abi.velocity.x > 0.0f && abi.density < 1.0f,
              "abi: FieldSample matches DisturbanceSampleAt");
        // 場を名指しても同じ。無い場を名指すと外
        MyeFieldSample named = {};
        const MyeEntityId fieldId = { rig->field.index, rig->field.generation };
        check(rig->api.FieldSample(eng, fieldId, probe, &named) == 1 && named.density == abi.density,
              "abi: naming the field gives the same sample");
        MyeFieldSample none = {};
        none.density = 5.0f;
        check(rig->api.FieldSample(eng, MyeEntityId{ 999u, 0u }, probe, &none) == 0 && none.density == 0.0f,
              "abi: an unknown field returns 0 with a zeroed sample");
        MyeFieldSample outside = {};
        outside.density = 5.0f;
        check(rig->api.FieldSample(eng, MyeEntityId{}, { 500.0f, 0.2f, 0.0f }, &outside) == 0 && outside.density == 0.0f,
              "abi: outside the grid returns 0 with a zeroed sample");
        check(rig->api.FieldSample(eng, MyeEntityId{}, probe, nullptr) == 0, "abi: a null out returns 0");
    }
}

// 水面の波紋 (M96 sub-12) が読む転送指示の検査。波紋チャネルだけの層でも、状態があれば波紋の配列が
// 転送指示に載り (= RenderSystem が転送して水面が読める)、濃さの配列は載らない。GPU は使わない
bool RunDisturbanceWaterRippleTests()
{
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        if (!cond) {
            MYE_LOG_ERROR("  [disturbance] FAIL: %s", what);
            ok = false;
        }
    };
    auto s = std::make_unique<Sim>();
    const EntityID pond = AddField(*s, 9.0f, 0.12f, 3.0f, 32, 32, kDisturbanceChannelRipple, "Pond");
    const EntityID walker = AddSource(*s, "Walker", 9.0f, 0.12f, 3.0f);
    s->Flush();
    SourceOf(*s, walker)->ripple = 0.08f;
    SourceOf(*s, walker)->fullSpeed = 0.0f; // 速さに依らず係数 1
    (void)pond;
    s->Step(2);
    TransformOf(*s, walker)->position.x = 9.5f;
    s->Step(8);

    DisturbanceFieldUpload up;
    check(BuildDisturbanceFieldUpload(s->W(), &s->D(), up) == 1 && up.layers[0].hasState && !up.layers[0].hasDensity,
          "water ripple: a ripple-only layer with a state is packed without a density channel");
    check(up.layers[0].ripple != nullptr && up.layers[0].density == nullptr,
          "water ripple: the ripple array is handed to the renderer, the density array is not");
    float maxH = 0.0f;
    if (up.layers[0].ripple != nullptr) {
        const size_t n = static_cast<size_t>(up.layers[0].cellsX) * static_cast<size_t>(up.layers[0].cellsZ);
        for (size_t k = 0; k < n; ++k) {
            maxH = std::max(maxH, std::fabs(up.layers[0].ripple[k]));
        }
    }
    check(maxH > 1e-4f && maxH < kMaxRippleHeight, "water ripple: a moving source raises a finite ripple inside the limit");
    return ok;
}

// ---- Volume (3D、M96g) ----

EntityID AddVolume(Sim& s, float x, float y, float z, int cx, int cy, int cz, const char* name = "Volume")
{
    GameObject go = s.scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, y, z);
    auto* f = go.AddComponent<DisturbanceFieldComponent>();
    f->shape = kDisturbanceVolume;
    f->cellsX = cx;
    f->cellsY = cy;
    f->cellsZ = cz;
    f->cellSize = 0.5f;
    f->channels = kDisturbanceChannelDensity;
    f->restDensity = 0.0f; // 何も無い空気。源が足した分だけ煙になる
    return go.Id();
}

// 濃さの重心の高さ (ワールド y)。質量 0 なら NaN にならないよう 0
float DensityCentroidY(const DisturbanceSlot& s)
{
    double mass = 0.0;
    double moment = 0.0;
    for (int y = 0; y < s.cellsY; ++y) {
        const double wy = (static_cast<double>(s.originCell[1] + y) + 0.5) * static_cast<double>(s.cellSize);
        for (int k = 0; k < s.cellsX * s.cellsZ; ++k) {
            const double rho = s.density[static_cast<size_t>(k) + static_cast<size_t>(s.cellsX) * static_cast<size_t>(s.cellsZ) * static_cast<size_t>(y)];
            mass += rho;
            moment += rho * wy;
        }
    }
    return mass > 0.0 ? static_cast<float>(moment / mass) : 0.0f;
}

size_t VolumeIndex(const DisturbanceSlot& s, int x, int y, int z)
{
    return static_cast<size_t>(x) + static_cast<size_t>(s.cellsX) * (static_cast<size_t>(z) + static_cast<size_t>(s.cellsZ) * static_cast<size_t>(y));
}

// 静止状態 (流れ 3 成分 0、濃さ rest) をビットで比べる
bool IsRestVolume(const DisturbanceSlot& s, float rest)
{
    const std::vector<float> zero(s.CellCount(), 0.0f);
    const std::vector<float> dense(s.CellCount(), rest);
    return SameBits(s.flowX, zero) && SameBits(s.flowY, zero) && SameBits(s.flowZ, zero) && SameBits(s.density, dense);
}

// 2 ワールド並走・往復が使う場面: 風、Volume 2 個 (寸法と浮力が違う) + 2D 層 1 個、動く源・上向きの噴き出し
struct VolumeScenario {
    EntityID walker = kNullEntity;
};

VolumeScenario BuildVolumeScenario(Sim& s)
{
    VolumeScenario sc;
    GameObject env = s.scene.CreateGameObjectTracked("Environment");
    env.AddComponent<PhysicsEnvironmentComponent>()->windVelocity = { 0.8f, 0.2f, -0.4f };
    const EntityID a = AddVolume(s, 0.0f, 2.0f, 0.0f, 24, 12, 24, "VolumeA");
    const EntityID b = AddVolume(s, 20.0f, 2.0f, 0.0f, 16, 16, 16, "VolumeB");
    AddField(s, 0.0f, 0.0f, 12.0f, 32, 32, 3, "Layer");
    sc.walker = AddSource(s, "Walker", -3.0f, 1.0f, 0.0f);
    const EntityID vent = AddSource(s, "Vent", 0.0f, 0.0f, 0.0f);
    s.Flush();
    FieldOf(s, a)->buoyancy = 6.0f;
    FieldOf(s, b)->restDensity = 1.0f;
    auto* w = SourceOf(s, sc.walker);
    w->radius = 1.5f;
    w->densityDelta = -0.4f;
    auto* v = SourceOf(s, vent);
    v->mode = 1;
    v->constantVelocity = { 0.0f, 1.0f, 0.0f };
    v->densityDelta = 0.4f;
    v->radius = 1.2f;
    return sc;
}

void DriveVolumeScenario(Sim& s, const VolumeScenario& sc, int tick)
{
    TransformOf(s, sc.walker)->position.x = -3.0f + 0.05f * static_cast<float>(tick % 120);
    TransformOf(s, sc.walker)->position.y = 1.0f + 0.02f * static_cast<float>(tick % 50);
    TransformOf(s, sc.walker)->position.z = 0.03f * static_cast<float>(tick % 60);
}

void LateVolumeInputs(Sim& s, int tick)
{
    if (tick % 70 == 30) {
        FieldImpulse imp = MakeImpulse(0.0f, 1.0f, 0.0f, 2.5f, 0.0f, 0.0f, 5.0f);
        imp.shape = kFieldImpulseRadial;
        DisturbanceQueueImpulse(s.scene, imp);
    }
    if (tick % 70 == 50) {
        FieldImpulse imp = MakeImpulse(20.0f, 2.0f, 0.0f, 3.0f, 1.0f, 0.0f, 4.0f);
        imp.dir[1] = 0.5f; // 3D の向き
        imp.shape = kFieldImpulseCone;
        imp.coneCos = 0.7f;
        imp.density = -0.5f;
        DisturbanceQueueImpulse(s.scene, imp);
    }
}

// Volume の確認項目 (受け入れ 20)。GPU は使わない。--field-bench でも走らせて、selftest を回さずに実行できる
bool RunDisturbanceVolumeTests(bool heavy)
{
    bool ok = true;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  [disturbance] FAIL: %s", what);
            ok = false;
        }
    };
    RegisterBuiltinComponents();

    // ---- 既定値と状態の形 ----
    check(DisturbanceFieldComponent{}.buoyancy == 0.0f && sizeof(DisturbanceFieldComponent) == 88,
          "volume: buoyancy defaults to 0 and is the last field");
    {
        auto s = std::make_unique<Sim>();
        const EntityID v = AddVolume(*s, 3.0f, 2.0f, -1.0f, 32, 16, 32);
        s->Flush();
        FieldOf(*s, v)->channels = 7; // 跡・波紋のビットは Volume では無視される
        s->Step(2);
        check(s->D().Slots().size() == 1 && s->D().OverflowWarnCount() == 0, "volume: a Volume field creates a state and does not warn");
        const DisturbanceSlot& slot = s->D().Slots()[0];
        const size_t n = 32u * 16u * 32u;
        check(slot.shape == kDisturbanceVolume && slot.cellsY == 16 && slot.flowX.size() == n && slot.flowY.size() == n
                  && slot.flowZ.size() == n && slot.channels == kDisturbanceChannelDensity && slot.trail.empty()
                  && slot.ripple.empty() && slot.density.size() == n,
              "volume: 3 flow components and a density array only (trail / ripple bits are ignored)");
        check(slot.originCell[0] == 6 - 16 && slot.originCell[1] == 4 - 8 && slot.originCell[2] == -2 - 16,
              "volume: the origin is centered on the entity on every axis");
        check(IsRestVolume(slot, 0.0f), "volume: with no source the field stays at the rest state");
        s->Step(58);
        check(IsRestVolume(s->D().Slots()[0], 0.0f), "volume: the rest state holds for 60 ticks bit for bit");
        // 2D の場は flowY を持たない (ハッシュ列を増やさない)
        const EntityID layer = AddField(*s, 0.0f, 0.0f, 20.0f, 16, 16, 1, "Layer");
        (void)layer;
        s->Flush();
        s->Step(1);
        check(s->D().Slots().size() == 2 && s->D().Slots()[1].flowY.empty(), "volume: a ground layer keeps an empty flowY");
    }

    // ---- 注入は 3D のユークリッド距離 ----
    {
        auto s = std::make_unique<Sim>();
        const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 16, 16, 16);
        s->Flush();
        FieldOf(*s, v)->pressureIterations = 0;
        FieldOf(*s, v)->velocityKeep = 1.0f;
        s->Step();
        FieldImpulse imp = MakeImpulse(0.25f, 0.25f, 0.25f, 1.6f, 1.0f, 0.0f, 3.0f); // セル中心 (x,y,z = 8,8,8 のセルの中心)
        imp.density = 0.5f;
        DisturbanceQueueImpulse(s->scene, imp);
        s->Step();
        const DisturbanceSlot& d = s->D().Slots()[0];
        check(d.flowX[VolumeIndex(d, 8, 8, 8)] > 0.0f && d.density[VolumeIndex(d, 8, 8, 8)] > 0.0f,
              "volume impulse: the center cell is pushed and filled");
    }
    {
        // 半径 1.0 の球の外 (y 方向 1.5 m) は触らない。球の内 (y 方向 0.5 m) は触る
        auto s = std::make_unique<Sim>();
        const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 16, 16, 16);
        s->Flush();
        FieldOf(*s, v)->pressureIterations = 0;
        s->Step();
        FieldImpulse imp = MakeImpulse(0.25f, 0.25f, 0.25f, 1.0f, 1.0f, 0.0f, 3.0f);
        imp.density = 0.5f;
        DisturbanceQueueImpulse(s->scene, imp);
        s->Step();
        const DisturbanceSlot& d = s->D().Slots()[0];
        check(d.density[VolumeIndex(d, 8, 9, 8)] > 0.0f && d.density[VolumeIndex(d, 8, 11, 8)] == 0.0f
                  && d.density[VolumeIndex(d, 8, 8, 11)] == 0.0f,
              "volume impulse: the radius is a 3D ball (0.5 m up is inside, 1.5 m up and 1.5 m aside are outside)");
        // 球の外の角 (x,y,z すべて 1.0 m ずれ → 距離 1.73 m > 1.0 m) は、立方体の範囲には入るが触らない
        check(d.density[VolumeIndex(d, 10, 10, 10)] == 0.0f, "volume impulse: a corner of the bounding box outside the ball is untouched");
    }
    {
        // Radial: 上のマスは +y、下のマスは -y へ。中心は 0
        auto s = std::make_unique<Sim>();
        const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 16, 16, 16);
        s->Flush();
        FieldOf(*s, v)->pressureIterations = 0;
        s->Step();
        FieldImpulse imp = MakeImpulse(0.25f, 0.25f, 0.25f, 2.0f, 0.0f, 0.0f, 4.0f);
        imp.shape = kFieldImpulseRadial;
        DisturbanceQueueImpulse(s->scene, imp);
        s->Step();
        const DisturbanceSlot& d = s->D().Slots()[0];
        check(d.flowY[VolumeIndex(d, 8, 10, 8)] > 0.0f && d.flowY[VolumeIndex(d, 8, 6, 8)] < 0.0f
                  && d.flowY[VolumeIndex(d, 8, 8, 8)] == 0.0f && d.flowX[VolumeIndex(d, 8, 8, 8)] == 0.0f,
              "volume impulse: Radial pushes away from the point in 3D (zero at the exact center)");
        FieldImpulse cone = MakeImpulse(0.25f, 0.25f, 0.25f, 2.0f, 0.0f, 0.0f, 4.0f);
        cone.dir[1] = 1.0f;
        cone.shape = kFieldImpulseCone;
        cone.coneCos = 0.9f;
        s->D().Clear();
        s->Step();
        DisturbanceQueueImpulse(s->scene, cone);
        s->Step();
        const DisturbanceSlot& c = s->D().Slots()[0];
        check(c.flowY[VolumeIndex(c, 8, 10, 8)] > 0.0f && c.flowY[VolumeIndex(c, 12, 8, 8)] == 0.0f
                  && c.flowY[VolumeIndex(c, 8, 6, 8)] == 0.0f,
              "volume impulse: an upward Cone pushes inside the cone only");
    }

    // ---- 浮力 (受け入れ 20) ----
    {
        const auto run = [&](float buoyancy, std::vector<float>& centroids) {
            auto s = std::make_unique<Sim>();
            const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 16, 16, 16);
            const EntityID vent = AddSource(*s, "Vent", 0.0f, -1.5f, 0.0f);
            s->Flush();
            FieldOf(*s, v)->buoyancy = buoyancy;
            auto* src = SourceOf(*s, vent);
            src->mode = 1;
            src->strength = 0.0f;
            src->densityDelta = 0.5f;
            src->radius = 1.2f;
            s->Step(30);
            SourceOf(*s, vent)->densityDelta = 0.0f; // 源を止める
            for (int t = 0; t < 40; ++t) {
                s->Step();
                centroids.push_back(DensityCentroidY(s->D().Slots()[0]));
            }
            return s->D().Slots()[0].density.size() == 16u * 16u * 16u;
        };
        std::vector<float> rising;
        std::vector<float> still;
        check(run(8.0f, rising) && run(0.0f, still), "volume buoyancy: both runs completed");
        bool everyTick = rising.size() == 40;
        for (size_t i = 1; everyTick && i < rising.size(); ++i) {
            everyTick = rising[i] > rising[i - 1];
        }
        check(everyTick && rising.back() - rising.front() > 0.1f,
              "volume buoyancy: with buoyancy the density centroid rises on every tick, also after the source is stopped");
        check(still.size() == 40 && std::fabs(still.back() - still.front()) < 1.0e-3f && still.front() < -0.5f,
              "volume buoyancy: with buoyancy = 0 the smoke does not rise (the centroid stays at the vent)");
    }

    // ---- 風は 3 成分に効く ----
    {
        auto s = std::make_unique<Sim>();
        GameObject env = s->scene.CreateGameObjectTracked("Environment");
        env.AddComponent<PhysicsEnvironmentComponent>()->windVelocity = { 0.0f, 1.0f, 0.0f };
        const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 16, 16, 16);
        s->Flush();
        s->Step(120);
        const DisturbanceSlot& d = s->D().Slots()[0];
        const float centerY = d.flowY[VolumeIndex(d, 8, 8, 8)];
        const float centerX = d.flowX[VolumeIndex(d, 8, 8, 8)];
        MYE_LOG_INFO("  [disturbance] volume wind: center flow (%g, %g)", centerX, centerY);
        // 上向きの風が格子の縁から入って満ちる。投影が起こす横向きの流れは上向きに比べて小さい
        check(centerY > 0.5f && std::fabs(centerX) < 0.1f * centerY, "volume wind: a vertical wind fills the volume with an upward flow");
        (void)v;
    }

    // ---- 読み取り ----
    {
        auto s = std::make_unique<Sim>();
        const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 16, 16, 16);
        s->Flush();
        FieldOf(*s, v)->pressureIterations = 0;
        FieldOf(*s, v)->velocityKeep = 1.0f;
        s->Step();
        FieldImpulse imp = MakeImpulse(0.25f, 0.25f, 0.25f, 1.5f, 0.0f, 0.0f, 3.0f);
        imp.dir[1] = 1.0f;
        imp.density = 0.6f;
        DisturbanceQueueImpulse(s->scene, imp);
        s->Step();
        FieldSampleOut out;
        check(DisturbanceSampleAt(s->D(), s->W(), kNullEntity, { 0.25f, 0.25f, 0.25f }, out) && out.flow[1] > 0.0f
                  && out.density > 0.0f && out.field == v,
              "volume sample: inside the grid it returns the 3-component flow and the density");
        check(!DisturbanceSampleAt(s->D(), s->W(), kNullEntity, { 0.25f, 40.0f, 0.25f }, out) && out.flow[1] == 0.0f,
              "volume sample: above the grid is outside (no height shape for a Volume)");
        check(DisturbanceSampleAt(s->D(), s->W(), kNullEntity, { 0.25f, 3.5f, 0.25f }, out), "volume sample: the top of the grid is inside");
    }

    // ---- ハッシュは流れ Y を含む / 描画への詰め替え ----
    {
        auto s = std::make_unique<Sim>();
        const EntityID v = AddVolume(*s, 0.0f, 0.0f, 0.0f, 8, 8, 8);
        s->Flush();
        s->Step();
        const uint64_t before = s->Hash();
        s->D().Slots()[0].flowY[5] = 0.25f;
        check(s->Hash() != before, "volume hash: flowY is folded into the world hash");
        (void)v;
    }
    {
        auto s = std::make_unique<Sim>();
        DisturbanceVolumeUpload up;
        check(BuildDisturbanceVolumeUpload(s->W(), nullptr, up) == 0, "volume render: a scene without fields packs no volume");
        const EntityID v0 = AddVolume(*s, 0.0f, 2.0f, 0.0f, 8, 8, 8, "V0");
        AddField(*s, 0.0f, 0.0f, 9.0f, 16, 16, 1, "Layer");
        const EntityID v1 = AddVolume(*s, 10.0f, 2.0f, 0.0f, 12, 6, 10, "V1");
        const EntityID v2 = AddVolume(*s, 20.0f, 2.0f, 0.0f, 8, 8, 8, "V2");
        const EntityID v3 = AddVolume(*s, 30.0f, 2.0f, 0.0f, 8, 8, 8, "V3");
        const EntityID v4 = AddVolume(*s, 40.0f, 2.0f, 0.0f, 8, 8, 8, "V4");
        s->Flush();
        // 先頭 4 個 (entity index 昇順) = V0, Layer, V1, V2。2D 層は Volume の数に入らず、V3 / V4 は sim も描画もされない
        check(BuildDisturbanceVolumeUpload(s->W(), &s->D(), up) == 3 && up.volumes[0].entityIndex == v0.index
                  && up.volumes[1].entityIndex == v1.index && up.volumes[2].entityIndex == v2.index && !up.volumes[0].hasState
                  && up.volumes[1].cellsX == 12 && up.volumes[1].cellsY == 6 && up.volumes[1].cellsZ == 10,
              "volume render: the first four fields by entity index are considered, volumes keep their own sizes, the layer is not counted");
        check(up.volumes[0].originCell[1] == 4 - 4, "volume render: before the first step the origin is the one the step will build");
        s->Step(2);
        check(BuildDisturbanceVolumeUpload(s->W(), &s->D(), up) == 3 && up.volumes[0].hasState && up.volumes[0].flowY != nullptr
                  && up.volumes[0].density == s->D().Slots()[0].density.data() && up.serial == s->D().RenderSerial(),
              "volume render: with a state the volume points at the slot's arrays and carries the serial");
        (void)v3;
        (void)v4;
        // 4 個とも Volume の場面 (描画は最大 4 個)
        auto q = std::make_unique<Sim>();
        for (int i = 0; i < 6; ++i) {
            AddVolume(*q, static_cast<float>(i) * 10.0f, 2.0f, 0.0f, 8, 8, 8, "Q");
        }
        q->Flush();
        q->Step();
        check(BuildDisturbanceVolumeUpload(q->W(), &q->D(), up) == 4 && q->D().Slots().size() == 4,
              "volume render: four volumes are simulated and packed, the fifth and sixth are not");
    }

    // ---- 決定論: 2 ワールド並走 (acc 20) ----
    {
        const int ticks = heavy ? 600 : 120;
        auto a = std::make_unique<Sim>();
        auto b = std::make_unique<Sim>();
        const VolumeScenario sa = BuildVolumeScenario(*a);
        const VolumeScenario sb = BuildVolumeScenario(*b);
        a->Flush();
        b->Flush();
        bool same = true;
        uint64_t last = 0;
        for (int t = 0; t < ticks; ++t) {
            DriveVolumeScenario(*a, sa, t);
            DriveVolumeScenario(*b, sb, t);
            a->Step();
            b->Step();
            LateVolumeInputs(*a, t);
            LateVolumeInputs(*b, t);
            last = a->Hash();
            same = same && last == b->Hash();
        }
        MYE_LOG_INFO("  [disturbance] volume two-world scenario final hash @%d = %016llX", ticks, static_cast<unsigned long long>(last));
        check(same, "volume determinism: two worlds hash-match on every tick");
        check(a->D().Slots().size() == 3 && MaxAbsDev(a->D().Slots()[0].flowY, 0.0f) > 0.0f
                  && MaxAbsDev(a->D().Slots()[0].density, 0.0f) > 0.0f,
              "volume determinism: the compared scene has moving 3D state");
    }

    // ---- スナップショット往復 (acc 20) ----
    if (!heavy) {
        auto a = std::make_unique<Sim>();
        const VolumeScenario sc = BuildVolumeScenario(*a);
        a->Flush();
        for (int t = 0; t < 100; ++t) {
            DriveVolumeScenario(*a, sc, t);
            a->Step();
            LateVolumeInputs(*a, t);
        }
        DisturbanceQueueImpulse(a->scene, MakeImpulse(0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 2.0f));
        SimRefs refsA;
        refsA.scene = &a->scene;
        std::vector<std::byte> blob;
        check(CaptureSimSnapshot(refsA, blob), "volume snapshot: capture succeeds");
        const uint64_t hashA = a->Hash();
        auto b = std::make_unique<Sim>();
        SimRefs refsB;
        refsB.scene = &b->scene;
        check(RestoreSimSnapshot(refsB, blob.data(), blob.size()), "volume snapshot: restore into a fresh scene succeeds");
        bool slotsSame = a->D().Slots().size() == b->D().Slots().size();
        for (size_t i = 0; slotsSame && i < a->D().Slots().size(); ++i) {
            slotsSame = SameSlot(a->D().Slots()[i], b->D().Slots()[i]);
        }
        check(slotsSame && b->Hash() == hashA, "volume snapshot: the restored scene has the same 3D arrays (flowY included) and world hash");
        a->transforms.Update(a->W());
        b->transforms.Update(b->W());
        const VolumeScenario scB{ b->scene.Find("Walker").Id() };
        bool same = true;
        for (int t = 100; t < 200 && same; ++t) {
            DriveVolumeScenario(*a, sc, t);
            DriveVolumeScenario(*b, scB, t);
            a->Step();
            b->Step();
            LateVolumeInputs(*a, t);
            LateVolumeInputs(*b, t);
            same = same && a->Hash() == b->Hash();
        }
        check(same, "volume snapshot: 100 ticks after the round trip every tick hash matches");
    }
    return ok;
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
        float prevDev = 1e9f, prevTrail = 1e9f, flow = 0.0f;
        bool monotone = true;
        for (int t = 0; t < ticks; ++t) {
            sim->Step();
            const DisturbanceSlot& s = sim->D().Slots()[0];
            const float dev = MaxAbsDev(s.density, 1.0f);
            const float trail = MaxAbsDev(s.trail, 0.0f);
            flow = MaxFlow(s);
            monotone = monotone && dev <= prevDev && trail <= prevTrail;
            prevDev = dev;
            prevTrail = trail;
        }
        // 移流と投影が入っても濃さと跡の最大偏差は増えない (双線形は凸結合、復元は縮小写像)。
        // 流れの最大は投影で局所的に増えうるので単調性は主張せず、十分小さくなることを見る
        check(monotone, "decay: |density - rest| and trail never increase after the source stops (advection and projection on)");
        check(flow < 0.05f, "decay: the flow dies out after the source stops");
        check(prevDev < 1.0e-4f, "decay: density comes back to within 1e-4 of the rest value");
        MYE_LOG_INFO("  [disturbance] after %d ticks: density dev %.3g, trail %.3g, flow %.3g", ticks,
                     static_cast<double>(prevDev), static_cast<double>(prevTrail), static_cast<double>(flow));
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
        // 速さ 0: 流れが起きないので移流で濃さが半径の外へ滲まない (注入の範囲だけを見る)
        FieldImpulse corner = MakeImpulse(-4.0f, 0.0f, -4.0f, 1.0f, 1.0f, 0.0f, 0.0f);
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
                src->strength = 0.0f; // 流れを起こさない (濃さの係数だけを比べる)
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
        FieldOf(*sim, f)->pressureIterations = 0; // 放射状の流れは発散そのもの。投影すると消える
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
        uint64_t finalHash = 0;
        const bool same = RunTwoWorlds(ticks, finalHash);
        MYE_LOG_INFO("  [disturbance] two-world scenario final hash @%d = %016llX", ticks,
                     static_cast<unsigned long long>(finalHash));
        check(same, "determinism: two worlds hash-match on every tick");
        if (heavy) {
            // 600 tick 後の最終ハッシュ。Debug == Release (欄や計算を変えたら理由を添えて再採取する。
            // M96d: 流れのソルバ (移流・投影・Radial の発散源)、欄 2 つ、場の畳み方 (HashStateBytes) で変わった。
            // M96g: DisturbanceFieldComponent の末尾に buoyancy (4 バイト) を足したので、World のカラムの生バイトが伸びて変わった
            // (0x674FE5CFD80F29A9 → 下の値。2D の sim は 1 ビットも変えていない)。--field-bench が同じ値を出す)
            constexpr uint64_t kBakedFinalHash = 0x2D2D2A180F889F62ull;
            check(finalHash == kBakedFinalHash, "determinism: the final hash at tick 600 equals the baked value (Debug == Release)");
        }
    }

    // ---- 流れのソルバ (acc 3 / 13 / 16) ----
    if (!heavy) {
        SolverChecks(check);
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
        // 太陽光の bool は詰め替えを素通りして描画 (地面の霧の CB) へ届く
        check(!up.layers[0].fogSunLight, "render: fogSunLight is off by default");
        s->W().GetComponent<DisturbanceFieldComponent>(f)->fogSunLight = true;
        check(BuildDisturbanceFieldUpload(s->W(), &s->D(), up) == 1 && up.layers[0].fogSunLight,
              "render: fogSunLight reaches the upload");
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

    if (!heavy) {
        RunDisturbanceAbiTests(check); // M96d (ABI v35)
    }

    // 水面の波紋 (M96 sub-12)
    if (!RunDisturbanceWaterRippleTests()) {
        ++failCount;
    }

    // Volume (3D、M96g)
    if (!RunDisturbanceVolumeTests(heavy)) {
        ++failCount;
    }

    return failCount == 0;
}

// M96o: 地面の霧を照らす局所ライトの選び方 (GPU を使わない純粋関数) と欄の既定値
bool RunDisturbanceLocalLightTests()
{
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };
    check(!DisturbanceFieldComponent{}.fogLocalLights, "local lights: fogLocalLights is off by default");
    check(sizeof(DisturbanceFieldComponent) == 88, "local lights: the component size is only the 4 bytes of buoyancy more (M96g)");

    // 原点 (0,0,0) から 8x8 マス (cellSize 1)、y は 0〜1 m の 1 枚の層
    DisturbanceFieldCB layers;
    layers.header[0] = 1;
    DisturbanceLayerCB& l = layers.layers[0];
    l.originSize[2] = 1.0f;
    l.extent[0] = 8.0f;
    l.extent[1] = 8.0f;
    l.extent[2] = 1.0f;
    l.look[0] = 1.0f;
    l.look[3] = 1.0f;

    SceneLightData lights;
    const auto addPoint = [&](float x, float y, float z, float range) {
        GpuLight& g = lights.lights[lights.count++];
        g.type = 1;
        g.position = { x, y, z };
        g.range = range;
        g.color = { 1.0f, 0.5f, 0.25f };
        g.intensity = 2.0f;
    };
    GroundFogLightsCB out;

    check(SelectGroundFogLights(lights, layers, 1u, out) == 0 && out.header[0] == 0, "local lights: no lights selects none");
    addPoint(4.0f, 0.5f, 4.0f, 3.0f);      // 板の中
    addPoint(30.0f, 0.5f, 4.0f, 3.0f);     // 板から遠い (影響球が交わらない)
    addPoint(8.0f + 2.0f, 0.5f, 4.0f, 3.0f); // 板の縁から 2 m、range 3 で届く
    lights.lights[lights.count++].type = 0;  // 平行光は選ばれない
    check(SelectGroundFogLights(lights, layers, 1u, out) == 2 && out.header[0] == 2 && out.header[1] == 1u,
          "local lights: only point / spot lights whose sphere reaches the slab are selected");
    check(out.lights[0].posRange[0] == 4.0f && out.lights[1].posRange[0] == 10.0f,
          "local lights: the selection keeps the light list order");
    check(out.lights[0].colorKind[0] == 2.0f && out.lights[0].colorKind[1] == 1.0f && out.lights[0].colorKind[3] == 1.0f,
          "local lights: the color is premultiplied by the intensity and the kind is carried");
    check(SelectGroundFogLights(lights, layers, 0u, out) == 0 && out.header[0] == 0 && out.header[1] == 0,
          "local lights: a layer with fogLocalLights off selects nothing");

    // 9 本置く: 8 本だけ詰め、戻り値は条件を満たした総数 (超過の本数を呼び出し側が数えられる)
    lights = SceneLightData{};
    for (int i = 0; i < 9; ++i) {
        addPoint(1.0f + static_cast<float>(i) * 0.5f, 0.5f, 4.0f, 2.0f);
    }
    check(SelectGroundFogLights(lights, layers, 1u, out) == 9 && out.header[0] == kGroundFogMaxLights
              && out.lights[kGroundFogMaxLights - 1].posRange[0] == 1.0f + 7.0f * 0.5f,
          "local lights: 9 lights keep the first 8 and report 9");

    // 板の高さ: baseY を上げると真下の低いライトも、高さ方向の距離が range 以内なら届く
    lights = SceneLightData{};
    addPoint(4.0f, 5.0f, 4.0f, 3.0f); // 板の上端 (y = 1) から 4 m 上: 届かない
    check(SelectGroundFogLights(lights, layers, 1u, out) == 0, "local lights: a light above the slab out of range is dropped");
    lights.lights[0].range = 4.5f;
    check(SelectGroundFogLights(lights, layers, 1u, out) == 1, "local lights: a light above the slab within range is kept");

    MYE_LOG_INFO("  local lights: %s", failCount == 0 ? "OK" : "FAILED");
    return failCount == 0;
}

} // namespace

bool RunDisturbanceFieldSelfTest()
{
    const bool localLightsOk = RunDisturbanceLocalLightTests();
    return RunDisturbanceFieldImpl(false) && localLightsOk;
}

bool RunDisturbanceFieldLongSelfTest()
{
    return RunDisturbanceFieldImpl(true);
}

int RunDisturbanceFieldBenchmark()
{
    MYE_LOG_INFO("==== Disturbance field benchmark ====");
    RegisterBuiltinComponents();
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        MYE_LOG_INFO("  %s: %s", cond ? "PASS" : "FAIL", what);
        failCount += cond ? 0 : 1;
    };
    // 他の起動経路が起こしたワーカーは止めない。ここで起こした分だけ片付ける
    const bool startedHere = jobs::System().WorkerCount() == 0;
    jobs::System().Init();
    MYE_LOG_INFO("  workers = %d", jobs::System().WorkerCount());

    // 焼き込みハッシュの再採取用 (selftest の長い層と同じ場面)
    uint64_t finalHash = 0;
    const bool same = RunTwoWorlds(600, finalHash);
    MYE_LOG_INFO("  [bench] two-world scenario @600: match=%d final hash = 0x%016llXull", same ? 1 : 0,
                 static_cast<unsigned long long>(finalHash));

    SolverChecks(check);
    // Volume の確認項目 (selftest を回さずに、この経路で実行して確かめる)
    check(RunDisturbanceVolumeTests(false), "volume checks (the same ones the disturbance suite runs)");

    // 性能: 128 x 128、動く源 3 + 定常の源 1 + 風。step と HashWorld を別々に測る。
    // jobs なし / あり、反復回数とチャネルを変えた内訳 (どこが重いか) を並べる
    const auto measure = [&](bool jobsOn, int iterations, uint32_t channels) {
        jobs::System().SetEnabled(jobsOn);
        auto sim = std::make_unique<Sim>();
        GameObject envGo = sim->scene.CreateGameObjectTracked("Environment");
        envGo.AddComponent<PhysicsEnvironmentComponent>()->windVelocity = { 2.0f, 0.0f, 0.5f };
        const EntityID field = AddField(*sim, 0.0f, 0.0f, 0.0f, 128, 128, channels);
        EntityID movers[3];
        for (int i = 0; i < 3; ++i) {
            movers[i] = AddSource(*sim, "Mover", -8.0f, 0.0f, static_cast<float>(i - 1) * 6.0f);
            SourceOf(*sim, movers[i])->radius = 1.5f;
            SourceOf(*sim, movers[i])->trailDepth = 0.3f;
            SourceOf(*sim, movers[i])->ripple = 0.05f;
        }
        const EntityID fan = AddSource(*sim, "Fan", 8.0f, 0.0f, 0.0f);
        sim->Flush();
        FieldOf(*sim, field)->pressureIterations = iterations;
        auto* fanSrc = SourceOf(*sim, fan);
        fanSrc->mode = 1;
        fanSrc->constantVelocity = { -2.0f, 0.0f, 1.0f };
        fanSrc->densityDelta = 0.1f;
        constexpr int kWarm = 60;
        constexpr int kMeasured = 400;
        constexpr int kWindow = 10; // 他のプロセスが CPU を使う環境でも下限が読めるよう、10 tick 窓 (40 個) の最良・平均・最悪を出す
        double stepMs = 0.0;
        double hashMs = 0.0;
        double windowStep = 0.0;
        double windowHash = 0.0;
        double bestStep = 1.0e9;
        double bestHash = 1.0e9;
        double bestTotal = 1.0e9;
        double worstTotal = 0.0;
        for (int t = 0; t < kWarm + kMeasured; ++t) {
            for (int i = 0; i < 3; ++i) {
                TransformOf(*sim, movers[i])->position.x = -8.0f + 0.05f * static_cast<float>(t % 320);
            }
            const auto t0 = std::chrono::steady_clock::now();
            sim->Step();
            const auto t1 = std::chrono::steady_clock::now();
            const uint64_t h = sim->Hash();
            const auto t2 = std::chrono::steady_clock::now();
            (void)h;
            if (t >= kWarm) {
                const double st = std::chrono::duration<double, std::milli>(t1 - t0).count();
                const double hs = std::chrono::duration<double, std::milli>(t2 - t1).count();
                stepMs += st;
                hashMs += hs;
                windowStep += st;
                windowHash += hs;
                if ((t - kWarm + 1) % kWindow == 0) {
                    const double total = (windowStep + windowHash) / kWindow;
                    if (total < bestTotal) {
                        bestTotal = total;
                        bestStep = windowStep / kWindow;
                        bestHash = windowHash / kWindow;
                    }
                    worstTotal = std::max(worstTotal, total);
                    windowStep = 0.0;
                    windowHash = 0.0;
                }
            }
        }
        MYE_LOG_INFO("  [bench] jobs %s, %2d iterations, channels %u: mean step %.3f + hash %.3f = %.3f ms/tick; "
                     "best %d-tick window step %.3f + hash %.3f = %.3f, worst window %.3f ms/tick",
                     jobsOn ? "on " : "off", iterations, channels, stepMs / kMeasured, hashMs / kMeasured,
                     (stepMs + hashMs) / kMeasured, kWindow, bestStep, bestHash, bestTotal, worstTotal);
        if (!jobsOn && iterations == 20 && channels == 7) {
            SimRefs refs;
            refs.scene = &sim->scene;
            std::vector<std::byte> blob;
            const bool ok = CaptureSimSnapshot(refs, blob);
            MYE_LOG_INFO("  [bench] snapshot: capture=%d, %zu bytes (1 field 128x128, channels 7)", ok ? 1 : 0, blob.size());
        }
    };
    measure(false, 20, 7);
    measure(true, 20, 7);
    measure(false, 0, 7);
    measure(false, 20, 1);

    // Volume (M96g): 32 x 16 x 32 (16384 マス)・20 反復・風・動く源 2 + 上向きの噴き出し 1 + 浮力。窓の測り方は上と同じ
    uint64_t volumeFinalHash[2] = {}; // [jobs なし, jobs あり] の 20 反復の最終ハッシュ (同じ入力なので一致するはず)
    const auto measureVolume = [&](bool jobsOn, int iterations) {
        jobs::System().SetEnabled(jobsOn);
        auto sim = std::make_unique<Sim>();
        GameObject envGo = sim->scene.CreateGameObjectTracked("Environment");
        envGo.AddComponent<PhysicsEnvironmentComponent>()->windVelocity = { 1.0f, 0.2f, 0.5f };
        const EntityID vol = AddVolume(*sim, 0.0f, 4.0f, 0.0f, 32, 16, 32);
        EntityID movers[2];
        for (int i = 0; i < 2; ++i) {
            movers[i] = AddSource(*sim, "Mover", -6.0f, 1.0f, static_cast<float>(i) * 4.0f - 2.0f);
            SourceOf(*sim, movers[i])->radius = 1.5f;
        }
        const EntityID vent = AddSource(*sim, "Vent", 0.0f, 0.5f, 0.0f);
        sim->Flush();
        FieldOf(*sim, vol)->pressureIterations = iterations;
        FieldOf(*sim, vol)->buoyancy = 5.0f;
        auto* ventSrc = SourceOf(*sim, vent);
        ventSrc->mode = 1;
        ventSrc->constantVelocity = { 0.0f, 1.5f, 0.0f };
        ventSrc->densityDelta = 0.3f;
        constexpr int kWarm = 60;
        constexpr int kMeasured = 400;
        constexpr int kWindow = 10;
        double stepMs = 0.0;
        double hashMs = 0.0;
        double windowStep = 0.0;
        double windowHash = 0.0;
        double bestStep = 1.0e9;
        double bestHash = 1.0e9;
        double bestTotal = 1.0e9;
        double worstTotal = 0.0;
        for (int t = 0; t < kWarm + kMeasured; ++t) {
            for (int i = 0; i < 2; ++i) {
                TransformOf(*sim, movers[i])->position.x = -6.0f + 0.04f * static_cast<float>(t % 300);
            }
            const auto t0 = std::chrono::steady_clock::now();
            sim->Step();
            const auto t1 = std::chrono::steady_clock::now();
            const uint64_t h = sim->Hash();
            const auto t2 = std::chrono::steady_clock::now();
            if (iterations == 20) {
                volumeFinalHash[jobsOn ? 1 : 0] = h;
            }
            if (t >= kWarm) {
                const double st = std::chrono::duration<double, std::milli>(t1 - t0).count();
                const double hs = std::chrono::duration<double, std::milli>(t2 - t1).count();
                stepMs += st;
                hashMs += hs;
                windowStep += st;
                windowHash += hs;
                if ((t - kWarm + 1) % kWindow == 0) {
                    const double total = (windowStep + windowHash) / kWindow;
                    if (total < bestTotal) {
                        bestTotal = total;
                        bestStep = windowStep / kWindow;
                        bestHash = windowHash / kWindow;
                    }
                    worstTotal = std::max(worstTotal, total);
                    windowStep = 0.0;
                    windowHash = 0.0;
                }
            }
        }
        MYE_LOG_INFO("  [bench] volume 32x16x32, jobs %s, %2d iterations: mean step %.3f + hash %.3f = %.3f ms/tick; "
                     "best %d-tick window step %.3f + hash %.3f = %.3f, worst window %.3f ms/tick",
                     jobsOn ? "on " : "off", iterations, stepMs / kMeasured, hashMs / kMeasured, (stepMs + hashMs) / kMeasured,
                     kWindow, bestStep, bestHash, bestTotal, worstTotal);
        if (!jobsOn && iterations == 20) {
            SimRefs refs;
            refs.scene = &sim->scene;
            std::vector<std::byte> blob;
            const bool ok = CaptureSimSnapshot(refs, blob);
            MYE_LOG_INFO("  [bench] volume snapshot: capture=%d, %zu bytes (1 volume 32x16x32)", ok ? 1 : 0, blob.size());
        }
    };
    measureVolume(false, 20);
    measureVolume(true, 20);
    measureVolume(false, 0);
    MYE_LOG_INFO("  [bench] volume final hash @460: jobs off = %016llX, jobs on = %016llX",
                 static_cast<unsigned long long>(volumeFinalHash[0]), static_cast<unsigned long long>(volumeFinalHash[1]));
    check(volumeFinalHash[0] != 0 && volumeFinalHash[0] == volumeFinalHash[1], "volume: jobs on / off final hashes match");
    jobs::System().SetEnabled(true);
    if (startedHere) {
        jobs::System().Shutdown();
    }
    MYE_LOG_INFO("==== Disturbance field benchmark: %s ====", failCount == 0 ? "all checks passed" : "CHECKS FAILED");
    return 0;
}


} // namespace mye
