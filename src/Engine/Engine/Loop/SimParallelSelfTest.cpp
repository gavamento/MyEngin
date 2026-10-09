//====================================================================================
//                          SimParallelSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          sim の並列化 (ADR-028) の jobs あり / なし一致の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Loop/SimParallelSelfTest.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Engine/Animation/PartFollowSystem.h"
#include "Engine/Engine/Animation/TwoBoneIkSystem.h"
#include "Engine/Engine/Particles/CpuParticleBackend.h"
#include "Engine/Engine/Perception/PerceptionSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h" // SolidContact
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Mesh/Skeleton.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr int kTicks = 90;

// 並列化の最小単位 (各系の kXxxGrain) を超える数。これ未満だと直列と同じ経路になって何も試せない
constexpr int kEmitterCount = 24;
constexpr int kObserverCount = 40;
constexpr int kBodyCount = 200;

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

// 1 回の実行ぶんの観測 (jobs の統計の差分)
struct JobDelta {
    uint64_t batches = 0;
    uint64_t workerChunks = 0;
};

// 系の Update だけの所要時間 [ms] の合計 (jobs あり / なし)。参考値でゲートにしない
struct UpdateTimes {
    double jobsOnMs = 0.0;
    double jobsOffMs = 0.0;
};

// fn() の所要時間 [ms] を acc に足す
template <typename Fn>
void TimeInto(double& acc, Fn&& fn)
{
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void LogTimes(const char* name, const UpdateTimes& t)
{
    MYE_LOG_INFO("  [time] %s: update %.3f ms/tick with jobs, %.3f ms/tick serial (reference only)", name,
                 t.jobsOnMs / kTicks, t.jobsOffMs / kTicks);
}

// 全 tick を jobs あり (a) / なし (b) で回し、毎 tick のハッシュが一致したかを返す。
// stepA / stepB は引数の jobs 設定のまま 1 tick 進めて、その tick の状態ハッシュを返す
bool RunAB(const std::function<uint64_t(int tick)>& stepA, const std::function<uint64_t(int tick)>& stepB,
           JobDelta& delta, uint64_t& lastHash)
{
    bool same = true;
    const jobs::JobSystem::Stats before = jobs::System().GetStats();
    for (int tick = 0; tick < kTicks; ++tick) {
        jobs::System().SetEnabled(true);
        const uint64_t a = stepA(tick);
        jobs::System().SetEnabled(false);
        const uint64_t b = stepB(tick);
        same = same && (a == b);
        lastHash = a;
    }
    jobs::System().SetEnabled(true);
    const jobs::JobSystem::Stats after = jobs::System().GetStats();
    delta.batches = after.parallelBatches - before.parallelBatches;
    delta.workerChunks = after.workerChunks - before.workerChunks;
    return same;
}

uint64_t Fnv(uint64_t h, const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ---- CPU 粒子 ----
void BuildEmitters(Scene& scene)
{
    for (int i = 0; i < kEmitterCount; ++i) {
        GameObject go = scene.CreateGameObjectTracked("Emitter");
        auto* em = go.AddComponent<ParticleEmitterComponent>();
        em->seed = 100u + static_cast<uint32_t>(i);
        em->shape = i % 4;
        em->rate = 40.0f + static_cast<float>(i % 7) * 25.0f;
        em->turbulence = 0.3f * static_cast<float>(i % 3);
        em->turbulenceMode = i % 2; // 0 = 渦 (SIMD) / 1 = カールノイズ (スカラー)
        em->simulationSpace = (i % 5 == 0) ? 1 : 0;
        em->burstCount = (i % 6 == 0) ? 30 : 0;
        em->prewarmTime = (i % 8 == 0) ? 0.5f : 0.0f;
        em->subframeEmission = (i % 4 == 1);
        em->velocityInheritance = (i % 3 == 2) ? 0.5f : 0.0f;
        em->maxParticles = 400;
    }
    scene.GetWorld().ApplyStructuralChanges();
}

void MoveEmitters(World& world, int tick)
{
    const ComponentTypeId req[] = { ParticleEmitterComponent::sTypeId, WorldMatrixComponent::sTypeId };
    int n = 0;
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row, ++n) {
            auto* wm = static_cast<WorldMatrixComponent*>(arch.GetPtr(wi, row));
            wm->value._41 = static_cast<float>(n) * 0.7f + 0.01f * static_cast<float>(tick);
            wm->value._42 = static_cast<float>(n % 5);
            wm->value._43 = 0.02f * static_cast<float>(tick * (n % 3));
        }
    });
}

void TestParticles(Checker& ck)
{
    Scene scene;
    BuildEmitters(scene);
    World& world = scene.GetWorld();
    CpuParticleBackend withJobs, serial;
    UpdateTimes times;
    const auto step = [&](CpuParticleBackend& backend, int tick, double& acc) {
        MoveEmitters(world, tick);
        TimeInto(acc, [&] { backend.Update(world, kDt); });
        SimSources src;
        src.particles = &backend;
        return HashWorld(world, src);
    };
    JobDelta delta;
    uint64_t lastHash = 0;
    const bool same = RunAB([&](int t) { return step(withJobs, t, times.jobsOnMs); },
                            [&](int t) { return step(serial, t, times.jobsOffMs); }, delta, lastHash);
    ck.Check(same, "CPU 粒子: jobs あり / なしで毎 tick のワールドハッシュが一致する");
    ck.Check(!withJobs.Pools().empty() && withJobs.Pools()[0].alive > 0 && withJobs.Stats().aliveTotal > 0,
             "CPU 粒子: テスト中に粒子が生きている");
    LogTimes("particles", times);
    MYE_LOG_INFO("  [jobs] particles: parallel batches %llu, chunks run by workers %llu",
                 static_cast<unsigned long long>(delta.batches), static_cast<unsigned long long>(delta.workerChunks));
    ck.Check(jobs::System().WorkerCount() == 0 || delta.batches > 0, "CPU 粒子: プールの更新がワーカーへ配られる");
}

// ---- Perception ----
struct PerceptionRig {
    Scene scene;
    PerceptionSystem perception;
    TransformSystem transforms;
    std::vector<SolidContact> contacts;
    std::vector<EntityID> targets;
};

void BuildPerception(PerceptionRig& rig)
{
    for (int i = 0; i < kObserverCount; ++i) {
        const float x = static_cast<float>(i % 8) * 3.0f - 12.0f;
        const float z = static_cast<float>(i / 8) * 3.0f - 7.5f;
        GameObject observer = rig.scene.CreateGameObjectTracked("Observer");
        observer.SetLocalPosition(x, 0.0f, z);
        observer.SetLocalRotationEuler(0.0f, static_cast<float>(i * 37 % 360), 0.0f);
        auto* p = observer.AddComponent<AIPerceptionComponent>();
        p->sightRadius = 14.0f;
        p->loseSightRadius = 16.0f;
        p->fovDeg = 120.0f;
        p->eyeHeight = 1.6f;
        p->autoSuccessRange = 0.5f;
        p->hearingEnabled = true;
        p->hearingRange = 20.0f;
        p->touchEnabled = (i % 4 == 0);

        GameObject target = rig.scene.CreateGameObjectTracked("Target");
        target.SetLocalPosition(x + 1.5f, 0.0f, z + 1.5f);
        auto* s = target.AddComponent<AIStimulusSourceComponent>();
        s->targetHeight = 1.6f;
        rig.targets.push_back(target.Id());
    }
    for (int i = 0; i < 20; ++i) {
        GameObject wall = rig.scene.CreateGameObjectTracked("Wall");
        wall.SetLocalPosition(static_cast<float>(i % 5) * 6.0f - 12.0f, 1.5f, static_cast<float>(i / 5) * 5.0f - 7.0f);
        auto* col = wall.AddComponent<ColliderComponent>();
        col->shape = collidershape::kBox;
        col->halfExtents = { 0.8f, 1.5f, 0.3f };
    }
    World& world = rig.scene.GetWorld();
    world.ApplyStructuralChanges();
    rig.transforms.Update(world);
}

uint64_t StepPerception(PerceptionRig& rig, int tick, double& acc)
{
    World& world = rig.scene.GetWorld();
    for (size_t i = 0; i < rig.targets.size(); ++i) {
        auto* t = world.GetComponent<LocalTransform>(rig.targets[i]);
        const float phase = static_cast<float>(tick) * 0.05f + static_cast<float>(i);
        t->position.x += 0.04f * std::sin(phase);
        t->position.z += 0.04f * std::cos(phase);
    }
    if (tick % 7 == 0 && !rig.targets.empty()) {
        const float at[3] = { 0.0f, 0.0f, 0.0f };
        PerceptionReportNoise(world, at, 1.0f, 40.0f, rig.targets[0]);
    }
    TimeInto(acc, [&] { rig.perception.Update(world, static_cast<uint64_t>(tick) + 1u, kDt, rig.contacts); });
    world.ApplyStructuralChanges();
    rig.transforms.Update(world);
    return HashWorld(world);
}

void TestPerception(Checker& ck)
{
    PerceptionRig a, b;
    BuildPerception(a);
    BuildPerception(b);
    JobDelta delta;
    uint64_t lastHash = 0;
    UpdateTimes times;
    const bool same = RunAB([&](int t) { return StepPerception(a, t, times.jobsOnMs); },
                            [&](int t) { return StepPerception(b, t, times.jobsOffMs); }, delta, lastHash);
    LogTimes("perception", times);
    ck.Check(same, "Perception: jobs あり / なしで毎 tick のワールドハッシュが一致する");
    ck.Check(a.perception.Stats().perceivers == kObserverCount && a.perception.Stats().losRays > 0
                 && a.perception.Stats().losRays == b.perception.Stats().losRays
                 && a.perception.Stats().losColliders == b.perception.Stats().losColliders,
             "Perception: 視線のレイ数とコライダー数の統計が jobs あり / なしで一致する");
    MYE_LOG_INFO("  [jobs] perception: parallel batches %llu, chunks run by workers %llu",
                 static_cast<unsigned long long>(delta.batches), static_cast<unsigned long long>(delta.workerChunks));
    ck.Check(jobs::System().WorkerCount() == 0 || delta.batches > 0, "Perception: 知覚者の更新がワーカーへ配られる");
}

// ---- TwoBoneIk + PartFollow ----
struct BodyRig {
    Scene scene;
    RenderResources resources;
    TwoBoneIkSystem ik;
    PartFollowSystem follow;
    TransformSystem transforms;
    std::vector<EntityID> bodies;
};

void BuildBodies(BodyRig& rig)
{
    // Hip (根) -> Knee -> Ankle -> Toe。クリップ無しのバインドポーズ (SkeletonSelfTest の脚と同じ)
    SkinnedModel leg;
    const char* names[] = { "Hip", "Knee", "Ankle", "Toe" };
    const XMFLOAT3 offsets[] = { { 0.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 0.2f } };
    for (int j = 0; j < 4; ++j) {
        SkeletonJoint joint;
        joint.parent = j - 1;
        joint.name = names[j];
        joint.bindT = offsets[j];
        leg.joints.push_back(joint);
    }
    const AssetID legId = rig.resources.skinnedModels.Register("selftest_sim_parallel_leg", SkinnedModel(leg));
    for (int i = 0; i < kBodyCount; ++i) {
        GameObject body = rig.scene.CreateGameObjectTracked("Body");
        body.SetLocalPosition(static_cast<float>(i) * 1.5f, 0.0f, 0.0f);
        body.AddComponent<SkinnedMeshComponent>()->model = legId;
        auto* ik = body.AddComponent<TwoBoneIKComponent>();
        std::memcpy(ik->chains[0].endJoint, "Ankle", sizeof("Ankle"));
        ik->chains[0].mode = twoboneikmode::kPosition;
        ik->chains[0].poleHint = { 0.0f, -1.0f, 1.0f };
        // 鎖 1 は名前違い: 警告は並列段の後に 1 回だけ出る
        if (i % 50 == 0) {
            ik->chains[1] = ik->chains[0];
            std::memcpy(ik->chains[1].endJoint, "NoSuchJoint", sizeof("NoSuchJoint"));
        }
        GameObject part = rig.scene.CreateGameObjectTracked("Toe");
        part.SetParent(body);
        auto* pc = part.AddComponent<PartComponent>();
        std::memcpy(pc->joint, "Toe", sizeof("Toe"));
        rig.bodies.push_back(body.Id());
    }
    World& world = rig.scene.GetWorld();
    world.ApplyStructuralChanges();
    rig.transforms.Update(world);
}

uint64_t StepBodies(BodyRig& rig, int tick, double& acc)
{
    World& world = rig.scene.GetWorld();
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < rig.bodies.size(); ++i) {
        auto* ik = world.GetComponent<TwoBoneIKComponent>(rig.bodies[i]);
        const float phase = static_cast<float>(tick) * 0.07f + static_cast<float>(i) * 0.3f;
        ik->chains[0].targetPosition = { 0.3f * std::sin(phase), -1.6f - 0.3f * std::cos(phase), 0.4f };
    }
    TimeInto(acc, [&] {
        rig.ik.Update(world, rig.resources);
        rig.follow.Update(world, rig.resources);
    });
    world.ApplyStructuralChanges();
    rig.transforms.Update(world);
    // SkinnedMesh は NoHash なので poseIk は自前で畳む (IK の出力そのもの)
    for (const EntityID body : rig.bodies) {
        const auto* sm = world.GetComponent<SkinnedMeshComponent>(body);
        h = Fnv(h, &sm->poseIkCount, sizeof(sm->poseIkCount));
        for (int32_t c = 0; c < sm->poseIkCount; ++c) {
            const SkinnedMeshComponent::PoseIkChain& chain = sm->poseIk[c];
            h = Fnv(h, &chain.endJoint, sizeof(chain.endJoint));
            h = Fnv(h, chain.target, sizeof(chain.target));
            h = Fnv(h, chain.rotation, sizeof(chain.rotation));
            h = Fnv(h, chain.pole, sizeof(chain.pole));
            h = Fnv(h, &chain.weight, sizeof(chain.weight));
        }
    }
    const uint64_t worldHash = HashWorld(world);
    return Fnv(h, &worldHash, sizeof(worldHash));
}

void TestBodies(Checker& ck)
{
    BodyRig a, b;
    BuildBodies(a);
    BuildBodies(b);
    JobDelta delta;
    uint64_t lastHash = 0;
    UpdateTimes times;
    const bool same = RunAB([&](int t) { return StepBodies(a, t, times.jobsOnMs); },
                            [&](int t) { return StepBodies(b, t, times.jobsOffMs); }, delta, lastHash);
    LogTimes("ik + part follow", times);
    ck.Check(same, "TwoBoneIk + PartFollow: jobs あり / なしで毎 tick の poseIk とワールドハッシュが一致する");
    const auto* sm = a.scene.GetWorld().GetComponent<SkinnedMeshComponent>(a.bodies[3]);
    ck.Check(sm != nullptr && sm->poseIkCount == 1, "TwoBoneIk: 名前違いの鎖は書かれず、有効な鎖だけが poseIk に入る");
    MYE_LOG_INFO("  [jobs] ik + part follow: parallel batches %llu, chunks run by workers %llu",
                 static_cast<unsigned long long>(delta.batches), static_cast<unsigned long long>(delta.workerChunks));
    ck.Check(jobs::System().WorkerCount() == 0 || delta.batches > 0,
             "TwoBoneIk + PartFollow: 更新がワーカーへ配られる");
}

} // namespace

bool RunSimParallelSelfTest()
{
    MYE_LOG_INFO("==== SimParallel self test ====");
    RegisterBuiltinComponents();
    Checker ck;
    // 他の起動経路が起こしたワーカーは止めない。この試験が起こした分だけ片付ける
    const bool startedHere = jobs::System().WorkerCount() == 0;
    jobs::System().Init();
    MYE_LOG_INFO("  workers = %d", jobs::System().WorkerCount());

    TestParticles(ck);
    TestPerception(ck);
    TestBodies(ck);

    if (startedHere) {
        jobs::System().Shutdown();
    }
    jobs::System().SetEnabled(true);
    return ck.failCount == 0;
}

} // namespace mye
