#include "Engine/Engine/App/PerfBenchmark.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Physics/Rigid/Broadphase.h"
#include "Engine/Engine/Physics/Xpbd/XpbdBackend.h"
#include "Engine/Engine/Physics/Xpbd/XpbdSolver.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Renderer/Pipeline/PerfDrawBenchmark.h"

namespace mye {
namespace {

constexpr int kEntities = 10000;
constexpr int kWarmups = 3;
constexpr int kSamples = 9;
volatile uint64_t gBenchSink = 0;

struct Result {
    const char* name;
    double medianMs;
    double targetMs;
};

template <typename Fn>
double Measure(Fn&& fn)
{
    std::vector<double> samples;
    for (int i = -kWarmups; i < kSamples; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        if (!fn()) return -1.0;
        const auto end = std::chrono::steady_clock::now();
        if (i >= 0) samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void FillWorld(World& world)
{
    for (int i = 0; i < kEntities; ++i) {
        const EntityID entity = world.CreateEntity("perf");
        world.GetComponent<LocalTransform>(entity)->position.x = static_cast<float>(i);
    }
}

double BenchEcs(World& world)
{
    const ComponentTypeId required[] = { LocalTransform::sTypeId };
    return Measure([&] {
        uint32_t visited = 0;
        world.ForEachArchetype(required, [&](Archetype& archetype) {
            const int column = archetype.FindTypeIndex(LocalTransform::sTypeId);
            LocalTransform* transforms = archetype.ColumnData<LocalTransform>(column);
            for (uint32_t row = 0; row < archetype.Count(); ++row) {
                transforms[row].position.y += 0.001f;
                ++visited;
            }
        });
        gBenchSink = visited;
        return visited == kEntities;
    });
}

double BenchTransform(World& world)
{
    TransformSystem system;
    system.Update(world);
    return Measure([&] {
        const ComponentTypeId required[] = { LocalTransform::sTypeId };
        world.ForEachArchetype(required, [&](Archetype& archetype) {
            LocalTransform* transforms = archetype.ColumnData<LocalTransform>(
                archetype.FindTypeIndex(LocalTransform::sTypeId));
            for (uint32_t row = 0; row < archetype.Count(); ++row) transforms[row].position.z += 0.001f;
        });
        system.Update(world);
        gBenchSink = system.LastStats().computed;
        return system.LastStats().computed == kEntities;
    });
}

double BenchBroadphase()
{
    std::vector<BroadphaseEntry> entries(kEntities);
    for (uint32_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        e.id = i;
        e.minX = static_cast<float>(i % 100) * 4.0f;
        e.maxX = e.minX + 1.0f;
        e.minY = static_cast<float>(i / 100) * 4.0f;
        e.maxY = e.minY + 1.0f;
        e.minZ = 0.0f;
        e.maxZ = 1.0f;
    }
    std::vector<uint64_t> pairs;
    return Measure([&] {
        ComputeCandidatePairs(entries, pairs);
        gBenchSink = pairs.size();
        return pairs.empty();
    });
}

double BenchXpbd()
{
    XpbdBackend::Pool pool;
    pool.px.resize(kEntities);
    pool.py.resize(kEntities);
    pool.pz.resize(kEntities);
    pool.vx.resize(kEntities);
    pool.vy.resize(kEntities);
    pool.vz.resize(kEntities);
    pool.prevX.resize(kEntities);
    pool.prevY.resize(kEntities);
    pool.prevZ.resize(kEntities);
    pool.invMass.assign(kEntities, 1.0f);
    pool.ca.reserve(kEntities - 1);
    pool.cb.reserve(kEntities - 1);
    pool.rest.reserve(kEntities - 1);
    for (int i = 0; i < kEntities; ++i) {
        pool.px[i] = static_cast<float>(i) * 0.1f;
        if (i != 0) {
            pool.ca.push_back(i - 1);
            pool.cb.push_back(i);
            pool.rest.push_back(0.1f);
        }
    }
    pool.invMass[0] = 0.0f;
    std::vector<float> lambda;
    return Measure([&] {
        xpbd::Predict(pool, 0.0f, -9.8f, 0.0f, 1.0f / 60.0f, true, 0.01f);
        xpbd::Solve(pool, 0.0f, 1.0f / 60.0f, lambda);
        gBenchSink = lambda.size();
        return lambda.size() == kEntities - 1 && std::isfinite(pool.py.back());
    });
}

double BenchSnapshot(Scene& scene)
{
    SimRefs refs;
    refs.scene = &scene;
    InputSnapshot previousInputs[kMaxPlayers] = {};
    uint64_t audioHandleSeq = 0;
    uint64_t tickIndex = 0;
    refs.prevTickInput = previousInputs;
    refs.audioHandleSeq = &audioHandleSeq;
    refs.tickIndex = &tickIndex;
    std::vector<std::byte> blob;
    return Measure([&] {
        const bool ok = CaptureSimSnapshot(refs, blob);
        gBenchSink = blob.size();
        return ok && !blob.empty();
    });
}

double BenchHash(World& world)
{
    return Measure([&] {
        gBenchSink = HashWorld(world);
        return true;
    });
}

double BenchAssetDatabase(const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) return -1.0;
    for (int i = 0; i < kEntities; ++i) {
        const auto asset = root / (std::to_wstring(i) + L".mat.json");
        if (!std::filesystem::exists(asset)) {
            std::ofstream file(asset, std::ios::binary);
            if (!file) return -1.0;
            file << 'x';
        }
    }
    AssetDatabase db;
    db.ScanAndSync(root.wstring()); // Build .meta files outside the measured region.
    if (db.Count() != kEntities) return -1.0;
    return Measure([&] {
        db.ScanAndSync(root.wstring());
        gBenchSink = db.Count();
        return db.Count() == kEntities;
    });
}

} // namespace

int RunPerfBenchmark(const std::wstring& outputPath, const std::string& commitSha)
{
    Scene scene;
    World& world = scene.GetWorld();
    FillWorld(world);
    const std::filesystem::path output(outputPath);
    const std::filesystem::path assets = output.parent_path() / L"perf_assets_10k_material";
    const Result results[] = {
        { "ecs_update_10k", BenchEcs(world), 0.30 },
        { "transform_10k", BenchTransform(world), 0.40 },
        { "broadphase_10k", BenchBroadphase(), 1.00 },
        { "xpbd_particles_10k", BenchXpbd(), 2.00 },
        { "draw_submission_1000", RunDrawSubmissionBenchmark(kWarmups, kSamples), 0.80 },
        { "rollback_snapshot", BenchSnapshot(scene), 0.20 },
        { "replay_hash", BenchHash(world), 0.30 },
        { "asset_database_10k", BenchAssetDatabase(assets), 100.0 },
    };
    for (const Result& result : results) if (result.medianMs < 0.0 || !std::isfinite(result.medianMs)) return 1;
    std::ofstream file(output, std::ios::binary);
    if (!file) return 1;
    file << "{\n  \"schema\": 1,\n  \"commit\": \"" << commitSha
         << "\",\n  \"configuration\": \"Release-x64\",\n  \"warmups\": " << kWarmups
         << ",\n  \"samples\": " << kSamples
         << ",\n  \"fixture\": { \"entities\": 10000, \"broadphase\": \"100x100 sparse grid\", "
            "\"xpbd\": \"10000 particles, 9999 constraints, 8 iterations\", "
            "\"draw_calls\": 1000, \"draw_device\": \"D3D11 WARP\", "
            "\"assets\": 10000, \"asset_type\": \"material\", \"asset_meta\": \"warm\" },"
            "\n  \"metrics\": {\n";
    for (size_t i = 0; i < std::size(results); ++i) {
        const Result& result = results[i];
        file << "    \"" << result.name << "\": { \"median_ms\": " << std::fixed
             << std::setprecision(6) << result.medianMs << ", \"target_ms\": "
             << result.targetMs << " }" << (i + 1 == std::size(results) ? "\n" : ",\n");
    }
    file << "  }\n}\n";
    return file.good() ? 0 : 1;
}

} // namespace mye
