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
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/Navigation/NavBakeInput.h"
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
constexpr uint32_t kObstacleColor = 0xFF9030FFu;
constexpr uint32_t kModifierColor = 0xC070FFFFu; // エリアを塗り替える箱 (SceneView のギズモと同じ紫)
constexpr uint32_t kLinkColor = 0x40FFC0FFu;     // Off-Mesh Link (SceneView のギズモと同じ緑)
constexpr float kLinkArrowLength = 0.35f;
constexpr float kLinkArrowSpread = 0.4f;         // 矢尻の開き (長さに対する横の比)
// Link の吸着半径の下限 (m)。width が小さすぎても歩行面の端の数 cm の差で入口が外れないように
constexpr float kLinkMinRadius = 0.05f;
// 渡りの 1 フェーズの長さの上限 (tick)。速度が極端に小さい Link で整数があふれないための歯止め
constexpr int kLinkMaxTicks = 60 * 600;

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

// 詰まり検出: 経路の残り距離を kStuckTicks の間に max(半径 x 係数, 下限) 以上縮められない Moving の Agent を止める
constexpr int kStuckTicks = 60;
constexpr float kStuckProgressRadiusScale = 0.25f;
constexpr float kStuckMinProgress = 0.01f;
// 部分経路の終点からこの距離 (radius の倍率) 以内で前進が止まったら Arrived とみなす。
// CC は NavMesh の縁の手前で止まる (カプセルの接触・skinWidth) ので、終点までの残りが stoppingDistance を割れないことがある
constexpr float kPartialArriveRadiusScale = 1.0f;
// 動いた障害物を作り直す閾値 (m)。これ以下の動きは TileCache を触らない (毎 tick 動く物体で全タイルを入れ直さない)
constexpr float kObstacleMoveThreshold = 0.05f;
constexpr int kCylinderSegments = 16;
// 回転した動きの閾値 (ラジアン)。半寸法 2.5 m の箱の端で約 5 cm
constexpr float kObstacleYawThreshold = 0.02f;
// ワールド行列が「y 軸まわりの回転 + 拡大」とみなせる傾きの許容 (行の長さに対する比)。超えると外接 AABB で切る
constexpr float kObstacleTiltEpsilon = 1.0e-3f;
// dtTileCache が回転箱の範囲に使う係数 (DetourTileCache.cpp の getObstacleBounds と同じ)
constexpr float kOrientedBoxReach = 1.41f;

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

// 目的地の変更・到着・取り消しで、詰まりの記録を初期化する
void ResetStuck(NavAgentSlot& slot)
{
    slot.stuck = 0;
    slot.noProgressTicks = 0;
    slot.bestRemaining = -1.0f;
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
                || !r.Pod(slot.destInvalid) || !r.Pod(slot.arrived) || !r.Pod(slot.stuck) || !r.Pod(slot.noProgressTicks)
                || !r.Pod(slot.bestRemaining) || !r.Bytes(slot.requestedDest, sizeof(slot.requestedDest))
                || !r.Pod(slot.linkPhase) || !r.Pod(slot.linkMode) || !r.Pod(slot.linkTick) || !r.Pod(slot.linkTicks)
                || !r.Pod(slot.linkHeight) || !r.Pod(slot.linkSpeed) || !r.Bytes(slot.linkFrom, sizeof(slot.linkFrom))
                || !r.Bytes(slot.linkStart, sizeof(slot.linkStart)) || !r.Bytes(slot.linkEnd, sizeof(slot.linkEnd))
                || slot.linkPhase > 3) {
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
        w.Pod(slot.stuck);
        w.Pod(slot.noProgressTicks);
        w.Pod(slot.bestRemaining);
        w.Bytes(slot.requestedDest, sizeof(slot.requestedDest));
        w.Pod(slot.linkPhase);
        w.Pod(slot.linkMode);
        w.Pod(slot.linkTick);
        w.Pod(slot.linkTicks);
        w.Pod(slot.linkHeight);
        w.Pod(slot.linkSpeed);
        w.Bytes(slot.linkFrom, sizeof(slot.linkFrom));
        w.Bytes(slot.linkStart, sizeof(slot.linkStart));
        w.Bytes(slot.linkEnd, sizeof(slot.linkEnd));
    }
}

void WriteBlock(NavByteWriter& w, const NavByteWriter& block)
{
    w.Pod(static_cast<uint32_t>(block.Size()));
    w.Bytes(block.Data().data(), block.Size());
}

// LocalTransform から行ベクトル規約のワールド行列 (親が無い前提)。TransformSystem の行列と同じ向きで、スカラーだけで組む
void MatrixOfRootTransform(const LocalTransform& t, float (&m)[4][4])
{
    const float x = t.rotation.x;
    const float y = t.rotation.y;
    const float z = t.rotation.z;
    const float w = t.rotation.w;
    const float r[3][3] = {
        { 1.0f - 2.0f * (y * y + z * z), 2.0f * (x * y - w * z), 2.0f * (x * z + w * y) },
        { 2.0f * (x * y + w * z), 1.0f - 2.0f * (x * x + z * z), 2.0f * (y * z - w * x) },
        { 2.0f * (x * z - w * y), 2.0f * (y * z + w * x), 1.0f - 2.0f * (x * x + y * y) },
    };
    const float s[3] = { t.scale.x, t.scale.y, t.scale.z };
    for (int j = 0; j < 3; ++j) {
        // 行 j = 基底 e_j を拡大して回した像 = R の列 j に s_j を掛けたもの
        for (int i = 0; i < 3; ++i) {
            m[j][i] = s[j] * r[i][j];
        }
        m[j][3] = 0.0f;
    }
    m[3][0] = t.position.x;
    m[3][1] = t.position.y;
    m[3][2] = t.position.z;
    m[3][3] = 1.0f;
}

void TransformPoint(const float (&m)[4][4], float x, float y, float z, float (&out)[3])
{
    for (int i = 0; i < 3; ++i) {
        out[i] = x * m[0][i] + y * m[1][i] + z * m[2][i] + m[3][i];
    }
}

bool IsFinitePositive(float v)
{
    return std::isfinite(v) && v > 0.0f;
}

// 障害物の形 (type, v) が前回と閾値以内に同じか
bool SameObstacleShape(const NavObstacleSpec& a, const NavObstacleSpec& b)
{
    if (a.type != b.type || a.area != b.area || std::fabs(a.yaw - b.yaw) > kObstacleYawThreshold) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        if (std::fabs(a.v[i] - b.v[i]) > kObstacleMoveThreshold) {
            return false;
        }
    }
    return true;
}

// 障害物の外接 AABB (Surface の範囲との重なり判定用)
void ObstacleBounds(const NavObstacleSpec& o, float (&lo)[3], float (&hi)[3])
{
    if (o.type == DT_OBSTACLE_CYLINDER) {
        lo[0] = o.v[0] - o.v[3];
        lo[1] = o.v[1];
        lo[2] = o.v[2] - o.v[3];
        hi[0] = o.v[0] + o.v[3];
        hi[1] = o.v[1] + o.v[4];
        hi[2] = o.v[2] + o.v[3];
    } else if (o.type == DT_OBSTACLE_ORIENTED_BOX) {
        const float reach = kOrientedBoxReach * (std::max)(o.v[3], o.v[5]);
        lo[0] = o.v[0] - reach;
        lo[1] = o.v[1] - o.v[4];
        lo[2] = o.v[2] - reach;
        hi[0] = o.v[0] + reach;
        hi[1] = o.v[1] + o.v[4];
        hi[2] = o.v[2] + reach;
    } else {
        for (int i = 0; i < 3; ++i) {
            lo[i] = o.v[i];
            hi[i] = o.v[3 + i];
        }
    }
}

// エンティティのワールド行列。配置は LocalTransform (親が無い) か前 tick の WorldMatrix
bool EntityMatrix(World& world, EntityID e, float (&m)[4][4])
{
    const auto* local = world.GetComponent<LocalTransform>(e);
    if (local != nullptr && world.GetParent(e) == kNullEntity) {
        MatrixOfRootTransform(*local, m);
        return true;
    }
    if (const auto* wm = world.GetComponent<WorldMatrixComponent>(e)) {
        std::memcpy(m, wm->value.m, sizeof(m));
        return true;
    }
    return false;
}

// Surface のベイク範囲 (ベイクと同じ計算)。Transform が無ければ false
bool SurfaceWorldBounds(World& world, EntityID surface, float (&lo)[3], float (&hi)[3])
{
    const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(surface);
    const auto* matrix = world.GetComponent<WorldMatrixComponent>(surface);
    if (comp == nullptr || matrix == nullptr) {
        return false;
    }
    const NavBakeConfig bake = NavMakeBakeConfig(*comp, matrix->value);
    std::memcpy(lo, bake.boundsMin, sizeof(lo));
    std::memcpy(hi, bake.boundsMax, sizeof(hi));
    return true;
}

// store の障害物 (paint = false: 切り抜き / true: 塗り替え) を wanted (key 昇順) と突き合わせた差分。
// store の一覧は key 昇順なので、種類で絞っても昇順のまま
struct SpecDiff {
    std::vector<uint64_t> removeKeys;
    std::vector<size_t> addIndices;
};

void DiffSpecs(const NavTileStore& store, const std::vector<NavObstacleSpec>& wanted, bool paint, SpecDiff& diff)
{
    const int have = store.ObstacleCount();
    int si = 0;
    size_t wi = 0;
    for (;;) {
        while (si < have && (store.ObstacleAt(si).area != kNavNoPaint) != paint) {
            ++si;
        }
        const bool storeLeft = si < have;
        const bool wantLeft = wi < wanted.size();
        if (!storeLeft && !wantLeft) {
            return;
        }
        const uint64_t storeKey = storeLeft ? store.ObstacleAt(si).key : 0;
        const uint64_t wantKey = wantLeft ? wanted[wi].key : 0;
        if (storeLeft && (!wantLeft || storeKey < wantKey)) {
            diff.removeKeys.push_back(storeKey);
            ++si;
        } else if (wantLeft && (!storeLeft || wantKey < storeKey)) {
            diff.addIndices.push_back(wi);
            ++wi;
        } else {
            if (!SameObstacleShape(store.ObstacleAt(si), wanted[wi])) {
                diff.removeKeys.push_back(storeKey);
                diff.addIndices.push_back(wi);
            }
            ++si;
            ++wi;
        }
    }
}

// 差分を store へ積む (Commit は呼び出し側)。失敗した操作の数を返す
int ApplyDiff(NavTileStore& store, const std::vector<NavObstacleSpec>& wanted, const SpecDiff& diff)
{
    int failures = 0;
    for (const uint64_t key : diff.removeKeys) {
        if (!store.RemoveObstacle(key)) {
            ++failures;
        }
    }
    for (const size_t index : diff.addIndices) {
        const NavObstacleSpec& spec = wanted[index];
        bool added = false;
        if (spec.type == DT_OBSTACLE_CYLINDER) {
            added = store.AddCylinderObstacle(spec.key, spec.v, spec.v[3], spec.v[4], spec.area);
        } else if (spec.type == DT_OBSTACLE_ORIENTED_BOX) {
            added = store.AddOrientedBoxObstacle(spec.key, spec.v, spec.v + 3, spec.yaw, spec.area);
        } else {
            added = store.AddBoxObstacle(spec.key, spec.v, spec.v + 3, spec.area);
        }
        if (!added) {
            ++failures;
        }
    }
    return failures;
}

// 進行方向 (mx, mz) へ向ける (親が無いときだけ。親付きの向きは親の回転と合成されるので触らない)
void TurnToward(LocalTransform& transform, const NavMeshAgentComponent& agent, bool rooted, float mx, float mz, float dt)
{
    const float speed2 = mx * mx + mz * mz;
    if (!rooted || agent.angularSpeedDeg <= 0.0f || speed2 <= kMinTurnSpeed * kMinTurnSpeed) {
        return;
    }
    const float current = YawOf(transform.rotation);
    const float target = std::atan2(mx, mz);
    const float maxStep = agent.angularSpeedDeg * (kPi / 180.0f) * dt;
    const float delta = (std::max)(-maxStep, (std::min)(maxStep, WrapPi(target - current)));
    const float yaw = current + delta;
    transform.rotation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

float Distance3(const float* a, const float* b)
{
    return Length3(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// 距離を速さ x dt で割った tick 数 (切り上げ、1..kLinkMaxTicks)
int TicksFor(float distance, float speed, float dt)
{
    const float step = (std::max)(speed, 0.01f) * dt;
    const float ticks = std::ceil(distance / step);
    if (!(ticks >= 1.0f)) {
        return 1;
    }
    return ticks > static_cast<float>(kLinkMaxTicks) ? kLinkMaxTicks : static_cast<int>(ticks);
}

void Lerp3(const float* a, const float* b, float u, float (&out)[3])
{
    for (int i = 0; i < 3; ++i) {
        out[i] = a[i] + (b[i] - a[i]) * u;
    }
}

// Agent 1 体分の渡りを 1 tick 進め、この tick の足元の位置を out に返す。渡り終えたら finished = true (位置は出口)
void AdvanceLink(NavAgentSlot& slot, NavMeshAgentComponent& agent, float dt, float (&out)[3], bool& finished)
{
    finished = false;
    ++slot.linkTick;
    if (slot.linkPhase == 1) {
        const float u = (std::min)(1.0f, static_cast<float>(slot.linkTick) / static_cast<float>((std::max)(slot.linkTicks, 1)));
        Lerp3(slot.linkFrom, slot.linkStart, u, out);
        if (slot.linkTick >= slot.linkTicks) {
            slot.linkTick = 0;
            if (slot.linkMode == navlinktraversal::kManual) {
                slot.linkPhase = 3;
                slot.linkTicks = 0;
            } else {
                slot.linkPhase = 2;
                slot.linkTicks = TicksFor(Distance3(slot.linkStart, slot.linkEnd), slot.linkSpeed, dt);
            }
        }
    } else if (slot.linkPhase == 2) {
        const float u = (std::min)(1.0f, static_cast<float>(slot.linkTick) / static_cast<float>((std::max)(slot.linkTicks, 1)));
        Lerp3(slot.linkStart, slot.linkEnd, u, out);
        if (slot.linkMode == navlinktraversal::kJump) {
            out[1] += slot.linkHeight * 4.0f * u * (1.0f - u);
        }
        if (slot.linkTick >= slot.linkTicks) {
            finished = true;
            std::memcpy(out, slot.linkEnd, sizeof(out));
        }
    } else {
        std::memcpy(out, slot.linkStart, sizeof(out));
        if (agent.linkComplete) {
            agent.linkComplete = false;
            finished = true;
            std::memcpy(out, slot.linkEnd, sizeof(out));
        }
    }
}

} // namespace

uint64_t NavLinkKey(EntityID entity)
{
    return NavObstacleKey(entity);
}

bool NavMakeLinkSpec(const NavMeshLinkComponent& link, const float (&m)[4][4], EntityID entity, NavLinkSpec& out)
{
    out = NavLinkSpec{};
    float start[3];
    float end[3];
    TransformPoint(m, link.start.x, link.start.y, link.start.z, start);
    TransformPoint(m, link.end.x, link.end.y, link.end.z, end);
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(start[i]) || !std::isfinite(end[i])) {
            return false;
        }
    }
    if (Distance3(start, end) < kLinkMinRadius) {
        return false;
    }
    const float sx = Length3(m[0][0], m[0][1], m[0][2]);
    const float sz = Length3(m[2][0], m[2][1], m[2][2]);
    const float radius = 0.5f * std::fabs(link.width) * (std::max)(sx, sz);
    if (!std::isfinite(radius)) {
        return false;
    }
    out.key = NavLinkKey(entity);
    std::memcpy(out.start, start, sizeof(start));
    std::memcpy(out.end, end, sizeof(end));
    out.radius = (std::max)(radius, kLinkMinRadius);
    out.bidirectional = link.bidirectional ? 1 : 0;
    out.area = static_cast<uint8_t>((std::min)((std::max)(link.area, 0), kNavAreaCount - 1));
    out.userId = entity.index;
    return true;
}

NavLinkPlacement NavCheckLinkPlacement(World& world, const NavLinkSpec& link)
{
    const ComponentTypeId req[] = { NavMeshSurfaceComponent::sTypeId };
    NavLinkPlacement result = NavLinkPlacement::NoSurface;
    bool found = false;
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            float lo[3];
            float hi[3];
            if (found || !IsEntityActive(world, e) || !SurfaceWorldBounds(world, e, lo, hi)) {
                continue;
            }
            bool inside = true;
            for (int i = 0; i < 3; ++i) {
                inside = inside && link.start[i] >= lo[i] && link.start[i] <= hi[i];
            }
            if (!inside) {
                continue;
            }
            found = true;
            const auto* comp = world.GetComponent<NavMeshSurfaceComponent>(e);
            const float span = static_cast<float>(comp->tileSize) * NavResolveCellSize(*comp).cellSize;
            const auto tileOf = [&](const float* p, int axis) {
                return static_cast<int>(std::floor((p[axis] - lo[axis]) / span));
            };
            const bool far = std::abs(tileOf(link.end, 0) - tileOf(link.start, 0)) > 1
                || std::abs(tileOf(link.end, 2) - tileOf(link.start, 2)) > 1;
            result = far ? NavLinkPlacement::ExitTooFar : NavLinkPlacement::Ok;
        }
    });
    return result;
}

void NavCollectLinkSpecs(World& world, std::vector<NavLinkSpec>& out)
{
    out.clear();
    const ComponentTypeId req[] = { NavMeshLinkComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int li = arch.FindTypeIndex(NavMeshLinkComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            float m[4][4];
            NavLinkSpec spec;
            const auto* link = static_cast<const NavMeshLinkComponent*>(arch.GetPtr(li, row));
            if (EntityMatrix(world, e, m) && NavMakeLinkSpec(*link, m, e, spec)) {
                out.push_back(spec);
            }
        }
    });
    std::sort(out.begin(), out.end(), [](const NavLinkSpec& a, const NavLinkSpec& b) { return a.key < b.key; });
}

void NavFilterLinksToSurface(World& world, EntityID surface, const std::vector<NavLinkSpec>& all,
                             std::vector<NavLinkSpec>& out)
{
    out.clear();
    float surfaceMin[3] = {};
    float surfaceMax[3] = {};
    if (!SurfaceWorldBounds(world, surface, surfaceMin, surfaceMax)) {
        return;
    }
    for (const NavLinkSpec& link : all) {
        bool inside = true;
        for (int i = 0; i < 3; ++i) {
            inside = inside && link.start[i] >= surfaceMin[i] && link.start[i] <= surfaceMax[i];
        }
        if (inside) {
            out.push_back(link);
        }
    }
}

uint64_t NavObstacleKey(EntityID entity)
{
    return (static_cast<uint64_t>(entity.index) << 32) | static_cast<uint64_t>(entity.generation);
}

uint64_t NavModifierKey(EntityID entity)
{
    return NavObstacleKey(entity) | (1ull << 63);
}

bool NavMakeModifierSpec(const NavMeshModifierComponent& modifier, const float (&m)[4][4], uint64_t key,
                         NavObstacleSpec& out)
{
    NavMeshObstacleComponent box;
    box.shape = navobstacleshape::kBox;
    box.center = modifier.center;
    box.size = modifier.size;
    if (!NavMakeObstacleSpec(box, m, key, out)) {
        return false;
    }
    out.area = static_cast<uint8_t>((std::min)((std::max)(modifier.area, 0), kNavAreaCount - 1));
    return true;
}

void NavCollectModifierSpecs(World& world, std::vector<NavObstacleSpec>& out)
{
    out.clear();
    const ComponentTypeId req[] = { NavMeshModifierComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int mi = arch.FindTypeIndex(NavMeshModifierComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            float m[4][4];
            NavObstacleSpec spec;
            const auto* modifier = static_cast<const NavMeshModifierComponent*>(arch.GetPtr(mi, row));
            if (EntityMatrix(world, e, m) && NavMakeModifierSpec(*modifier, m, NavModifierKey(e), spec)) {
                out.push_back(spec);
            }
        }
    });
    std::sort(out.begin(), out.end(), [](const NavObstacleSpec& a, const NavObstacleSpec& b) { return a.key < b.key; });
}

void NavFilterSpecsToSurface(World& world, EntityID surface, const std::vector<NavObstacleSpec>& all,
                             std::vector<NavObstacleSpec>& out)
{
    out.clear();
    float surfaceMin[3] = {};
    float surfaceMax[3] = {};
    const bool hasBounds = SurfaceWorldBounds(world, surface, surfaceMin, surfaceMax);
    for (const NavObstacleSpec& spec : all) {
        float lo[3];
        float hi[3];
        ObstacleBounds(spec, lo, hi);
        bool overlap = true;
        for (int i = 0; hasBounds && i < 3; ++i) {
            overlap = overlap && lo[i] <= surfaceMax[i] && hi[i] >= surfaceMin[i];
        }
        if (overlap) {
            out.push_back(spec);
        }
    }
}

int NavApplyModifiers(NavTileStore& store, const std::vector<NavObstacleSpec>& wanted, int& failures)
{
    SpecDiff diff;
    DiffSpecs(store, wanted, true, diff);
    if (diff.removeKeys.empty() && diff.addIndices.empty()) {
        return 0;
    }
    failures += ApplyDiff(store, wanted, diff);
    if (!store.Commit()) {
        ++failures;
    }
    return static_cast<int>(diff.removeKeys.size() + diff.addIndices.size());
}

bool NavMakeObstacleSpec(const NavMeshObstacleComponent& obstacle, const float (&m)[4][4], uint64_t key,
                         NavObstacleSpec& out)
{
    out = NavObstacleSpec{};
    out.key = key;
    if (obstacle.shape == navobstacleshape::kCylinder) {
        if (!IsFinitePositive(obstacle.radius) || !IsFinitePositive(obstacle.height)) {
            return false;
        }
        float center[3];
        TransformPoint(m, obstacle.center.x, obstacle.center.y, obstacle.center.z, center);
        const float sx = Length3(m[0][0], m[0][1], m[0][2]);
        const float sy = Length3(m[1][0], m[1][1], m[1][2]);
        const float sz = Length3(m[2][0], m[2][1], m[2][2]);
        const float radius = obstacle.radius * (std::max)(sx, sz);
        const float height = obstacle.height * sy;
        if (!std::isfinite(center[0]) || !std::isfinite(center[1]) || !std::isfinite(center[2]) || !IsFinitePositive(radius)
            || !IsFinitePositive(height)) {
            return false;
        }
        out.type = DT_OBSTACLE_CYLINDER;
        out.v[0] = center[0];
        out.v[1] = center[1] - height * 0.5f;
        out.v[2] = center[2];
        out.v[3] = radius;
        out.v[4] = height;
        return true;
    }
    const float hx = std::fabs(obstacle.size.x) * 0.5f;
    const float hy = std::fabs(obstacle.size.y) * 0.5f;
    const float hz = std::fabs(obstacle.size.z) * 0.5f;
    if (!IsFinitePositive(hx) || !IsFinitePositive(hy) || !IsFinitePositive(hz)) {
        return false;
    }
    // y 軸まわりの回転だけなら、実形のまま回転箱で切る (dtTileCache::addBoxObstacle の yaw 版)
    const float len0 = Length3(m[0][0], m[0][1], m[0][2]);
    const float len1 = Length3(m[1][0], m[1][1], m[1][2]);
    const float len2 = Length3(m[2][0], m[2][1], m[2][2]);
    const bool pureYaw = std::fabs(m[1][0]) <= kObstacleTiltEpsilon * len1 && std::fabs(m[1][2]) <= kObstacleTiltEpsilon * len1
        && std::fabs(m[0][1]) <= kObstacleTiltEpsilon * len0 && std::fabs(m[2][1]) <= kObstacleTiltEpsilon * len2;
    if (pureYaw) {
        float center[3];
        TransformPoint(m, obstacle.center.x, obstacle.center.y, obstacle.center.z, center);
        const float half[3] = { hx * len0, hy * len1, hz * len2 };
        const float yaw = std::atan2(-m[0][2], m[0][0]);
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(center[i]) || !IsFinitePositive(half[i])) {
                return false;
            }
        }
        if (!std::isfinite(yaw)) {
            return false;
        }
        out.type = DT_OBSTACLE_ORIENTED_BOX;
        for (int i = 0; i < 3; ++i) {
            out.v[i] = center[i];
            out.v[3 + i] = half[i];
        }
        out.yaw = yaw;
        return true;
    }
    float lo[3] = {};
    float hi[3] = {};
    for (int corner = 0; corner < 8; ++corner) {
        float p[3];
        TransformPoint(m, obstacle.center.x + ((corner & 1) ? hx : -hx), obstacle.center.y + ((corner & 2) ? hy : -hy),
                       obstacle.center.z + ((corner & 4) ? hz : -hz), p);
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(p[i])) {
                return false;
            }
            lo[i] = corner == 0 ? p[i] : (std::min)(lo[i], p[i]);
            hi[i] = corner == 0 ? p[i] : (std::max)(hi[i], p[i]);
        }
    }
    out.type = DT_OBSTACLE_BOX;
    for (int i = 0; i < 3; ++i) {
        out.v[i] = lo[i];
        out.v[3 + i] = hi[i];
    }
    return true;
}

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

void NavSystem::SyncObstacles(World& world)
{
    stats_.obstacleUs = 0.0;
    stats_.obstacleChanges = 0;
    stats_.linkChanges = 0;
    bool anyLoaded = false;
    for (const NavSurfaceRuntime& surface : surfaces_) {
        anyLoaded = anyLoaded || surface.state == NavSurfaceState::Loaded;
    }
    if (!anyLoaded) {
        return;
    }

    // 欲しい障害物 (carve が立ち、形が有効なもの) をキー順に並べる
    wantedObstacles_.clear();
    const ComponentTypeId req[] = { NavMeshObstacleComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int oi = arch.FindTypeIndex(NavMeshObstacleComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            const auto* obstacle = static_cast<const NavMeshObstacleComponent*>(arch.GetPtr(oi, row));
            if (!obstacle->carve || !IsEntityActive(world, e)) {
                continue;
            }
            float m[4][4];
            NavObstacleSpec spec;
            if (EntityMatrix(world, e, m) && NavMakeObstacleSpec(*obstacle, m, NavObstacleKey(e), spec)) {
                wantedObstacles_.push_back(spec);
            }
        }
    });
    std::sort(wantedObstacles_.begin(), wantedObstacles_.end(),
              [](const NavObstacleSpec& a, const NavObstacleSpec& b) { return a.key < b.key; });
    // Modifier (エリアの塗り替え) も同じ store の一覧に入る。キーの最上位ビットで Obstacle と区別する
    NavCollectModifierSpecs(world, wantedModifiers_);
    // Link (Off-Mesh Link)。入口を持つタイル列だけを作り直す。Obstacle / Modifier と同じ Commit にまとめる
    NavCollectLinkSpecs(world, wantedLinks_);

    // Surface ごとに、store が持つ障害物 (これも復元済みの状態) と突き合わせる。NavSystem 自身は前回の記録を持たないので、
    // restore 直後でも二重に足さず消し忘れない
    int failures = 0;
    bool anyCommit = false;
    for (NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        NavTileStore& store = *surface.store;
        // この Surface の範囲 (ベイクと同じ計算) と重なるものだけを付ける。範囲外は容量 (maxObstacles) を食うだけ
        NavFilterSpecsToSurface(world, surface.entity, wantedObstacles_, wantedHere_);
        NavFilterSpecsToSurface(world, surface.entity, wantedModifiers_, wantedModifiersHere_);
        SpecDiff carveDiff;
        SpecDiff paintDiff;
        DiffSpecs(store, wantedHere_, false, carveDiff);
        DiffSpecs(store, wantedModifiersHere_, true, paintDiff);
        const size_t changes = carveDiff.removeKeys.size() + carveDiff.addIndices.size() + paintDiff.removeKeys.size()
            + paintDiff.addIndices.size();
        NavFilterLinksToSurface(world, surface.entity, wantedLinks_, wantedLinksHere_);
        if (changes == 0 && wantedLinksHere_.empty() && store.LinkCount() == 0) {
            continue;
        }
        // 撤去 -> 追加 -> Link -> Commit の順で、同じ tick 内に確定する
        const auto t0 = std::chrono::steady_clock::now();
        failures += ApplyDiff(store, wantedHere_, SpecDiff{ carveDiff.removeKeys, {} });
        failures += ApplyDiff(store, wantedModifiersHere_, SpecDiff{ paintDiff.removeKeys, {} });
        failures += ApplyDiff(store, wantedHere_, SpecDiff{ {}, carveDiff.addIndices });
        failures += ApplyDiff(store, wantedModifiersHere_, SpecDiff{ {}, paintDiff.addIndices });
        const int linkChanges = store.ReplaceLinks(wantedLinksHere_);
        if (linkChanges < 0) {
            ++failures;
        }
        if (changes == 0 && linkChanges <= 0) {
            continue;
        }
        if (!store.Commit()) {
            ++failures;
        }
        stats_.obstacleUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        stats_.obstacleChanges += static_cast<int>(changes);
        stats_.linkChanges += (std::max)(linkChanges, 0);
        anyCommit = true;
    }
    // 入口か出口が歩行面につながらない Link (歩行面が無い・出口が 2 タイル以上離れている) は使われない。
    // タイルを作り直した tick にだけ数え直し、数が変わったときに 1 回警告する
    if (anyCommit) {
        int disconnected = 0;
        for (const NavSurfaceRuntime& surface : surfaces_) {
            if (surface.state == NavSurfaceState::Loaded) {
                disconnected += surface.store->LinkCount() - surface.store->ConnectedLinkCount();
            }
        }
        stats_.linkDisconnected = disconnected;
        if (disconnected != linkDisconnected_) {
            if (disconnected > 0) {
                ++stats_.linkWarnings;
                MYE_LOG_WARN("[nav] %d link(s) are not connected to the navigation mesh at the entrance or the exit "
                             "(no walkable surface there, or the exit is 2 or more tiles away) and are not used",
                             disconnected);
            }
            linkDisconnected_ = disconnected;
        }
    }
    stats_.maxObstacleUs = (std::max)(stats_.maxObstacleUs, stats_.obstacleUs);
    // 失敗は容量超過 (maxObstacles) など。毎 tick 再試行されるので、数が変わったときだけ警告する
    if (failures != obstacleFailures_) {
        if (failures > 0) {
            MYE_LOG_WARN("[nav] %d obstacle operation(s) failed (the TileCache may be full); retrying every tick", failures);
        }
        obstacleFailures_ = failures;
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
    SyncObstacles(world);
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

    // dtCrowd の filter は Agent の areaMask ごとに 1 つ (種類は areaMask の昇順 = World だけで決まる)。
    // 16 種を超える areaMask は最後の filter を共有する (その Agent は他の Agent の mask で歩く)
    filterMasks_.clear();
    for (const int idx : wanted) {
        filterMasks_.push_back(agents_[static_cast<size_t>(idx)].agent->areaMask & kNavFlagAllAreas);
    }
    std::sort(filterMasks_.begin(), filterMasks_.end());
    filterMasks_.erase(std::unique(filterMasks_.begin(), filterMasks_.end()), filterMasks_.end());
    if (filterMasks_.size() > static_cast<size_t>(DT_CROWD_MAX_QUERY_FILTER_TYPE)) {
        if (!filterOverflowWarned_) {
            MYE_LOG_WARN("[nav] surface '%s': more than %d distinct agent area masks; the rest share the last filter",
                         world.GetName(surface.entity), DT_CROWD_MAX_QUERY_FILTER_TYPE);
            filterOverflowWarned_ = true;
        }
        filterMasks_.resize(static_cast<size_t>(DT_CROWD_MAX_QUERY_FILTER_TYPE));
    }
    if (filterMasks_.empty()) {
        filterMasks_.push_back(kNavFlagAllAreas);
    }
    // エリアのコスト (Surface のコンポーネントから毎 tick 写す。インスペクタでの変更がそのまま効く)
    const auto* surfaceComp = world.GetComponent<NavMeshSurfaceComponent>(surface.entity);
    bool costsRaised = false;
    for (int i = 0; surfaceComp != nullptr && i < kNavAreaCount; ++i) {
        costsRaised = costsRaised || surfaceComp->areaCosts[i] > 1.0f;
    }
    for (size_t f = 0; f < filterMasks_.size(); ++f) {
        dtQueryFilter* filter = crowd.getEditableFilter(static_cast<int>(f));
        filter->setIncludeFlags(static_cast<unsigned short>(filterMasks_[f]));
        filter->setExcludeFlags(0);
        for (int i = 0; surfaceComp != nullptr && i < kNavAreaCount; ++i) {
            filter->setAreaCost(i, (std::max)(1.0f, surfaceComp->areaCosts[i]));
        }
    }
    // Agent の areaMask に対応する filter の番号
    const auto filterIndexOf = [&](const NavMeshAgentComponent& agent) {
        const uint32_t mask = agent.areaMask & kNavFlagAllAreas;
        const auto it = std::lower_bound(filterMasks_.begin(), filterMasks_.end(), mask);
        return it == filterMasks_.end() ? static_cast<int>(filterMasks_.size()) - 1 : static_cast<int>(it - filterMasks_.begin());
    };

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
        const int filterIndex = filterIndexOf(agent);
        const dtQueryFilter* filter = crowd.getFilter(filterIndex);
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
            params.queryFilterType = static_cast<unsigned char>(filterIndex);
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
        const int filterIndex = filterIndexOf(agent);
        const dtQueryFilter* filter = crowd.getFilter(filterIndex);

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
        params.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_TOPO;
        if (!costsRaised) {
            // 視線による経路の近道は raycast で行われ、エリアのコストを見ない (高コストの帯を突っ切る近道を取る)。
            // コストを上げたエリアがある Surface では使わない
            params.updateFlags |= DT_CROWD_OPTIMIZE_VIS;
        }
        if (quality > 0) {
            params.updateFlags |= DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
        }
        params.obstacleAvoidanceType = static_cast<unsigned char>(quality);
        params.queryFilterType = static_cast<unsigned char>(filterIndex);
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
            ResetStuck(slot);
        } else {
            const float dest[3] = { agent.destination.x, agent.destination.y, agent.destination.z };
            if (slot.requested == 0 || !SameBits(dest, slot.requestedDest)) {
                slot.requested = 1;
                slot.destInvalid = 0;
                slot.arrived = 0;
                ResetStuck(slot);
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

        // dtCrowd が Off-Mesh Link の入口に着いた (OFFMESH に入った) Agent は、ここから NavSystem が渡す
        if (ag->state == DT_CROWDAGENT_STATE_OFFMESH && slot.linkPhase == 0) {
            BeginLink(world, surface, a.slot, agent, dt);
        }
        if (slot.linkPhase != 0) {
            agent.status = navagentstatus::kOnLink;
            agent.remainingDistance = dtVdist(ag->npos, slot.linkEnd);
            agent.pathPartial = ag->partial;
            a.cc->moveInput = { 0.0f, 0.0f, 0.0f }; // 位置は物理の後に PostPhysics が上書きする
            TurnToward(*a.transform, agent, a.rooted, slot.linkEnd[0] - slot.linkStart[0],
                       slot.linkEnd[2] - slot.linkStart[2], dt);
            continue;
        }

        // 状態
        int status = navagentstatus::kIdle;
        float remaining = 0.0f;
        bool partial = false;
        if (!agent.hasDestination) {
            status = navagentstatus::kIdle;
        } else if (slot.destInvalid != 0 || ag->state == DT_CROWDAGENT_STATE_INVALID) {
            status = navagentstatus::kNoPath;
        } else if (slot.stuck != 0) {
            status = navagentstatus::kStuck; // 目的地が変わるまで保持 (止めた時点の残り距離を見せ続ける)
            remaining = agent.remainingDistance;
            partial = agent.pathPartial;
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
                ResetStuck(slot);
                crowd.resetMoveTarget(a.slot);
                status = navagentstatus::kArrived;
            } else {
                status = navagentstatus::kMoving;
                // 詰まり検出: 残り距離が基準から半径の 1/4 以上動かない tick が続いたら止める。
                // 基準より遠ざかった (経路が変わった) ときも基準を取り直す = 迂回の始まりを詰まりと見なさない
                const float progress = (std::max)(agent.radius * kStuckProgressRadiusScale, kStuckMinProgress);
                if (slot.bestRemaining < 0.0f || std::fabs(remaining - slot.bestRemaining) >= progress) {
                    slot.bestRemaining = remaining;
                    slot.noProgressTicks = 0;
                } else if (++slot.noProgressTicks >= kStuckTicks && ag->partial && endReached
                           && toEnd <= (std::max)(arriveDistance, agent.radius * kPartialArriveRadiusScale)) {
                    // 部分経路の終点 (届く限りの最寄り) の近くで前進が止まった = 到着。CC の停止位置は NavMesh の縁と数 cm ずれる
                    slot.arrived = 1;
                    ResetStuck(slot);
                    crowd.resetMoveTarget(a.slot);
                    status = navagentstatus::kArrived;
                    partial = true;
                } else if (slot.noProgressTicks >= kStuckTicks) {
                    slot.stuck = 1;
                    crowd.resetMoveTarget(a.slot);
                    status = navagentstatus::kStuck;
                    MYE_LOG_WARN("[nav] agent '%s' is stuck: its path did not get shorter for %d ticks (%.2f m left)",
                                 world.GetName(a.entity), kStuckTicks, remaining);
                }
            }
        }
        agent.status = status;
        agent.remainingDistance = remaining;
        agent.pathPartial = partial;

        // 移動入力。crowd が今 tick に進めた変位 (速度の積分 + 衝突の押し戻し) をそのまま CC に歩かせる
        const float mx = (ag->npos[0] - a.synced[0]) * invDt;
        const float mz = (ag->npos[2] - a.synced[2]) * invDt;
        const bool halted = slot.stuck != 0;
        a.cc->moveInput = halted ? DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f } : DirectX::XMFLOAT3{ mx, 0.0f, mz };

        // 進行方向へ向ける
        if (!halted) {
            TurnToward(*a.transform, agent, a.rooted, mx, mz, dt);
        }
    }
}

void NavSystem::BeginLink(World& world, NavSurfaceRuntime& surface, int slotIndex, NavMeshAgentComponent& agent,
                          float dt) const
{
    NavAgentSlot& slot = surface.slots[static_cast<size_t>(slotIndex)];
    dtCrowd& crowd = *surface.crowd;
    dtCrowdAgent* ag = crowd.getEditableAgent(slotIndex);
    const dtCrowdAgentAnimation* anim = crowd.getAgentAnimation(slotIndex);
    if (anim == nullptr || !anim->active) {
        ag->state = DT_CROWDAGENT_STATE_WALKING; // 渡り先の記録が無い (起きないはずの状態)。歩行へ戻して経路を引き直させる
        return;
    }
    slot.linkMode = static_cast<uint8_t>(navlinktraversal::kLinear);
    slot.linkHeight = 0.0f;
    slot.linkSpeed = (std::max)(agent.speed, 0.01f);
    // Link の渡り方。入口の dtOffMeshConnection に残した userId (= エンティティの index) からコンポーネントを引く
    const dtOffMeshConnection* con = surface.store->NavMesh()->getOffMeshConnectionByRef(anim->polyRef);
    if (con != nullptr) {
        const ComponentTypeId req[] = { NavMeshLinkComponent::sTypeId };
        bool found = false;
        world.ForEachArchetype(req, [&](Archetype& arch) {
            const int li = arch.FindTypeIndex(NavMeshLinkComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count() && !found; ++row) {
                if (arch.EntityAt(row).index != con->userId) {
                    continue;
                }
                const auto* link = static_cast<const NavMeshLinkComponent*>(arch.GetPtr(li, row));
                slot.linkMode = static_cast<uint8_t>((std::min)((std::max)(link->traversal, 0), static_cast<int>(navlinktraversal::kManual)));
                slot.linkHeight = (std::max)(link->jumpHeight, 0.0f);
                slot.linkSpeed = (std::max)(link->traversalSpeed, 0.01f);
                found = true;
            }
        });
    }
    std::memcpy(slot.linkFrom, anim->initPos, sizeof(slot.linkFrom));
    std::memcpy(slot.linkStart, anim->startPos, sizeof(slot.linkStart));
    std::memcpy(slot.linkEnd, anim->endPos, sizeof(slot.linkEnd));
    slot.linkTick = 0;
    slot.linkPhase = 1;
    slot.linkTicks = TicksFor(Distance3(slot.linkFrom, slot.linkStart), (std::max)(agent.speed, 0.01f), dt);
    agent.linkComplete = false;
    agent.linkStart = { slot.linkStart[0], slot.linkStart[1], slot.linkStart[2] };
    agent.linkEnd = { slot.linkEnd[0], slot.linkEnd[1], slot.linkEnd[2] };
    static const char* const kTraversalNames[] = { "Linear", "Jump", "Manual" };
    if (logCrossings_) {
        MYE_LOG_INFO("[nav] agent '%s' starts crossing a %s link: (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)",
                     world.GetName(slot.entity), kTraversalNames[slot.linkMode], slot.linkStart[0], slot.linkStart[1],
                     slot.linkStart[2], slot.linkEnd[0], slot.linkEnd[1], slot.linkEnd[2]);
    }
}

// 渡り終えた Agent を、渡り始めに保存した出口 (exitPos) で歩行へ戻す。渡っている間に Link が消えた・動いた・Surface が変わった
// ときは、corridor の古い polyRef (Off-Mesh のポリゴン) が無効になっているので使わない:
// 出口の最寄り点へ corridor を置き直し、目的地を要求し直させる。最寄り点が無ければ crowd から外す (次の Update が
// ナビメッシュの外の Agent として扱う)
void NavSystem::FinishLink(NavSurfaceRuntime& surface, int slotIndex, const NavMeshAgentComponent& agent, const float* exitPos)
{
    NavAgentSlot& slot = surface.slots[static_cast<size_t>(slotIndex)];
    dtCrowd& crowd = *surface.crowd;
    dtCrowdAgent* ag = crowd.getEditableAgent(slotIndex);
    const float placeH = (std::max)(kPlaceHorizontalScale * agent.radius, kPlaceHorizontalMin);
    const float placeExt[3] = { placeH, kPlaceVertical, placeH };
    dtPolyRef ref = 0;
    float nearest[3] = {};
    surface.query->findNearestPoly(exitPos, placeExt, crowd.getFilter(ag->params.queryFilterType), &ref, nearest);
    if (ref == 0 || !std::isfinite(nearest[0]) || !std::isfinite(nearest[1]) || !std::isfinite(nearest[2])) {
        crowd.removeAgent(slotIndex);
        slot = NavAgentSlot{};
        return;
    }
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
    crowd.resetMoveTarget(slotIndex);
    slot.linkPhase = 0;
    slot.linkTick = 0;
    slot.linkTicks = 0;
    slot.requested = 0; // 次の Update が目的地を出口から引き直す
    slot.arrived = 0;
    slot.destInvalid = 0;
    ResetStuck(slot);
}

void NavSystem::PostPhysics(World& world, float dt)
{
    for (NavSurfaceRuntime& surface : surfaces_) {
        if (surface.state != NavSurfaceState::Loaded) {
            continue;
        }
        for (int slotIndex = 0; slotIndex < kCrowdCapacity; ++slotIndex) {
            NavAgentSlot& slot = surface.slots[static_cast<size_t>(slotIndex)];
            if (slot.entity == kNullEntity || slot.linkPhase == 0) {
                continue;
            }
            auto* agent = world.GetComponent<NavMeshAgentComponent>(slot.entity);
            auto* cc = world.GetComponent<CharacterControllerComponent>(slot.entity);
            auto* transform = world.GetComponent<LocalTransform>(slot.entity);
            if (agent == nullptr || cc == nullptr || transform == nullptr) {
                continue; // 次の Update が外す
            }
            float feet[3];
            bool finished = false;
            AdvanceLink(slot, *agent, dt, feet, finished);

            // 足元 -> カプセルの中心 (CC の寸法規約と同じ) -> LocalTransform。親があれば親の逆行列でローカルへ戻す
            const EntityID parent = world.GetParent(slot.entity);
            const auto* parentMatrix = parent == kNullEntity ? nullptr : world.GetComponent<WorldMatrixComponent>(parent);
            float sx = transform->scale.x;
            float sy = transform->scale.y;
            float sz = transform->scale.z;
            if (parent != kNullEntity) {
                const auto* wm = world.GetComponent<WorldMatrixComponent>(slot.entity);
                if (wm != nullptr) {
                    sx = Length3(wm->value.m[0][0], wm->value.m[0][1], wm->value.m[0][2]);
                    sy = Length3(wm->value.m[1][0], wm->value.m[1][1], wm->value.m[1][2]);
                    sz = Length3(wm->value.m[2][0], wm->value.m[2][1], wm->value.m[2][2]);
                }
            }
            const float centerY = feet[1] + CapsuleHalfHeight(*cc, sx, sy, sz);
            if (parent == kNullEntity) {
                transform->position = { feet[0], centerY, feet[2] };
            } else if (parentMatrix != nullptr) {
                const DirectX::XMMATRIX inverse = DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&parentMatrix->value));
                DirectX::XMFLOAT3 local;
                DirectX::XMStoreFloat3(&local, DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(feet[0], centerY, feet[2], 0.0f), inverse));
                transform->position = local;
            }

            dtCrowdAgent* ag = surface.crowd->getEditableAgent(slotIndex);
            const float invDt = dt > 0.0f ? 1.0f / dt : 0.0f;
            cc->velocity = finished ? DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f }
                                    : DirectX::XMFLOAT3{ (feet[0] - ag->npos[0]) * invDt, 0.0f, (feet[2] - ag->npos[2]) * invDt };
            std::memcpy(ag->npos, feet, sizeof(feet));
            if (finished) {
                FinishLink(surface, slotIndex, *agent, feet);
            }
        }
    }
}

namespace {

// スクリプト API のクエリの上限。経路の回廊 (ポリゴン列) と角の数
constexpr int kQueryMaxPathPolys = 256;
constexpr int kQueryMaxCorners = 256;

// QueryRandomPoint が円の中の点を試す回数と、点をナビメッシュへ吸着する水平の範囲 (m)。
// 範囲を狭くするのは、吸着で一様性が崩れない (縁から遠い点は捨てる) ようにするため
constexpr int kRandomPointAttempts = 16;
constexpr float kRandomPointSnapHorizontal = 0.5f;

// クエリが返す点の y を歩行面に合わせる (ポリゴンは頂点の高さの平面で、段差の上や坂ではずれる)。
// 層に高さが無い点は Detour の値のまま
void SnapToSurface(const NavTileStore& store, float* point)
{
    float y = point[1];
    if (store.SampleSurfaceHeight(point[0], point[2], point[1], y)) {
        point[1] = y;
    }
}

bool AllFinite(const float* v, int count)
{
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

const NavSurfaceRuntime* NavSystem::ResolveQuerySurface(World& world, int agentTypeId, uint32_t areaMask,
                                                        dtQueryFilter& filter) const
{
    for (const NavSurfaceRuntime& surface : surfaces_) {
        const auto* sc = world.GetComponent<NavMeshSurfaceComponent>(surface.entity);
        if (sc == nullptr || sc->agentTypeId != agentTypeId) {
            continue;
        }
        if (surface.state != NavSurfaceState::Loaded || surface.query == nullptr) {
            return nullptr;
        }
        filter.setIncludeFlags(static_cast<unsigned short>(areaMask & kNavFlagAllAreas));
        filter.setExcludeFlags(0);
        for (int i = 0; i < kNavAreaCount; ++i) {
            filter.setAreaCost(i, (std::max)(1.0f, sc->areaCosts[i]));
        }
        return &surface;
    }
    return nullptr;
}

int NavSystem::QueryFindPath(World& world, int agentTypeId, const float* from, const float* to, uint32_t areaMask,
                             float* outCorners, int maxCorners, bool* outPartial) const
{
    if (outPartial != nullptr) {
        *outPartial = false;
    }
    if (outCorners == nullptr || maxCorners <= 0 || !AllFinite(from, 3) || !AllFinite(to, 3)) {
        return 0;
    }
    dtQueryFilter filter;
    const NavSurfaceRuntime* surface = ResolveQuerySurface(world, agentTypeId, areaMask, filter);
    if (surface == nullptr) {
        return 0;
    }
    const dtNavMeshQuery& query = *surface->query;
    const float ext[3] = { kDestHorizontal, kDestVertical, kDestHorizontal };
    dtPolyRef startRef = 0;
    dtPolyRef endRef = 0;
    float startPt[3] = {};
    float endPt[3] = {};
    query.findNearestPoly(from, ext, &filter, &startRef, startPt);
    query.findNearestPoly(to, ext, &filter, &endRef, endPt);
    if (startRef == 0 || endRef == 0) {
        return 0;
    }
    dtPolyRef polys[kQueryMaxPathPolys];
    int polyCount = 0;
    const dtStatus pathStatus = query.findPath(startRef, endRef, startPt, endPt, &filter, polys, &polyCount, kQueryMaxPathPolys);
    if (dtStatusFailed(pathStatus) || polyCount <= 0) {
        return 0;
    }
    // 回廊が目的地のポリゴンまで届かなければ、回廊の最後のポリゴン上の最寄り点を終点にする (dtCrowd と同じ)
    bool partial = dtStatusDetail(pathStatus, DT_PARTIAL_RESULT) || polys[polyCount - 1] != endRef;
    if (polys[polyCount - 1] != endRef) {
        float closest[3] = {};
        if (dtStatusFailed(query.closestPointOnPoly(polys[polyCount - 1], endPt, closest, nullptr))) {
            return 0;
        }
        dtVcopy(endPt, closest);
    }
    const int cap = (std::min)(maxCorners, kQueryMaxCorners);
    float straight[kQueryMaxCorners * 3];
    int cornerCount = 0;
    if (dtStatusFailed(query.findStraightPath(startPt, endPt, polys, polyCount, straight, nullptr, nullptr, &cornerCount, cap)) || cornerCount <= 0) {
        return 0;
    }
    for (int i = 0; i < cornerCount; ++i) {
        SnapToSurface(*surface->store, &straight[i * 3]);
    }
    std::memcpy(outCorners, straight, sizeof(float) * 3 * static_cast<size_t>(cornerCount));
    if (outPartial != nullptr) {
        *outPartial = partial;
    }
    return cornerCount;
}

bool NavSystem::QuerySamplePosition(World& world, int agentTypeId, const float* pos, const float* extents, uint32_t areaMask,
                                    float* outPoint) const
{
    if (outPoint == nullptr || !AllFinite(pos, 3) || !AllFinite(extents, 3)) {
        return false;
    }
    dtQueryFilter filter;
    const NavSurfaceRuntime* surface = ResolveQuerySurface(world, agentTypeId, areaMask, filter);
    if (surface == nullptr) {
        return false;
    }
    const float ext[3] = { std::fabs(extents[0]), std::fabs(extents[1]), std::fabs(extents[2]) };
    dtPolyRef ref = 0;
    float nearest[3] = {};
    surface->query->findNearestPoly(pos, ext, &filter, &ref, nearest);
    if (ref == 0) {
        return false;
    }
    SnapToSurface(*surface->store, nearest);
    dtVcopy(outPoint, nearest);
    return true;
}

bool NavSystem::QueryRaycast(World& world, int agentTypeId, const float* from, const float* to, uint32_t areaMask,
                             NavRaycastResult& out) const
{
    if (!AllFinite(from, 3) || !AllFinite(to, 3)) {
        return false;
    }
    dtQueryFilter filter;
    const NavSurfaceRuntime* surface = ResolveQuerySurface(world, agentTypeId, areaMask, filter);
    if (surface == nullptr) {
        return false;
    }
    const float ext[3] = { kDestHorizontal, kDestVertical, kDestHorizontal };
    dtPolyRef startRef = 0;
    float startPt[3] = {};
    surface->query->findNearestPoly(from, ext, &filter, &startRef, startPt);
    if (startRef == 0) {
        return false;
    }
    float t = 0.0f;
    float normal[3] = {};
    dtPolyRef visited[kQueryMaxPathPolys];
    int visitedCount = 0;
    if (dtStatusFailed(surface->query->raycast(startRef, startPt, to, &filter, &t, normal, visited, &visitedCount, kQueryMaxPathPolys))) {
        return false;
    }
    out = NavRaycastResult{};
    const float length = std::sqrt(dtVdistSqr(startPt, to));
    if (t > 1.0f) { // 壁に当たらず終点まで歩けた (Detour は FLT_MAX を返す)
        dtVcopy(out.point, to);
        out.distance = length;
        return true;
    }
    out.hit = true;
    for (int i = 0; i < 3; ++i) {
        out.point[i] = startPt[i] + (to[i] - startPt[i]) * t;
        out.normal[i] = normal[i];
    }
    SnapToSurface(*surface->store, out.point);
    out.distance = length * t;
    return true;
}

bool NavSystem::QueryRandomPoint(World& world, int agentTypeId, const float* center, float radius, uint32_t areaMask, Pcg32& rng,
                                 float* outPoint) const
{
    if (outPoint == nullptr || !AllFinite(center, 3) || !std::isfinite(radius) || radius <= 0.0f) {
        return false;
    }
    dtQueryFilter filter;
    const NavSurfaceRuntime* surface = ResolveQuerySurface(world, agentTypeId, areaMask, filter);
    if (surface == nullptr) {
        return false;
    }
    const float ext[3] = { kDestHorizontal, kDestVertical, kDestHorizontal };
    dtPolyRef startRef = 0;
    float startPt[3] = {};
    surface->query->findNearestPoly(center, ext, &filter, &startRef, startPt);
    if (startRef == 0) {
        return false;
    }
    // dtNavMeshQuery::findRandomPointAroundCircle は円に触れるポリゴンの中のどこかを返し、円の外の点も出る
    // (広い床だと半径が意味を持たない)。半径内を保証するため、円の中の点を自分で選んでナビメッシュへ吸着する
    constexpr float kTwoPi = 6.28318530717959f;
    const float snapExt[3] = { kRandomPointSnapHorizontal, kDestVertical, kRandomPointSnapHorizontal };
    for (int attempt = 0; attempt < kRandomPointAttempts; ++attempt) {
        const float angle = rng.NextFloat01() * kTwoPi;
        const float r = radius * std::sqrt(rng.NextFloat01());
        const float sample[3] = { center[0] + r * std::cos(angle), center[1], center[2] + r * std::sin(angle) };
        dtPolyRef ref = 0;
        float snapped[3] = {};
        surface->query->findNearestPoly(sample, snapExt, &filter, &ref, snapped);
        if (ref == 0) {
            continue;
        }
        const float dx = snapped[0] - center[0];
        const float dz = snapped[2] - center[2];
        if (dx * dx + dz * dz <= radius * radius) {
            SnapToSurface(*surface->store, snapped);
            dtVcopy(outPoint, snapped);
            return true;
        }
    }
    return false;
}

bool NavSystem::CompleteLink(World& world, EntityID entity) const
{
    auto* agent = world.GetComponent<NavMeshAgentComponent>(entity);
    if (agent == nullptr) {
        return false;
    }
    for (const NavSurfaceRuntime& surface : surfaces_) {
        for (const NavAgentSlot& slot : surface.slots) {
            // 渡りの途中 (Linear / Jump) に立てると、次の Manual の Link に持ち越して即完了してしまうので Manual だけ受ける
            if (slot.entity == entity && slot.linkPhase != 0 && slot.linkMode == navlinktraversal::kManual) {
                agent->linkComplete = true;
                return true;
            }
        }
    }
    return false;
}

void NavSystem::Reset()
{
    surfaces_.clear();
    loadedKeys_.clear();
    obstacleFailures_ = 0;
    linkDisconnected_ = 0;
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
        if (comp->drawObstacles) {
            AppendObstacleLines(*surface.store, out);
        }
        if (comp->drawLinks) {
            AppendLinkLines(*surface.store, out);
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

void NavSystem::AppendLinkLines(const NavTileStore& store, std::vector<DebugLineCmd>& out)
{
    for (int i = 0; i < store.LinkCount(); ++i) {
        const NavLinkSpec& l = store.LinkAt(i);
        AddLine(out, l.start, l.end, kLinkColor);
        // 矢尻: 向きの先端 (片方向は出口だけ、双方向は両端) に、線の後ろへ開く 2 本
        const float dx = l.end[0] - l.start[0];
        const float dz = l.end[2] - l.start[2];
        const float length = Length3(dx, 0.0f, dz);
        if (length < 1.0e-4f) {
            continue;
        }
        const float ux = dx / length;
        const float uz = dz / length;
        const auto arrowAt = [&](const float* tip, float dirX, float dirZ) {
            const float backX = -dirX * kLinkArrowLength;
            const float backZ = -dirZ * kLinkArrowLength;
            const float side = kLinkArrowLength * kLinkArrowSpread;
            const float left[3] = { tip[0] + backX - dirZ * side, tip[1], tip[2] + backZ + dirX * side };
            const float right[3] = { tip[0] + backX + dirZ * side, tip[1], tip[2] + backZ - dirX * side };
            AddLine(out, tip, left, kLinkColor);
            AddLine(out, tip, right, kLinkColor);
        };
        arrowAt(l.end, ux, uz);
        if (l.bidirectional != 0) {
            arrowAt(l.start, -ux, -uz);
        }
    }
}

void NavSystem::AppendObstacleLines(const NavTileStore& store, std::vector<DebugLineCmd>& out)
{
    for (int i = 0; i < store.ObstacleCount(); ++i) {
        const NavObstacleSpec& o = store.ObstacleAt(i);
        auto line = [&](float ax, float ay, float az, float bx, float by, float bz) {
            DebugLineCmd cmd;
            cmd.ax = ax;
            cmd.ay = ay;
            cmd.az = az;
            cmd.bx = bx;
            cmd.by = by;
            cmd.bz = bz;
            cmd.rgba = o.area == kNavNoPaint ? kObstacleColor : kModifierColor;
            out.push_back(cmd);
        };
        if (o.type == DT_OBSTACLE_CYLINDER) {
            const float r = o.v[3];
            const float y0 = o.v[1];
            const float y1 = o.v[1] + o.v[4];
            for (int k = 0; k < kCylinderSegments; ++k) {
                const float a0 = 2.0f * kPi * static_cast<float>(k) / static_cast<float>(kCylinderSegments);
                const float a1 = 2.0f * kPi * static_cast<float>(k + 1) / static_cast<float>(kCylinderSegments);
                const float x0 = o.v[0] + r * std::cos(a0);
                const float z0 = o.v[2] + r * std::sin(a0);
                const float x1 = o.v[0] + r * std::cos(a1);
                const float z1 = o.v[2] + r * std::sin(a1);
                line(x0, y0, z0, x1, y0, z1);
                line(x0, y1, z0, x1, y1, z1);
                if (k % 4 == 0) {
                    line(x0, y0, z0, x0, y1, z0);
                }
            }
        } else if (o.type == DT_OBSTACLE_ORIENTED_BOX) {
            const float cs = std::cos(o.yaw);
            const float sn = std::sin(o.yaw);
            // 局所 x 軸 = (cos, 0, -sin)、局所 z 軸 = (sin, 0, cos)
            float px[4];
            float pz[4];
            for (int k = 0; k < 4; ++k) {
                const float lx = (k & 1) ? o.v[3] : -o.v[3];
                const float lz = (k & 2) ? o.v[5] : -o.v[5];
                px[k] = o.v[0] + lx * cs + lz * sn;
                pz[k] = o.v[2] - lx * sn + lz * cs;
            }
            const int ring[4] = { 0, 1, 3, 2 };
            const float y0 = o.v[1] - o.v[4];
            const float y1 = o.v[1] + o.v[4];
            for (int k = 0; k < 4; ++k) {
                const int a = ring[k];
                const int b = ring[(k + 1) % 4];
                line(px[a], y0, pz[a], px[b], y0, pz[b]);
                line(px[a], y1, pz[a], px[b], y1, pz[b]);
                line(px[a], y0, pz[a], px[a], y1, pz[a]);
            }
        } else {
            const float* lo = o.v;
            const float* hi = o.v + 3;
            for (int k = 0; k < 4; ++k) {
                const float x0 = (k & 1) ? hi[0] : lo[0];
                const float z0 = (k & 2) ? hi[2] : lo[2];
                line(x0, lo[1], z0, x0, hi[1], z0); // 縦の 4 本
            }
            for (int level = 0; level < 2; ++level) {
                const float y = level == 0 ? lo[1] : hi[1];
                line(lo[0], y, lo[2], hi[0], y, lo[2]);
                line(hi[0], y, lo[2], hi[0], y, hi[2]);
                line(hi[0], y, hi[2], lo[0], y, hi[2]);
                line(lo[0], y, hi[2], lo[0], y, lo[2]);
            }
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
        w.Pod(surface.store->HashObstacles());
        if (surface.store->LinkCount() > 0) {
            w.Pod(surface.store->HashLinks()); // Link の無い Surface のハッシュは M82g のまま
        }
        NavSaveCrowd(*surface.crowd, w); // 途中状態は tick 境界に残らない (PATCHES.md)。失敗してもハッシュは決定的
        hash = NavFnv1a(hash, w.Data().data(), w.Size());
    }
    return hash;
}

} // namespace mye
