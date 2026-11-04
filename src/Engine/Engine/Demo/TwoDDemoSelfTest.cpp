//====================================================================================
//                          TwoDDemoSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          --2d-demo の 3000 tick 不変量と焼き込みハッシュの回帰テスト
//====================================================================================
#include "Engine/Engine/Demo/TwoDDemoSelfTest.h"

#include <process.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Archetype.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Script/EngineApiTable.h"
#include "Engine/Platform/Input.h"

namespace mye {
namespace {

constexpr uint64_t kTicks = 3000;
// 合成入力 (SynthLaneInput) で kTicks 回した HashWorld。Debug == Release (GameLogic.dll の ZTwoDHero / ZTwoDMover 込み)
constexpr uint64_t kExpectedHash = 0x1FD6DDE81A0FCA6Cull;
constexpr uint64_t kLogInterval = 500;
constexpr int kQueryRepeat = 200;

int sFailCount = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        MYE_LOG_INFO("  PASS: %s", what);
    } else {
        MYE_LOG_ERROR("  FAIL: %s", what);
        ++sFailCount;
    }
}

// plane2D の物 (Rigidbody 持ち) 1 個ぶんの初期値。z が動かないこと自体が不変量
struct PlaneBody {
    EntityID entity;
    float z0;
};

bool ReadField(HeadlessSim& sim, EntityID entity, const char* component, const char* field, uint64_t& out)
{
    ScriptApiContext apiCtx;
    apiCtx.scene = sim.Refs().scene;
    MyeEngineApi api = {};
    BuildEngineApi(api, &apiCtx);
    uint64_t buf = 0;
    const MyeEntityId id = { entity.index, entity.generation };
    if (api.GetComponentField(&apiCtx, id, HashStr(component), HashStr(field), &buf, sizeof(buf), nullptr) <= 0) {
        return false;
    }
    out = buf;
    return true;
}

// 剛体と CC の plane2D の不変量が崩れた物の数 (z が初期値のまま、z 速度と X / Y 軸まわりの角速度・姿勢が厳密に 0)
int CountViolations(World& world, const std::vector<PlaneBody>& bodies)
{
    int violations = 0;
    for (const PlaneBody& b : bodies) {
        const LocalTransform* lt = world.GetComponent<LocalTransform>(b.entity);
        if (lt == nullptr) {
            ++violations;
            continue;
        }
        bool ok = lt->position.z == b.z0 && lt->rotation.x == 0.0f && lt->rotation.y == 0.0f;
        if (const RigidbodyComponent* rb = world.GetComponent<RigidbodyComponent>(b.entity)) {
            ok = ok && rb->velocity.z == 0.0f && rb->angularVelocity.x == 0.0f && rb->angularVelocity.y == 0.0f;
        }
        if (!ok) {
            ++violations;
        }
    }
    return violations;
}

} // namespace

bool RunTwoDDemoSelfTest()
{
    MYE_LOG_INFO("==== TwoDDemo self test ====");
    sFailCount = 0;

    HeadlessSimSetup setup;
    setup.config.title = L"MyEngine TwoDDemoSelfTest";
    setup.saveDirOverride = (std::filesystem::temp_directory_path() / (L"mye_selftest_save_p" + std::to_wstring(_getpid()))).wstring(); // 同時実行の相手と save_server を共有しない
    setup.scene.showcase = FindShowcase(L"--2d-demo", /*editor=*/true);
    HeadlessSim sim;
    if (setup.scene.showcase == nullptr || !sim.Init(setup)) {
        Check(false, "ヘッドレス sim が --2d-demo で起動する (リポジトリのルートから実行すること)");
        return false;
    }
    sim.Activate();
    Scene& scene = *sim.Refs().scene;
    World& world = scene.GetWorld();

    const GameObject hero = scene.Find("Hero");
    const GameObject enemy = scene.Find("Enemy");
    const GameObject level = scene.Find("Level");
    Check(hero && enemy && level, "デモシーンに Hero / Enemy / Level がある");
    if (!hero || !enemy || !level) {
        return false;
    }

    // plane2D の剛体 (箱 3 + 移動床) と CC 2 体の初期値を集める
    std::vector<PlaneBody> bodies;
    int rigidCount = 0;
    const ComponentTypeId rigidReq[] = { RigidbodyComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(rigidReq, [&](Archetype& arch) {
        const int ri = arch.FindTypeIndex(RigidbodyComponent::sTypeId);
        const int li = arch.FindTypeIndex(LocalTransform::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* rb = static_cast<const RigidbodyComponent*>(arch.GetPtr(ri, row));
            if (rb->plane2D) {
                const auto* lt = static_cast<const LocalTransform*>(arch.GetPtr(li, row));
                bodies.push_back({ arch.EntityAt(row), lt->position.z });
                ++rigidCount;
            }
        }
    });
    Check(rigidCount == 4, "plane2D の剛体は 4 個 (箱 3 + 移動床)");
    for (const GameObject& cc : { hero, enemy }) {
        const LocalTransform* lt = world.GetComponent<LocalTransform>(cc.Id());
        bodies.push_back({ cc.Id(), lt != nullptr ? lt->position.z : 0.0f });
    }

    const ComponentTypeId heroScript = ComponentRegistry::Get().FindByName("ZTwoDHero");
    const bool scriptsLoaded = heroScript != kInvalidComponentType;
    if (!scriptsLoaded) {
        MYE_LOG_WARN("  SKIP: GameLogic.dll に ZTwoDHero が無い (GameLogic をビルドしていない)。主人公の動きとハッシュの照合は未実施");
    }

    const LocalTransform startHero = *world.GetComponent<LocalTransform>(hero.Id());
    const LocalTransform startEnemy = *world.GetComponent<LocalTransform>(enemy.Id());

    // ---- 3000 tick。毎 tick 不変量を見て、1 tick の所要時間を測る (測るだけで sim には入れない) ----
    int violatingTicks = 0;
    int firstViolationTick = -1;
    double totalMs = 0.0;
    double maxMs = 0.0;
    float heroTravel = 0.0f;
    float enemyTravel = 0.0f;
    for (uint64_t t = 0; t < kTicks; ++t) {
        InputSnapshot lanes[kMaxPlayers] = {};
        lanes[0] = SynthLaneInput(sim.TickIndex(), 0);
        const auto begin = std::chrono::steady_clock::now();
        sim.RunTick(lanes, nullptr, false);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        totalMs += ms;
        maxMs = (std::max)(maxMs, ms);
        if (CountViolations(world, bodies) != 0) {
            ++violatingTicks;
            if (firstViolationTick < 0) {
                firstViolationTick = static_cast<int>(t);
            }
        }
        heroTravel = (std::max)(heroTravel, std::fabs(world.GetComponent<LocalTransform>(hero.Id())->position.x - startHero.position.x));
        enemyTravel = (std::max)(enemyTravel, std::fabs(world.GetComponent<LocalTransform>(enemy.Id())->position.x - startEnemy.position.x));
        if ((t + 1) % kLogInterval == 0) {
            const LocalTransform* h = world.GetComponent<LocalTransform>(hero.Id());
            const LocalTransform* e = world.GetComponent<LocalTransform>(enemy.Id());
            uint64_t attacks = 0;
            ReadField(sim, hero.Id(), "ZTwoDHero", "attackCount", attacks);
            const GridAgent2DComponent* ga = world.GetComponent<GridAgent2DComponent>(enemy.Id());
            MYE_LOG_INFO("  [twod] tick %llu: hero (%.2f, %.2f) attacks %llu / enemy (%.2f, %.2f) status %d",
                         static_cast<unsigned long long>(t + 1), static_cast<double>(h->position.x),
                         static_cast<double>(h->position.y), static_cast<unsigned long long>(attacks),
                         static_cast<double>(e->position.x), static_cast<double>(e->position.y),
                         ga != nullptr ? ga->status : -1);
        }
    }
    if (firstViolationTick >= 0) {
        MYE_LOG_ERROR("  [twod] plane2D invariant broke first at tick %d (%d ticks)", firstViolationTick, violatingTicks);
    }
    Check(violatingTicks == 0, "3000 tick の間、全 plane2D の剛体と CC で z / vz / wx / wy / qx / qy が初期値 / 0.0f のまま");
    MYE_LOG_INFO("  [twod] sim %.3f ms/tick (max %.3f ms), hero travel %.2f m, enemy travel %.2f m",
                 totalMs / static_cast<double>(kTicks), maxMs, static_cast<double>(heroTravel),
                 static_cast<double>(enemyTravel));

    // ---- 供給口 (タイルの箱 + SpriteShape の辺) つきの空間クエリ 1 本の時間。tick に N 本撃つなら N 倍の見積り ----
    {
        const MyeVec3 origin = { 10.0f, 6.0f, 0.0f };
        const MyeVec3 down = { 0.0f, -1.0f, 0.0f };
        MyeRaycastHit hit = {};
        int hits = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < kQueryRepeat; ++i) {
            hits += RaycastWorld(world, origin, down, 20.0f, &hit);
        }
        const double rayUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count()
                             / kQueryRepeat;
        const auto boxBegin = std::chrono::steady_clock::now();
        for (int i = 0; i < kQueryRepeat; ++i) {
            hits += BoxCastWorld(world, origin, { 0.3f, 0.9f, 0.4f }, down, 20.0f, &hit);
        }
        const double boxUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - boxBegin).count()
                             / kQueryRepeat;
        std::vector<StaticShape> shapes;
        physicsquery::CollectStaticShapes(world, 0xFFFFFFFFu, shapes);
        MYE_LOG_INFO("  [twod] query cost: Raycast %.1f us, BoxCast %.1f us per call (Collect returns %zu static boxes)", rayUs,
                     boxUs, shapes.size());
        Check(hits == 2 * kQueryRepeat, "供給口つきの Raycast / BoxCast が床 (タイルの箱) に当たる");
    }

    // ---- 焼き込みハッシュと、スクリプトが実際に主人公・敵を動かしたこと ----
    const uint64_t hash = sim.WorldHash();
    MYE_LOG_INFO("  [twod] world hash after %llu ticks: 0x%016llX", static_cast<unsigned long long>(kTicks),
                 static_cast<unsigned long long>(hash));
    if (scriptsLoaded) {
        Check(heroTravel > 0.5f, "合成入力で主人公が動く (ZTwoDHero が入力を読んでいる)");
        Check(kExpectedHash == 0 || hash == kExpectedHash, "ワールドハッシュが焼き込み値と一致する (Debug == Release)");
    }

    MYE_LOG_INFO("TwoDDemo self test: %s", sFailCount == 0 ? "PASS" : "FAIL");
    return sFailCount == 0;
}

} // namespace mye
