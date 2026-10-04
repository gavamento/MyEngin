//====================================================================================
//                          NavSystem.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュの実行時の持ち主の実装
//====================================================================================
#include "Engine/Engine/Navigation/NavSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "DetourCommon.h"
#include "Engine/Core/Asset/AssetGuidResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

void NavQueryDeleter::operator()(dtNavMeshQuery* query) const
{
    dtFreeNavMeshQuery(query);
}

void NavCrowdDeleter::operator()(dtCrowd* crowd) const
{
    dtFreeCrowd(crowd);
}

int NavSurfaceRuntime::OccupiedSlots() const
{
    int count = 0;
    for (const NavAgentSlot& slot : slots) {
        if (slot.entity != kNullEntity) {
            ++count;
        }
    }
    return count;
}

namespace {

// 経路線を床と Z ファイトさせないための持ち上げ (m)
constexpr float kPathLift = 0.03f;
// 0xRRGGBBAA (DebugLineCmd と同じ並び)
constexpr uint32_t kPathMovingColor = 0x60FF60FFu;
constexpr uint32_t kPathArrivedColor = 0x80C0FFFFu;
constexpr uint32_t kPathNoPathColor = 0xFF4040FFu;

// dtCrowd の容量 (Surface ごと)。超えた Agent はエンティティキーの後ろから Inactive
constexpr int kCrowdCapacity = 128;
// dtCrowd の近傍格子の大きさの目安。これより大きい半径の Agent も動くが格子の効率が落ちる
constexpr float kCrowdMaxAgentRadius = 0.6f;
constexpr int kQueryMaxNodes = 2048;
constexpr int kAvoidanceTypeCount = 4;
// CC の実位置が crowd の位置からこれ以上離れたら、歩いた結果ではなく瞬間移動として置き直す
constexpr float kTeleportDistance = 2.0f;
// 到着判定の下限。dtCrowd は終点で速度が 0 へ漸近するので、stoppingDistance が 0 でも着地できるようにする
constexpr float kArriveEpsilon = 0.05f;
// 回転を始める最低速度 (m/s)
constexpr float kMinTurnSpeed = 0.05f;
constexpr float kPi = 3.14159265358979f;
constexpr float kNoAvoidanceQueryRange = 0.01f;
// Agent を crowd に載せる / 目的地を探す近傍 (半径の倍率・固定の高さ幅、m)
constexpr float kPlaceHorizontalScale = 2.0f;
constexpr float kPlaceHorizontalMin = 0.6f;
constexpr float kPlaceVertical = 1.0f;
constexpr float kDestHorizontal = 1.0f;
constexpr float kDestVertical = 2.0f;

constexpr uint32_t kSnapshotMagic = 0x3156414Eu; // 'NAV1'
constexpr uint32_t kMaxSnapshotSurfaces = 4096;

// Agent を動かせない理由 (0 = 動かせる)
enum InactiveReason : int {
    kReasonNone = 0,
    kReasonNoController,    // CharacterController が無い
    kReasonRigidbody,       // Rigidbody があり CharacterController が無効
    kReasonNoSurface,       // agentTypeId が合う Surface が無い
    kReasonSurfaceNotReady, // Surface はあるが未ベイク / 読み込み失敗
    kReasonCapacity,        // crowd の容量超過
};

const char* ReasonText(int reason)
{
    switch (reason) {
    case kReasonNoController: return "no CharacterController";
    case kReasonRigidbody: return "a Rigidbody disables the CharacterController";
    case kReasonNoSurface: return "no NavMeshSurface with the same agent type id";
    case kReasonSurfaceNotReady: return "the NavMeshSurface has no loaded navigation mesh";
    case kReasonCapacity: return "the crowd of the surface is full";
    default: return "unknown";
    }
}

void AddLine(std::vector<DebugLineCmd>& out, const float* a, const float* b, uint32_t rgba)
{
    DebugLineCmd cmd;
    cmd.ax = a[0];
    cmd.ay = a[1] + kPathLift;
    cmd.az = a[2];
    cmd.bx = b[0];
    cmd.by = b[1] + kPathLift;
    cmd.bz = b[2];
    cmd.rgba = rgba;
    out.push_back(cmd);
}

bool KeyLess(const EntityID& a, const EntityID& b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// 回避の品質 (Recast の RecastDemo の 4 段階)。Agent の avoidanceQuality 1..3 がそのまま添字になる
dtObstacleAvoidanceParams AvoidanceParams(int level)
{
    dtObstacleAvoidanceParams p;
    std::memset(&p, 0, sizeof(p));
    p.velBias = 0.4f;
    p.weightDesVel = 2.0f;
    p.weightCurVel = 0.75f;
    p.weightSide = 0.75f;
    p.weightToi = 2.5f;
    p.horizTime = 2.5f;
    p.gridSize = 33;
    p.adaptiveDivs = 7;
    p.adaptiveRings = 2;
    p.adaptiveDepth = 5;
    if (level == 0) {
        p.adaptiveDivs = 5;
        p.adaptiveRings = 2;
        p.adaptiveDepth = 1;
    } else if (level == 1) {
        p.adaptiveDivs = 5;
        p.adaptiveRings = 2;
        p.adaptiveDepth = 2;
    } else if (level == 2) {
        p.adaptiveDivs = 7;
        p.adaptiveRings = 2;
        p.adaptiveDepth = 3;
    }
    return p;
}

float Length3(float x, float y, float z)
{
    return std::sqrt(x * x + y * y + z * z);
}

// CC の足元の高さ (エンティティ原点 = カプセルの中心から足までの距離)。PhysicsSystem::SolveCharacters の寸法規約と同じ
float CapsuleHalfHeight(const CharacterControllerComponent& cc, float scaleX, float scaleY, float scaleZ)
{
    const float radius = cc.radius * (std::max)(std::fabs(scaleX), std::fabs(scaleZ));
    const float half = cc.height * 0.5f * std::fabs(scaleY);
    return (std::max)(half, radius);
}

// y 軸まわりの向き (ラジアン)。+Z を向いた状態が 0
float YawOf(const DirectX::XMFLOAT4& q)
{
    const float fx = 2.0f * (q.x * q.z + q.w * q.y);
    const float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    if (fx == 0.0f && fz == 0.0f) {
        return 0.0f;
    }
    return std::atan2(fx, fz);
}

float WrapPi(float a)
{
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

// 構造体の浮動小数をビットで比べる (-0.0 と 0.0 も区別 = 目的地が書き換わったかの判定)
bool SameBits(const float* a, const float* b)
{
    return std::memcmp(a, b, sizeof(float) * 3) == 0;
}

// ---- Nav 節の書式 ----

struct SnapshotSurface {
    EntityID entity;
    uint64_t guid = 0;
    std::vector<uint8_t> store;
    std::vector<uint8_t> crowd;
    std::vector<NavAgentSlot> slots;
};

bool ReadBlock(NavByteReader& r, std::vector<uint8_t>& out)
{
    uint32_t size = 0;
    if (!r.Pod(size) || size > r.Remaining()) {
        return false;
    }
    out.resize(size);
    return size == 0 || r.Bytes(out.data(), size);
}

bool ParseSnapshot(const uint8_t* data, size_t size, std::vector<SnapshotSurface>& out)
{
    NavByteReader r(data, size);
    uint32_t magic = 0;
    uint32_t count = 0;
    if (!r.Pod(magic) || !r.Pod(count) || magic != kSnapshotMagic || count > kMaxSnapshotSurfaces) {
        return false;
    }
    out.clear();
    out.resize(count);
    for (SnapshotSurface& s : out) {
        uint32_t occupied = 0;
        if (!r.Pod(s.entity.index) || !r.Pod(s.entity.generation) || !r.Pod(s.guid) || !ReadBlock(r, s.store)
            || !ReadBlock(r, s.crowd) || !r.Pod(occupied) || occupied > static_cast<uint32_t>(kCrowdCapacity)) {
            return false;
        }
        s.slots.assign(kCrowdCapacity, NavAgentSlot{});
        int64_t previous = -1;
        for (uint32_t k = 0; k < occupied; ++k) {
            uint32_t index = 0;
            NavAgentSlot slot;
            if (!r.Pod(index) || index >= static_cast<uint32_t>(kCrowdCapacity) || static_cast<int64_t>(index) <= previous
                || !r.Pod(slot.entity.index) || !r.Pod(slot.entity.generation) || !r.Pod(slot.requested)
                || !r.Pod(slot.destInvalid) || !r.Pod(slot.arrived) || !r.Bytes(slot.requestedDest, sizeof(slot.requestedDest))) {
                return false;
            }
            previous = index;
            s.slots[index] = slot;
        }
    }
    return r.Ok() && r.Remaining() == 0;
}

// 載っているスロットだけを番号つきで書く (128 スロットの空きを毎 tick 撮らない)
void WriteSlots(NavByteWriter& w, const std::vector<NavAgentSlot>& slots)
{
    uint32_t occupied = 0;
    for (const NavAgentSlot& slot : slots) {
        occupied += slot.entity != kNullEntity ? 1u : 0u;
    }
    w.Pod(occupied);
    for (size_t i = 0; i < slots.size(); ++i) {
        const NavAgentSlot& slot = slots[i];
        if (slot.entity == kNullEntity) {
            continue;
        }
        w.Pod(static_cast<uint32_t>(i));
        w.Pod(slot.entity.index);
        w.Pod(slot.entity.generation);
        w.Pod(slot.requested);
        w.Pod(slot.destInvalid);
        w.Pod(slot.arrived);
        w.Bytes(slot.requestedDest, sizeof(slot.requestedDest));
    }
}

void WriteBlock(NavByteWriter& w, const NavByteWriter& block)
{
    w.Pod(static_cast<uint32_t>(block.Size()));
    w.Bytes(block.Data().data(), block.Size());
}

} // namespace

void NavSystem::Load(NavSurfaceRuntime& surface, const char* name)
{
    NavMeshAsset::Data data;
    if (!NavMeshAsset::LoadByGuid(surface.assetGuid, data)) {
        surface.state = NavSurfaceState::Failed;
        MYE_LOG_ERROR("[nav] surface '%s': failed to load the .mnav asset (guid %016llx)", name,
                      static_cast<unsigned long long>(surface.assetGuid));
        return;
    }
    auto store = std::make_unique<NavTileStore>();
    if (!NavMeshAsset::BuildStore(data, *store)) {
        surface.state = NavSurfaceState::Failed;
        MYE_LOG_ERROR("[nav] surface '%s': failed to build the navigation mesh from guid %016llx", name,
                      static_cast<unsigned long long>(surface.assetGuid));
        return;
    }

    dtNavMesh* nav = store->NavMesh();
    const dtNavMesh* navView = nav; // getTile の const 版 (非 const 版は private)
    std::unique_ptr<dtNavMeshQuery, NavQueryDeleter> query(dtAllocNavMeshQuery());
    std::unique_ptr<dtCrowd, NavCrowdDeleter> crowd(dtAllocCrowd());
    if (!query || dtStatusFailed(query->init(nav, kQueryMaxNodes)) || !crowd
        || !crowd->init(kCrowdCapacity, kCrowdMaxAgentRadius, nav)) {
        surface.state = NavSurfaceState::Failed;
        MYE_LOG_ERROR("[nav] surface '%s': failed to create the query / crowd", name);
        return;
    }
    for (int i = 0; i < kAvoidanceTypeCount; ++i) {
        const dtObstacleAvoidanceParams params = AvoidanceParams(i);
        crowd->setObstacleAvoidanceParams(i, &params);
    }

    surface.layerCount = store->LayerCount();
    surface.tileCount = 0;
    surface.polyCount = 0;
    for (int i = 0; i < navView->getMaxTiles(); ++i) {
        const dtMeshTile* tile = navView->getTile(i);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        ++surface.tileCount;
        surface.polyCount += tile->header->polyCount;
    }

    surface.store = std::move(store);
    surface.query = std::move(query);
    surface.crowd = std::move(crowd);
    surface.slots.assign(kCrowdCapacity, NavAgentSlot{});
    surface.state = NavSurfaceState::Loaded;
    MYE_LOG_INFO("[nav] surface '%s' loaded: %d tiles, %d layers, %d polygons", name, surface.tileCount,
                 surface.layerCount, surface.polyCount);
}

void NavSystem::ScanSurfaceKeys(World& world)
{
    scanKeys_.clear();
    const ComponentTypeId req[] = { NavMeshSurfaceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(NavMeshSurfaceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* surface = static_cast<const NavMeshSurfaceComponent*>(arch.GetPtr(si, row));
            scanKeys_.push_back({ e, surface->navAsset.value });
        }
    });
    // アーキタイプの列挙順は生成順に依るので、エンティティキー順に並べる
    std::sort(scanKeys_.begin(), scanKeys_.end(),
              [](const Key& a, const Key& b) { return KeyLess(a.entity, b.entity); });
}

bool NavSystem::SurfaceKeysChanged() const
{
    return scanKeys_.size() != loadedKeys_.size()
        || !std::equal(scanKeys_.begin(), scanKeys_.end(), loadedKeys_.begin(),
                       [](const Key& a, const Key& b) { return a.entity == b.entity && a.assetGuid == b.assetGuid; });
}

void NavSystem::SyncSurfaces(World& world)
{
    ScanSurfaceKeys(world);
    if (!SurfaceKeysChanged()) {
        return;
    }

    surfaces_.clear();
    surfaces_.reserve(scanKeys_.size());
    for (const Key& key : scanKeys_) {
        NavSurfaceRuntime& surface = surfaces_.emplace_back();
        surface.entity = key.entity;
        surface.assetGuid = key.assetGuid;
        if (key.assetGuid != 0) {
            Load(surface, world.GetName(key.entity));
        }
    }
    loadedKeys_ = scanKeys_;

    // 同じ agentTypeId の Surface が複数あるとエンティティキーの小さい方が勝つ (Agent の割り当てと同じ規則)
    for (size_t i = 0; i < surfaces_.size(); ++i) {
        const auto* a = world.GetComponent<NavMeshSurfaceComponent>(surfaces_[i].entity);
        for (size_t j = i + 1; a != nullptr && j < surfaces_.size(); ++j) {
            const auto* b = world.GetComponent<NavMeshSurfaceComponent>(surfaces_[j].entity);
            if (b != nullptr && a->agentTypeId == b->agentTypeId) {
                MYE_LOG_WARN("[nav] surfaces '%s' and '%s' share agent type id %d; agents use the first one",
                             world.GetName(surfaces_[i].entity), world.GetName(surfaces_[j].entity),
                             static_cast<int>(a->agentTypeId));
            }
        }
    }
}

void NavSystem::CollectAgents(World& world)
{
    agents_.clear();
    const ComponentTypeId req[] = { NavMeshAgentComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ai = arch.FindTypeIndex(NavMeshAgentComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            AgentRef& ref = agents_.emplace_back();
            ref.entity = e;
            ref.agent = static_cast<NavMeshAgentComponent*>(arch.GetPtr(ai, row));
        }
    });
    // アーキタイプの列挙順は生成順に依るので、エンティティキー順に並べる
    std::sort(agents_.begin(), agents_.end(),
              [](const AgentRef& a, const AgentRef& b) { return KeyLess(a.entity, b.entity); });
    for (AgentRef& a : agents_) {
        a.cc = world.GetComponent<CharacterControllerComponent>(a.entity);
        a.transform = world.GetComponent<LocalTransform>(a.entity);
        a.rooted = world.GetParent(a.entity) == kNullEntity;
    }
}

void NavSystem::Update(World& world, float dt)
{
    SyncSurfaces(world);
    CollectAgents(world);
    bool anyOccupied = false;
    for (const NavSurfaceRuntime& surface : surfaces_) {
        anyOccupied = anyOccupied || surface.OccupiedSlots() > 0;
    }
    if (agents_.empty() && !anyOccupied) {
        stats_.agents = 0;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    stats_.crowdUpdateUs = 0.0;

    // ---- 乗る Surface と、動かせない理由 ----
    wantedPerSurface_.assign(surfaces_.size(), {});
    for (size_t i = 0; i < agents_.size(); ++i) {
        AgentRef& a = agents_[i];
        if (a.cc == nullptr || a.transform == nullptr) {
            a.inactiveReason = kReasonNoController;
        } else if (world.GetComponent<RigidbodyComponent>(a.entity) != nullptr) {
            a.inactiveReason = kReasonRigidbody; // Rigidbody があると CC は無効 (Components.h)
        } else {
            a.inactiveReason = kReasonNoSurface;
            for (size_t s = 0; s < surfaces_.size(); ++s) {
                const auto* sc = world.GetComponent<NavMeshSurfaceComponent>(surfaces_[s].entity);
                if (sc == nullptr || sc->agentTypeId != a.agent->agentTypeId) {
                    continue;
                }
                a.surface = static_cast<int>(s);
                a.inactiveReason = surfaces_[s].state == NavSurfaceState::Loaded ? kReasonNone : kReasonSurfaceNotReady;
                break;
            }
        }
        if (a.inactiveReason == kReasonNone) {
            std::vector<int>& wanted = wantedPerSurface_[static_cast<size_t>(a.surface)];
            if (wanted.size() >= static_cast<size_t>(kCrowdCapacity)) {
                a.inactiveReason = kReasonCapacity; // キー順の後ろから溢れる
            } else {
                wanted.push_back(static_cast<int>(i));
            }
        }
    }

    for (size_t s = 0; s < surfaces_.size(); ++s) {
        UpdateSurface(world, s, dt);
    }

    // ---- 動かせない Agent ----
    for (AgentRef& a : agents_) {
        if (a.inactiveReason == kReasonNone) {
            continue;
        }
        NavMeshAgentComponent& agent = *a.agent;
        if (agent.status != navagentstatus::kInactive) {
            MYE_LOG_WARN("[nav] agent '%s' is inactive: %s", world.GetName(a.entity), ReasonText(a.inactiveReason));
            if (a.cc != nullptr) {
                a.cc->moveInput = { 0.0f, 0.0f, 0.0f }; // 歩きかけのまま保持される入力を止める
            }
        }
        agent.status = navagentstatus::kInactive;
        agent.remainingDistance = 0.0f;
        agent.pathPartial = false;
    }

    int onCrowd = 0;
    for (const NavSurfaceRuntime& surface : surfaces_) {
        onCrowd += surface.OccupiedSlots();
    }
    stats_.agents = onCrowd;
    stats_.updateUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    stats_.maxCrowdUpdateUs = (std::max)(stats_.maxCrowdUpdateUs, stats_.crowdUpdateUs);
}

void NavSystem::UpdateSurface(World& world, size_t surfaceIndex, float dt)
{
    NavSurfaceRuntime& surface = surfaces_[surfaceIndex];
    const std::vector<int>& wanted = wantedPerSurface_[surfaceIndex];
    if (surface.state != NavSurfaceState::Loaded || (wanted.empty() && surface.OccupiedSlots() == 0)) {
        return;
    }
    dtCrowd& crowd = *surface.crowd;
    dtNavMeshQuery& query = *surface.query;

    // エリアのコスト (Surface のコンポーネントから毎 tick 写す。インスペクタでの変更がそのまま効く)
    dtQueryFilter* filter = crowd.getEditableFilter(0);
    if (const auto* sc = world.GetComponent<NavMeshSurfaceComponent>(surface.entity)) {
        for (int i = 0; i < kNavAreaCount; ++i) {
            filter->setAreaCost(i, (std::max)(1.0f, sc->areaCosts[i]));
        }
    }

    // ---- 外れた Agent をスロットから外す (スロット番号の昇順) ----
    for (int slotIndex = 0; slotIndex < kCrowdCapacity; ++slotIndex) {
        NavAgentSlot& slot = surface.slots[static_cast<size_t>(slotIndex)];
        if (slot.entity == kNullEntity) {
            continue;
        }
        const bool stillWanted = std::any_of(wanted.begin(), wanted.end(), [&](int idx) {
            return agents_[static_cast<size_t>(idx)].entity == slot.entity;
        });
        if (!stillWanted) {
            crowd.removeAgent(slotIndex);
            slot = NavAgentSlot{};
        }
    }

    // ---- 載せる (キー順。空きスロットは常に最小番号から埋まる) ----
    for (const int idx : wanted) {
        AgentRef& a = agents_[static_cast<size_t>(idx)];
        NavMeshAgentComponent& agent = *a.agent;

        // CC の足元 (ワールド)。親が無ければ CC と同じく LocalTransform から、あれば前 tick の WorldMatrix から
        float sx = 1.0f;
        float sy = 1.0f;
        float sz = 1.0f;
        if (a.rooted) {
            a.feet[0] = a.transform->position.x;
            a.feet[1] = a.transform->position.y;
            a.feet[2] = a.transform->position.z;
            sx = a.transform->scale.x;
            sy = a.transform->scale.y;
            sz = a.transform->scale.z;
        } else if (const auto* wm = world.GetComponent<WorldMatrixComponent>(a.entity)) {
            const auto& m = wm->value.m;
            a.feet[0] = m[3][0];
            a.feet[1] = m[3][1];
            a.feet[2] = m[3][2];
            sx = Length3(m[0][0], m[0][1], m[0][2]);
            sy = Length3(m[1][0], m[1][1], m[1][2]);
            sz = Length3(m[2][0], m[2][1], m[2][2]);
        }
        a.feet[1] -= CapsuleHalfHeight(*a.cc, sx, sy, sz);

        // スロットの探索 (無ければ載せる)
        int slotIndex = -1;
        for (int k = 0; k < kCrowdCapacity; ++k) {
            if (surface.slots[static_cast<size_t>(k)].entity == a.entity) {
                slotIndex = k;
                break;
            }
        }
        const float placeH = (std::max)(kPlaceHorizontalScale * agent.radius, kPlaceHorizontalMin);
        const float placeExt[3] = { placeH, kPlaceVertical, placeH };
        if (slotIndex < 0) {
            dtPolyRef ref = 0;
            float nearest[3] = {};
            query.findNearestPoly(a.feet, placeExt, filter, &ref, nearest);
            if (ref == 0) {
                // ナビメッシュの外。載せずに毎 tick 探す (動かしたら次の tick で載る)
                agent.status = agent.hasDestination ? navagentstatus::kNoPath : navagentstatus::kIdle;
                agent.remainingDistance = 0.0f;
                agent.pathPartial = false;
                a.slot = -1;
                continue;
            }
            dtCrowdAgentParams params;
            std::memset(&params, 0, sizeof(params));
            params.radius = (std::max)(0.01f, agent.radius);
            params.height = (std::max)(0.01f, agent.height);
            params.maxAcceleration = (std::max)(0.01f, agent.acceleration);
            params.maxSpeed = (std::max)(0.0f, agent.speed);
            slotIndex = crowd.addAgent(nearest, &params);
            if (slotIndex < 0) {
                a.inactiveReason = kReasonCapacity;
                continue;
            }
            surface.slots[static_cast<size_t>(slotIndex)] = NavAgentSlot{};
            surface.slots[static_cast<size_t>(slotIndex)].entity = a.entity;
        }
        a.slot = slotIndex;
    }

    // ---- crowd と ECS の同期 ----
    for (const int idx : wanted) {
        AgentRef& a = agents_[static_cast<size_t>(idx)];
        if (a.slot < 0 || a.inactiveReason != kReasonNone) {
            continue;
        }
        NavMeshAgentComponent& agent = *a.agent;
        NavAgentSlot& slot = surface.slots[static_cast<size_t>(a.slot)];
        dtCrowdAgent* ag = crowd.getEditableAgent(a.slot);

        // 設定は毎 tick 写す (インスペクタやスクリプトの変更がそのまま効く)
        const int quality = (std::min)((std::max)(agent.avoidanceQuality, 0), 3);
        dtCrowdAgentParams params = ag->params;
        params.radius = (std::max)(0.01f, agent.radius);
        params.height = (std::max)(0.01f, agent.height);
        params.maxAcceleration = (std::max)(0.01f, agent.acceleration);
        params.maxSpeed = (std::max)(0.0f, agent.speed);
        // 回避なし (0) は近傍を探さない: dtCrowd の衝突解決 (押し戻し) は DT_CROWD_SEPARATION と無関係に近傍全員へ働くので、
        // 範囲を潰さないと「すり抜ける」にならない
        params.collisionQueryRange = quality > 0 ? params.radius * 12.0f : kNoAvoidanceQueryRange;
        params.pathOptimizationRange = params.radius * 30.0f;
        params.separationWeight = 2.0f;
        params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
        if (quality > 0) {
            params.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        }
        params.obstacleAvoidanceType = static_cast<unsigned char>(quality);
        params.queryFilterType = 0;
        crowd.updateAgentParameters(a.slot, &params);

        // CC の実位置を crowd へ書き戻す。歩いた分は corridor を動かし、瞬間移動 / 無効状態からの復帰は置き直す
        const float dx = a.feet[0] - ag->npos[0];
        const float dz = a.feet[2] - ag->npos[2];
        if (ag->state == DT_CROWDAGENT_STATE_INVALID || dx * dx + dz * dz > kTeleportDistance * kTeleportDistance) {
            const float placeH = (std::max)(kPlaceHorizontalScale * agent.radius, kPlaceHorizontalMin);
            const float placeExt[3] = { placeH, kPlaceVertical, placeH };
            dtPolyRef ref = 0;
            float nearest[3] = {};
            query.findNearestPoly(a.feet, placeExt, filter, &ref, nearest);
            if (ref != 0) {
                ag->corridor.reset(ref, nearest);
                dtVcopy(ag->npos, nearest);
                dtVset(ag->vel, 0.0f, 0.0f, 0.0f);
                dtVset(ag->dvel, 0.0f, 0.0f, 0.0f);
                dtVset(ag->nvel, 0.0f, 0.0f, 0.0f);
                dtVset(ag->disp, 0.0f, 0.0f, 0.0f);
                ag->boundary.reset();
                ag->nneis = 0;
                ag->ncorners = 0;
                ag->partial = false;
                ag->state = DT_CROWDAGENT_STATE_WALKING;
                crowd.resetMoveTarget(a.slot);
                slot.requested = 0;
                slot.arrived = 0;
                slot.destInvalid = 0;
            } else {
                ag->state = DT_CROWDAGENT_STATE_INVALID;
            }
        } else if (ag->state == DT_CROWDAGENT_STATE_WALKING) {
            ag->corridor.movePosition(a.feet, &query, filter);
            dtVcopy(ag->npos, ag->corridor.getPos());
        }
        dtVcopy(a.synced, ag->npos);

        // 目的地。変わったときだけ要求する (同じ値の再要求は経路を引き直してしまう)
        if (!agent.hasDestination) {
            if (slot.requested != 0 || ag->targetState != DT_CROWDAGENT_TARGET_NONE) {
                crowd.resetMoveTarget(a.slot);
            }
            slot.requested = 0;
            slot.destInvalid = 0;
            slot.arrived = 0;
        } else {
            const float dest[3] = { agent.destination.x, agent.destination.y, agent.destination.z };
            if (slot.requested == 0 || !SameBits(dest, slot.requestedDest)) {
                slot.requested = 1;
                slot.destInvalid = 0;
                slot.arrived = 0;
                std::memcpy(slot.requestedDest, dest, sizeof(dest));
                const float destExt[3] = { kDestHorizontal, kDestVertical, kDestHorizontal };
                dtPolyRef ref = 0;
                float nearest[3] = {};
                query.findNearestPoly(dest, destExt, filter, &ref, nearest);
                if (ref == 0) {
                    slot.destInvalid = 1;
                    crowd.resetMoveTarget(a.slot);
                } else {
                    crowd.requestMoveTarget(a.slot, ref, nearest);
                }
            }
        }
    }

    // ---- 進める ----
    const auto t0 = std::chrono::steady_clock::now();
    crowd.update(dt, nullptr);
    stats_.crowdUpdateUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    // 完了した経路要求の控え番号は dtPathQueue (private) の連番で、WAITING_FOR_PATH の間しか読まれない死んだ値。
    // 復元した crowd は連番が 1 から数え直しになるので、残すと連続実行と復元後でバイト列 / ハッシュが割れる
    for (int slotIndex = 0; slotIndex < kCrowdCapacity; ++slotIndex) {
        if (surface.slots[static_cast<size_t>(slotIndex)].entity != kNullEntity) {
            crowd.getEditableAgent(slotIndex)->targetPathqRef = DT_PATHQ_INVALID;
        }
    }

    // ---- 結果を ECS へ ----
    const float invDt = dt > 0.0f ? 1.0f / dt : 0.0f;
    for (const int idx : wanted) {
        AgentRef& a = agents_[static_cast<size_t>(idx)];
        if (a.slot < 0 || a.inactiveReason != kReasonNone) {
            continue;
        }
        NavMeshAgentComponent& agent = *a.agent;
        NavAgentSlot& slot = surface.slots[static_cast<size_t>(a.slot)];
        const dtCrowdAgent* ag = crowd.getAgent(a.slot);

        // 状態
        int status = navagentstatus::kIdle;
        float remaining = 0.0f;
        bool partial = false;
        if (!agent.hasDestination) {
            status = navagentstatus::kIdle;
        } else if (slot.destInvalid != 0 || ag->state == DT_CROWDAGENT_STATE_INVALID) {
            status = navagentstatus::kNoPath;
        } else if (slot.arrived != 0) {
            status = navagentstatus::kArrived;
            partial = agent.pathPartial;
        } else if (ag->targetState == DT_CROWDAGENT_TARGET_FAILED || ag->targetState == DT_CROWDAGENT_TARGET_NONE) {
            status = navagentstatus::kNoPath; // 経路探索の失敗 (NONE = 要求が受理されなかった)
        } else {
            partial = ag->partial;
            bool endReached = false;
            float toEnd = 0.0f;
            if (ag->ncorners > 0) {
                const int last = ag->ncorners - 1;
                endReached = (ag->cornerFlags[last] & DT_STRAIGHTPATH_END) != 0;
                toEnd = dtVdist2D(ag->npos, &ag->cornerVerts[last * 3]);
                // 経路に沿った残りの見積り: 見えている角を順にたどる。終点が角の外なら corridor の目標までを足す
                remaining = dtVdist(ag->npos, &ag->cornerVerts[0]);
                for (int k = 1; k < ag->ncorners; ++k) {
                    remaining += dtVdist(&ag->cornerVerts[(k - 1) * 3], &ag->cornerVerts[k * 3]);
                }
                if (!endReached) {
                    remaining += dtVdist(&ag->cornerVerts[last * 3], ag->corridor.getTarget());
                }
            } else {
                remaining = dtVdist(ag->npos, ag->corridor.getTarget());
                toEnd = dtVdist2D(ag->npos, ag->corridor.getTarget());
            }
            const float arriveDistance = (std::max)(agent.stoppingDistance, kArriveEpsilon);
            if (ag->targetState == DT_CROWDAGENT_TARGET_VALID && endReached && toEnd <= arriveDistance) {
                slot.arrived = 1;
                crowd.resetMoveTarget(a.slot);
                status = navagentstatus::kArrived;
            } else {
                status = navagentstatus::kMoving;
            }
        }
        agent.status = status;
        agent.remainingDistance = remaining;
        agent.pathPartial = partial;

        // 移動入力。crowd が今 tick に進めた変位 (速度の積分 + 衝突の押し戻し) をそのまま CC に歩かせる
        const float mx = (ag->npos[0] - a.synced[0]) * invDt;
        const float mz = (ag->npos[2] - a.synced[2]) * invDt;
        a.cc->moveInput = { mx, 0.0f, mz };

        // 進行方向へ向ける (親が無いときだけ。親付きの向きは親の回転と合成されるので触らない)
        const float speed2 = mx * mx + mz * mz;
        if (a.rooted && agent.angularSpeedDeg > 0.0f && speed2 > kMinTurnSpeed * kMinTurnSpeed) {
            const float current = YawOf(a.transform->rotation);
            const float target = std::atan2(mx, mz);
            const float maxStep = agent.angularSpeedDeg * (kPi / 180.0f) * dt;
            const float delta = (std::max)(-maxStep, (std::min)(maxStep, WrapPi(target - current)));
            const float yaw = current + delta;
            a.transform->rotation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        }
    }
}

void NavSystem::Reset()
{
    surfaces_.clear();
    loadedKeys_.clear();
    stats_ = NavSystemStats{};
}

void NavSystem::AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const
{
    for (const NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(surface.entity);
        if (comp == nullptr) {
            continue;
        }
        if (!comp->drawAgentPaths) {
            continue;
        }
        // Agent の経路: 現在地から見えている角を順に結び、目的地に短い縦線を立てる
        for (int slotIndex = 0; slotIndex < kCrowdCapacity; ++slotIndex) {
            const NavAgentSlot& slot = surface.slots[static_cast<size_t>(slotIndex)];
            if (slot.entity == kNullEntity || slot.requested == 0) {
                continue;
            }
            const auto* agent = world.GetComponent<NavMeshAgentComponent>(slot.entity);
            if (agent == nullptr) {
                continue;
            }
            const dtCrowdAgent* ag = surface.crowd->getAgent(slotIndex);
            const uint32_t color = agent->status == navagentstatus::kNoPath ? kPathNoPathColor
                : agent->status == navagentstatus::kArrived ? kPathArrivedColor : kPathMovingColor;
            const float* from = ag->npos;
            for (int k = 0; k < ag->ncorners; ++k) {
                const float* to = &ag->cornerVerts[k * 3];
                AddLine(out, from, to, color);
                from = to;
            }
            const float top[3] = { slot.requestedDest[0], slot.requestedDest[1] + 1.0f, slot.requestedDest[2] };
            AddLine(out, slot.requestedDest, top, color);
        }
    }
}

bool NavSystem::SaveSnapshot(NavByteWriter& w) const
{
    uint32_t count = 0;
    for (const NavSurfaceRuntime& surface : surfaces_) {
        count += surface.state == NavSurfaceState::Loaded ? 1u : 0u;
    }
    w.Pod(kSnapshotMagic);
    w.Pod(count);
    for (const NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        w.Pod(surface.entity.index);
        w.Pod(surface.entity.generation);
        w.Pod(surface.assetGuid);
        NavByteWriter storeBlock;
        surface.store->SaveState(storeBlock, false);
        WriteBlock(w, storeBlock);
        NavByteWriter crowdBlock;
        if (!NavSaveCrowd(*surface.crowd, crowdBlock)) {
            return false;
        }
        WriteBlock(w, crowdBlock);
        WriteSlots(w, surface.slots);
    }
    return true;
}

bool NavSystem::ValidateSnapshot(const uint8_t* data, size_t size)
{
    std::vector<SnapshotSurface> parsed;
    return ParseSnapshot(data, size, parsed);
}

bool NavSystem::ApplySnapshot(World& world, const uint8_t* data, size_t size)
{
    std::vector<SnapshotSurface> parsed;
    if (!ParseSnapshot(data, size, parsed)) {
        MYE_LOG_ERROR("[nav] snapshot section is corrupt");
        return false;
    }

    // World の Surface を先に読み込む (Nav 節を当てる前に .mnav を済ませる)。
    // 同じ (entity, asset) で読み込み済み、かつ節に状態がある Surface は作り直さずに状態だけ当てる
    ScanSurfaceKeys(world);
    std::vector<NavSurfaceRuntime> next;
    next.reserve(scanKeys_.size());
    bool ok = true;
    for (const Key& key : scanKeys_) {
        const SnapshotSurface* saved = nullptr;
        for (const SnapshotSurface& s : parsed) {
            if (s.entity == key.entity && s.guid == key.assetGuid) {
                saved = &s;
                break;
            }
        }
        NavSurfaceRuntime* reusable = nullptr;
        for (NavSurfaceRuntime& old : surfaces_) {
            if (saved != nullptr && old.state == NavSurfaceState::Loaded && old.entity == key.entity
                && old.assetGuid == key.assetGuid) {
                reusable = &old;
                break;
            }
        }
        NavSurfaceRuntime& surface = next.emplace_back();
        if (reusable != nullptr) {
            surface = std::move(*reusable);
        } else {
            surface.entity = key.entity;
            surface.assetGuid = key.assetGuid;
            if (key.assetGuid != 0) {
                Load(surface, world.GetName(key.entity));
            }
        }
        if (saved == nullptr || surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        NavByteReader storeReader(saved->store.data(), saved->store.size());
        NavByteReader crowdReader(saved->crowd.data(), saved->crowd.size());
        if (!surface.store->LoadState(storeReader) || !NavLoadCrowd(*surface.crowd, crowdReader)) {
            MYE_LOG_ERROR("[nav] surface '%s': failed to restore the navigation state", world.GetName(key.entity));
            surface.state = NavSurfaceState::Failed;
            ok = false;
            continue;
        }
        surface.slots = saved->slots;
    }
    surfaces_ = std::move(next);
    loadedKeys_ = scanKeys_;
    return ok;
}

bool NavSystem::HasHashableState() const
{
    for (const NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state == NavSurfaceState::Loaded && surface.OccupiedSlots() > 0) {
            return true;
        }
    }
    return false;
}

int NavSystem::HashedAgentCount() const
{
    int count = 0;
    for (const NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state == NavSurfaceState::Loaded) {
            count += surface.OccupiedSlots();
        }
    }
    return count;
}

uint64_t NavSystem::StateHash() const
{
    uint64_t hash = kNavFnvSeed;
    for (const NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        NavByteWriter w;
        w.Pod(surface.entity.index);
        w.Pod(surface.entity.generation);
        WriteSlots(w, surface.slots);
        w.Pod(surface.store->HashLayers());
        w.Pod(static_cast<uint32_t>(surface.store->ObstacleCount()));
        NavSaveCrowd(*surface.crowd, w); // 途中状態は tick 境界に残らない (PATCHES.md)。失敗してもハッシュは決定的
        hash = NavFnv1a(hash, w.Data().data(), w.Size());
    }
    return hash;
}

} // namespace mye
