//====================================================================================
//                          BehaviorTreeSystem.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの実行器の実装
//====================================================================================
#include "Engine/Engine/AI/BehaviorTreeSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Navigation/NavSystem.h"
#include "Engine/Engine/Perception/PerceptionSystem.h"
#include "Engine/Engine/Rendering/DebugDraw.h"
#include "Engine/Engine/Scene/Tags.h"

namespace mye {

namespace {

int32_t TickToField(uint64_t tick)
{
    return static_cast<int32_t>((std::min)(tick, static_cast<uint64_t>(INT32_MAX)));
}

// tick から ticks 後の tick (int32 へ丸める)
int32_t TickToFieldAt(uint64_t tick, int32_t ticks)
{
    return TickToField(tick + static_cast<uint64_t>(ticks));
}

// 1 ノードの訪問の結果
enum class BtResult : uint8_t {
    Running,
    Success,
    Failure,
};

// SimpleParallel の BtNodeState
constexpr uint8_t kParallelMainDone = 1u << 0;
constexpr int32_t kParallelMainSuccess = 1;
constexpr int32_t kParallelMainFailure = 2;

// 固定 60 Hz の 1 tick の秒数 (BehaviorTreeSystem::Update は dt を受け取らない)
constexpr float kBtTickSeconds = 1.0f / 60.0f;
constexpr float kPi = 3.14159265f;
constexpr float kObserveMoveDistance = 0.5f;    // observeTarget が目的地を書き直す目標の移動量 (m)
constexpr float kDefaultAngularSpeedDeg = 360.0f; // RotateTo の角速度の既定 (度/秒。Agent も無い / 0 のとき)
constexpr float kMinFacingDistance = 1.0e-4f;   // これ以下の水平距離では向きが定まらない (m)
constexpr float kSearchAcceptanceRadius = 0.5f; // SearchArea が点に着いたとみなす水平距離 (m)
constexpr float kSearchSnapHorizontal = 2.0f;   // SearchArea の起点をナビメッシュへ吸着する範囲 (m)
constexpr float kSearchSnapVertical = 4.0f;

// BT 節の 1 要素あたりの最小バイト数 (ByteReader::Count が残りバイトで件数を検証するのに使う)
constexpr size_t kBbValueBytes = sizeof(uint8_t) + sizeof(int32_t) + sizeof(float) + 3 * sizeof(float) + 2 * sizeof(uint32_t);
constexpr size_t kNodeStateBytes = 2 * sizeof(uint8_t) + 2 * sizeof(int32_t);
constexpr size_t kEventBytes = 2 * sizeof(uint64_t) + 4 * sizeof(uint32_t) + 4 * sizeof(float) + sizeof(int32_t) + sizeof(uint32_t);
constexpr size_t kInstanceMinBytes = 2 * sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint8_t) + 3 * sizeof(uint64_t);

bool KeyLess(EntityID a, EntityID b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// 1 体の 1 tick の作業記録
struct RunCtx {
    World& world;
    BtInstance& inst;
    const BehaviorTreeAsset& tree;
    uint64_t tick;
    std::vector<int32_t>* abortTrace;
    const NavSystem* nav; // null = ナビメッシュを引けない (FindRandomPoint / SearchArea は Failure)
    BehaviorTreeSystem* events = nullptr; // SendEvent の積み先。null = 積めない (SendEvent は Failure)
    const ControllerLibrary* controllers = nullptr; // PlayAnimation のステート名の引き先。null = PlayAnimation は Failure
    const AnimationLibrary* clips = nullptr;        // PlayAnimation の waitForEnd のクリップの長さの引き先。null = 待たない
    int steps = 0;
    bool stepLimitHit = false;
    bool aborted = false; // 実行中のノードを 1 つでも Abort した
};

BtResult Visit(RunCtx& c, int32_t index);

// 入っていたノードを抜ける。ノードの状態は初期値へ戻る
void Finish(BtNodeState& state)
{
    state = BtNodeState{};
}

BtNodeState& SlotOf(RunCtx& c, const BtDecoratorDef& deco)
{
    return c.inst.nodes[static_cast<size_t>(deco.slot)];
}

// ノードに入っているか。本体が動いている間に加えて、Decorator だけが動いている間 (Repeat の周回の切れ目など) も含む
bool NodeActive(const BtInstance& inst, const BehaviorTreeAsset& tree, int32_t index)
{
    if (inst.nodes[static_cast<size_t>(index)].active != 0) {
        return true;
    }
    for (const BtDecoratorDef& deco : tree.nodes[static_cast<size_t>(index)].decorators) {
        if (inst.nodes[static_cast<size_t>(deco.slot)].active != 0) {
            return true;
        }
    }
    return false;
}

// at から実行中の子を左から辿った一番深いノードの id。at が入っていなければ -1
int32_t ActiveLeafIdFrom(const BtInstance& inst, const BehaviorTreeAsset& tree, int32_t at)
{
    if (at < 0 || !NodeActive(inst, tree, at)) {
        return -1;
    }
    for (;;) {
        int32_t next = -1;
        for (const int32_t child : tree.nodes[static_cast<size_t>(at)].children) {
            if (NodeActive(inst, tree, child)) {
                next = child;
                break;
            }
        }
        if (next < 0) {
            return tree.nodes[static_cast<size_t>(at)].id;
        }
        at = next;
    }
}

// Decorator (source) が target の部分木を止めたことを表示用に残す。止める前に呼ぶ (止めると実行中の葉が分からなくなる)
void RecordDecoratorAbort(RunCtx& c, const BtNodeDef& source, int32_t stoppedIndex)
{
    const int32_t leaf = ActiveLeafIdFrom(c.inst, c.tree, stoppedIndex);
    if (leaf >= 0) {
        c.inst.lastAbort = BtAbortRecord{ c.tick, source.id, leaf };
    }
}

// ---- 種類別の追加状態と、ノードが外の世界 (Agent) へ書いたものの後始末 ----

template <typename T>
T LoadExtra(const RunCtx& c, const BtNodeDef& node)
{
    T value{};
    std::memcpy(&value, c.inst.extra.data() + node.extraOffset, sizeof(T));
    return value;
}

template <typename T>
void StoreExtra(RunCtx& c, const BtNodeDef& node, const T& value)
{
    std::memcpy(c.inst.extra.data() + node.extraOffset, &value, sizeof(T));
}

// ワールド位置。親が無ければ LocalTransform (Nav の足元と同じ)、あれば前 tick の WorldMatrix
bool WorldPositionOf(World& world, EntityID entity, float (&out)[3])
{
    if (world.GetParent(entity) == kNullEntity) {
        const LocalTransform* transform = world.GetComponent<LocalTransform>(entity);
        if (transform == nullptr) {
            return false;
        }
        out[0] = transform->position.x;
        out[1] = transform->position.y;
        out[2] = transform->position.z;
        return true;
    }
    const WorldMatrixComponent* matrix = world.GetComponent<WorldMatrixComponent>(entity);
    if (matrix == nullptr) {
        return false;
    }
    out[0] = matrix->value.m[3][0];
    out[1] = matrix->value.m[3][1];
    out[2] = matrix->value.m[3][2];
    return true;
}

// キー名のブラックボードの値。BB が無い・名前が空・キーが無ければ nullptr。型は type へ返す
BbValue* FindBb(RunCtx& c, const std::string& name, BbType& type)
{
    if (!c.inst.blackboardAsset || name.empty()) {
        return nullptr;
    }
    const int key = c.inst.blackboardAsset->FindKey(name);
    if (key < 0 || static_cast<size_t>(key) >= c.inst.blackboard.size()) {
        return nullptr;
    }
    type = c.inst.blackboardAsset->keys[static_cast<size_t>(key)].type;
    return &c.inst.blackboard[static_cast<size_t>(key)];
}

// MoveTo / RotateTo の目標の位置。Vector は書かれた値、Entity は生きているもののワールド位置。得られなければ false
bool ResolveTarget(RunCtx& c, const BtNodeDef& node, float (&out)[3])
{
    BbType type = BbType::Bool;
    const BbValue* value = FindBb(c, node.keys[btnodekey::kTarget], type);
    if (value == nullptr || value->isSet == 0) {
        return false;
    }
    if (type == BbType::Vector) {
        out[0] = value->v[0];
        out[1] = value->v[1];
        out[2] = value->v[2];
        return true;
    }
    return type == BbType::Entity && c.world.IsAlive(value->entity) && WorldPositionOf(c.world, value->entity, out);
}

// MoveTo の後始末: 目的地を倒して止め、差し替えた navFilter を戻す
void ReleaseMoveTo(RunCtx& c, const BtNodeDef& node)
{
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    if (agent == nullptr) {
        return;
    }
    agent->hasDestination = false;
    const BtMoveToState state = LoadExtra<BtMoveToState>(c, node);
    if ((state.flags & btmovetoflag::kNavFilterSwapped) != 0) {
        agent->navFilter = AssetID{ state.savedNavFilter };
    }
}

// SearchArea の後始末: 目的地を倒して止める (点の状態は ReleaseBody が 0 へ戻す)
void ReleaseSearchArea(RunCtx& c)
{
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    if (agent != nullptr) {
        agent->hasDestination = false;
    }
}

// Patrol の後始末: 目的地を倒して止める (次の点・向きの状態は ReleaseBody が 0 へ戻す)
void ReleasePatrol(RunCtx& c)
{
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    if (agent != nullptr) {
        agent->hasDestination = false;
    }
}

// RotateTo の後始末: 預かった updateRotation を戻す
void ReleaseRotateTo(RunCtx& c, const BtNodeDef& node)
{
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    const BtRotateToState state = LoadExtra<BtRotateToState>(c, node);
    if (agent != nullptr && (state.flags & btrotatetoflag::kAgentHandled) != 0) {
        agent->updateRotation = (state.flags & btrotatetoflag::kSavedUpdateRotation) != 0;
    }
}

// 入っていた本体が外へ書いたもの (Agent の目的地・navFilter・updateRotation) を元へ戻し、追加状態を消す。
// 終了 (Success / Failure) と Abort の両方がここを通る
void ReleaseBody(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    switch (node.kind) {
    case BtNodeKind::MoveTo: ReleaseMoveTo(c, node); break;
    case BtNodeKind::RotateTo: ReleaseRotateTo(c, node); break;
    case BtNodeKind::SearchArea: ReleaseSearchArea(c); break;
    case BtNodeKind::Patrol: ReleasePatrol(c); break;
    default: break;
    }
    const int extraBytes = BtNodeTypeOf(node.kind).extraStateBytes;
    if (extraBytes > 0) {
        std::memset(c.inst.extra.data() + node.extraOffset, 0, static_cast<size_t>(extraBytes));
    }
}

// 入るときの Decorator の初期化 (Repeat の周回ごとにも内側の分をやり直す)
void ArmDecorator(RunCtx& c, const BtDecoratorDef& deco)
{
    BtNodeState& slot = SlotOf(c, deco);
    slot.active = 1;
    switch (deco.kind) {
    case BtDecoratorKind::Repeat: slot.counter = 0; break;
    case BtDecoratorKind::Timeout: slot.counter = TickToFieldAt(c.tick, deco.params[0].i); break;
    default: break; // Cooldown の counter は前回の終了から持ち越す。BlackboardCondition の phase は入る前の評価で書いてある
    }
}

// ノードを抜けた (終了でも Abort でも) ときの Decorator の後始末。Cooldown はここで計時を始める。
// BlackboardCondition の phase (最後の結果) は監視の比較に要るので残す
void FinishDecorators(RunCtx& c, const BtNodeDef& node)
{
    for (const BtDecoratorDef& deco : node.decorators) {
        BtNodeState& slot = SlotOf(c, deco);
        switch (deco.kind) {
        case BtDecoratorKind::Cooldown:
            slot = BtNodeState{};
            slot.counter = TickToFieldAt(c.tick, deco.params[0].i);
            break;
        case BtDecoratorKind::BlackboardCondition: slot.active = 0; break;
        default: Finish(slot); break;
        }
    }
}

// 実行中のノードとその子孫を Abort する。子孫が先 (深い方から)。
// ノード固有の後始末 (MoveTo の停止など) は子孫の後・FinishDecorators と Finish の前でここへ足す
void AbortNode(RunCtx& c, int32_t index)
{
    if (!NodeActive(c.inst, c.tree, index)) {
        return;
    }
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    for (const int32_t child : node.children) {
        AbortNode(c, child);
    }
    if (c.abortTrace != nullptr) {
        c.abortTrace->push_back(node.id);
    }
    if (c.inst.nodes[static_cast<size_t>(index)].active != 0) {
        ReleaseBody(c, index); // ノード固有の後始末 (MoveTo の停止など)。子孫の後・Decorator の後始末の前
    }
    FinishDecorators(c, node);
    Finish(c.inst.nodes[static_cast<size_t>(index)]);
    c.aborted = true;
}

// Decorator は残して、ノード本体とその子孫だけを Abort する (Timeout の打ち切り用)
void AbortBody(RunCtx& c, int32_t index)
{
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    for (const int32_t child : node.children) {
        AbortNode(c, child);
    }
    if (state.active != 0) {
        if (c.abortTrace != nullptr) {
            c.abortTrace->push_back(node.id);
        }
        ReleaseBody(c, index);
        Finish(state);
        c.aborted = true;
    }
}

// Selector: 最初に Success した子で Success、全部 Failure で Failure。
// Sequence: 最初に Failure した子で Failure、全部 Success で Success。空の子は Selector = Failure、Sequence = Success
BtResult VisitSequenceOrSelector(RunCtx& c, int32_t index, bool isSelector)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    if (state.active == 0) {
        state.active = 1;
        state.child = 0;
    }
    const int32_t childCount = static_cast<int32_t>(node.children.size());
    while (state.child < childCount) {
        const BtResult result = Visit(c, node.children[static_cast<size_t>(state.child)]);
        if (result == BtResult::Running) {
            return BtResult::Running;
        }
        const bool decided = isSelector ? result == BtResult::Success : result == BtResult::Failure;
        if (decided) {
            Finish(state);
            return result;
        }
        ++state.child;
    }
    Finish(state);
    return isSelector ? BtResult::Failure : BtResult::Success;
}

// SimpleParallel: 子 0 = メインのタスク、子 1 = 背景。同じ tick に両方進める (メインが先)。結果はメインの結果。
// 背景が先に終わったら次の tick に背景を最初からやり直す
BtResult VisitSimpleParallel(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    if (state.active == 0) {
        state.active = 1;
        state.phase = 0;
        state.counter = 0;
    }
    const int32_t mainChild = node.children[0];
    const int32_t backgroundChild = node.children[1];
    const bool immediate = node.params[0].i == btparallelfinish::kImmediate;

    if ((state.phase & kParallelMainDone) == 0) {
        const BtResult result = Visit(c, mainChild);
        if (result != BtResult::Running) {
            state.phase |= kParallelMainDone;
            state.counter = result == BtResult::Success ? kParallelMainSuccess : kParallelMainFailure;
        }
    }
    const bool mainDone = (state.phase & kParallelMainDone) != 0;
    const BtResult mainResult = state.counter == kParallelMainSuccess ? BtResult::Success : BtResult::Failure;
    if (mainDone && immediate) {
        AbortNode(c, backgroundChild);
        Finish(state);
        return mainResult;
    }
    if (Visit(c, backgroundChild) != BtResult::Running && mainDone) {
        Finish(state);
        return mainResult;
    }
    return BtResult::Running;
}

// Wait: ticks ± randomDeviation tick 待って Success。入った tick から数えて ticks tick 後に終わる (0 以下は入った tick に終わる)。
// 偏差が 0 のときは World の RNG を引かない
BtResult VisitWait(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    if (state.active == 0) {
        state.active = 1;
        const int32_t ticks = node.params[0].i;
        const int32_t deviation = node.params[1].i;
        state.counter = deviation > 0 ? ticks + c.world.Rng().RangeInt(-deviation, deviation + 1) : ticks;
    } else {
        --state.counter;
    }
    if (state.counter <= 0) {
        Finish(state);
        return BtResult::Success;
    }
    return BtResult::Running;
}

float HorizontalDistance(const float* a, const float* b)
{
    const float dx = a[0] - b[0];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// 本体の終わり方 (Success / Failure)。後始末をして状態を初期値へ戻す
BtResult EndBody(RunCtx& c, int32_t index, BtResult result)
{
    ReleaseBody(c, index);
    Finish(c.inst.nodes[static_cast<size_t>(index)]);
    return result;
}

// MoveTo: 目標へ NavMeshAgent を歩かせる。目標 (BB のキー) が Vector / Entity、NavMeshAgent が無ければ Failure。
// 目的地を書いた tick の Nav はまだ走っていないので、その tick は status を読まない (前の目的地の Arrived が残っている)
BtResult VisitMoveTo(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    const bool starting = state.active == 0;
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    float target[3] = {};
    float self[3] = {};
    if (agent == nullptr || !ResolveTarget(c, node, target) || !WorldPositionOf(c.world, c.inst.entity, self)) {
        return starting ? BtResult::Failure : EndBody(c, index, BtResult::Failure);
    }
    const float acceptance = node.params[btmoveparam::kAcceptanceRadius].f;
    const auto writeDestination = [&]() {
        agent->destination = { target[0], target[1], target[2] };
        agent->hasDestination = true;
    };

    if (starting) {
        if (HorizontalDistance(self, target) <= acceptance) {
            return BtResult::Success; // もう着いている。Agent には何も書かない
        }
        BtMoveToState moveState;
        const uint64_t filter = node.params[btmoveparam::kNavFilter].u;
        if (filter != 0) {
            moveState.savedNavFilter = agent->navFilter.value;
            moveState.flags |= btmovetoflag::kNavFilterSwapped;
            agent->navFilter = AssetID{ filter };
        }
        std::memcpy(moveState.lastTarget, target, sizeof(target));
        StoreExtra(c, node, moveState);
        writeDestination();
        state.active = 1;
        return BtResult::Running;
    }

    BtMoveToState moveState = LoadExtra<BtMoveToState>(c, node);
    const float moved = std::sqrt((target[0] - moveState.lastTarget[0]) * (target[0] - moveState.lastTarget[0])
                                  + (target[1] - moveState.lastTarget[1]) * (target[1] - moveState.lastTarget[1])
                                  + (target[2] - moveState.lastTarget[2]) * (target[2] - moveState.lastTarget[2]));
    const bool rewrote = node.params[btmoveparam::kObserveTarget].i != 0 && moved >= kObserveMoveDistance;
    if (rewrote) {
        std::memcpy(moveState.lastTarget, target, sizeof(target));
        StoreExtra(c, node, moveState);
        writeDestination();
    }
    if (HorizontalDistance(self, target) <= acceptance) {
        return EndBody(c, index, BtResult::Success);
    }
    if (rewrote) {
        return BtResult::Running;
    }
    switch (agent->status) {
    case navagentstatus::kArrived: return EndBody(c, index, BtResult::Success);
    case navagentstatus::kNoPath:
    case navagentstatus::kInactive: return EndBody(c, index, BtResult::Failure);
    case navagentstatus::kStuck:
        return node.params[btmoveparam::kFailOnStuck].i != 0 ? EndBody(c, index, BtResult::Failure) : BtResult::Running;
    default: return BtResult::Running;
    }
}

// y 軸まわりの向き (ラジアン)。+Z を向いた状態が 0 (NavSystem の TurnToward と同じ定義)
float YawOf(const DirectX::XMFLOAT4& q)
{
    const float fx = 2.0f * (q.x * q.z + q.w * q.y);
    const float fz = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    return fx == 0.0f && fz == 0.0f ? 0.0f : std::atan2(fx, fz);
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

// RotateTo: 目標の方向へ LocalTransform を y 軸まわりに回す。実行中は Agent の updateRotation を預かって false にする。
// 親付きの向きは親の回転と合成されるので考慮しない (Nav の TurnToward と同じ割り切り)
BtResult VisitRotateTo(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    const bool starting = state.active == 0;
    LocalTransform* transform = c.world.GetComponent<LocalTransform>(c.inst.entity);
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    float target[3] = {};
    float self[3] = {};
    if (transform == nullptr || !ResolveTarget(c, node, target) || !WorldPositionOf(c.world, c.inst.entity, self)) {
        return starting ? BtResult::Failure : EndBody(c, index, BtResult::Failure);
    }
    if (starting) {
        BtRotateToState rotateState;
        if (agent != nullptr) {
            rotateState.flags = btrotatetoflag::kAgentHandled
                                | (agent->updateRotation ? btrotatetoflag::kSavedUpdateRotation : 0u);
            agent->updateRotation = false;
        }
        StoreExtra(c, node, rotateState);
        state.active = 1;
    }

    const float dx = target[0] - self[0];
    const float dz = target[2] - self[2];
    if (dx * dx + dz * dz <= kMinFacingDistance * kMinFacingDistance) {
        return EndBody(c, index, BtResult::Success); // 真上 / 同じ位置は向きが定まらない
    }
    float speedDeg = node.params[btrotateparam::kAngularSpeedDeg].f;
    if (speedDeg <= 0.0f) {
        speedDeg = agent != nullptr && agent->angularSpeedDeg > 0.0f ? agent->angularSpeedDeg : kDefaultAngularSpeedDeg;
    }
    const float tolerance = node.params[btrotateparam::kToleranceDeg].f * (kPi / 180.0f);
    const float current = YawOf(transform->rotation);
    const float delta = WrapPi(std::atan2(dx, dz) - current);
    float remaining = std::fabs(delta);
    if (remaining > tolerance) {
        const float step = (std::min)(remaining, speedDeg * (kPi / 180.0f) * kBtTickSeconds);
        const float yaw = current + (delta > 0.0f ? step : -step);
        transform->rotation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
        remaining -= step;
    }
    return remaining <= tolerance ? EndBody(c, index, BtResult::Success) : BtResult::Running;
}

// SetBlackboard: key へ値を書いて Success。型に合わない source・コピー元が無い / 未設定 / 型違いは Failure
BtResult VisitSetBlackboard(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BbType type = BbType::Bool;
    BbValue* destination = FindBb(c, node.keys[btnodekey::kTarget], type);
    if (destination == nullptr) {
        return BtResult::Failure;
    }
    BbValue value;
    value.isSet = 1;
    switch (node.params[btsetparam::kSource].i) {
    case btsetparam::kCopy: {
        BbType sourceType = BbType::Bool;
        const BbValue* source = FindBb(c, node.keys[btnodekey::kSourceKey], sourceType);
        if (source == nullptr || sourceType != type || source->isSet == 0) {
            return BtResult::Failure;
        }
        value = *source;
        break;
    }
    case btsetparam::kSelf:
        if (type == BbType::Entity) {
            value.entity = c.inst.entity;
        } else if (type == BbType::Vector) {
            float position[3] = {};
            if (!WorldPositionOf(c.world, c.inst.entity, position)) {
                return BtResult::Failure;
            }
            std::memcpy(value.v, position, sizeof(position));
        } else {
            return BtResult::Failure;
        }
        break;
    default: // 定数
        switch (type) {
        case BbType::Bool: value.i = node.params[btsetparam::kBoolValue].i != 0 ? 1 : 0; break;
        case BbType::Int: value.i = node.params[btsetparam::kIntValue].i; break;
        case BbType::Float: value.f = node.params[btsetparam::kFloatValue].f; break;
        case BbType::Vector:
            value.v[0] = node.params[btsetparam::kVectorX].f;
            value.v[1] = node.params[btsetparam::kVectorY].f;
            value.v[2] = node.params[btsetparam::kVectorZ].f;
            break;
        case BbType::Entity: return BtResult::Failure; // 定数のハンドルは無い (空にするのは ClearBlackboard)
        }
        break;
    }
    *destination = value;
    return BtResult::Success;
}

// ClearBlackboard: key を未設定へ戻して Success。キーが無ければ Failure
BtResult VisitClearBlackboard(RunCtx& c, int32_t index)
{
    BbType type = BbType::Bool;
    BbValue* destination = FindBb(c, c.tree.nodes[static_cast<size_t>(index)].keys[btnodekey::kTarget], type);
    if (destination == nullptr) {
        return BtResult::Failure;
    }
    *destination = BbValue{};
    return BtResult::Success;
}

// ---- AI ノード 4 種 (M85d) ----

// key の名前の Vector が書かれていればその値を out へ。BB が無い・名前が空・型違い・未設定は false
bool ReadVectorKey(RunCtx& c, const std::string& name, float (&out)[3])
{
    BbType type = BbType::Bool;
    const BbValue* value = FindBb(c, name, type);
    if (value == nullptr || type != BbType::Vector || value->isSet == 0) {
        return false;
    }
    std::memcpy(out, value->v, sizeof(out));
    return true;
}

// key が wanted 型のキーなら書き先の値を返す。無い・型違いは nullptr
BbValue* FindBbOfType(RunCtx& c, const std::string& name, BbType wanted)
{
    BbType type = BbType::Bool;
    BbValue* value = FindBb(c, name, type);
    return value != nullptr && type == wanted ? value : nullptr;
}

float DistanceSquared3(const float* a, const float* b)
{
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

// 距離の小さい順、同じなら entity キーの小さい順
bool CloserThan(float distance, EntityID entity, float bestDistance, EntityID bestEntity)
{
    return distance < bestDistance || (distance == bestDistance && KeyLess(entity, bestEntity));
}

// FindRandomPoint: 中心 (自分 / Vector キー) の radius 内でナビメッシュに乗る点を Vector キーへ書く。
// Nav・NavMeshAgent・書き先・中心のどれかが無い、点が見つからないときは Failure
BtResult VisitFindRandomPoint(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    const NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    BbValue* result = FindBbOfType(c, node.keys[btrandomkey::kResult], BbType::Vector);
    if (c.nav == nullptr || agent == nullptr || result == nullptr) {
        return BtResult::Failure;
    }
    float center[3] = {};
    const std::string& centerKey = node.keys[btrandomkey::kCenter];
    if (centerKey.empty() ? !WorldPositionOf(c.world, c.inst.entity, center) : !ReadVectorKey(c, centerKey, center)) {
        return BtResult::Failure;
    }
    float point[3] = {};
    if (!c.nav->QueryRandomPoint(c.world, agent->agentTypeId, center, node.params[btrandomparam::kRadius].f, agent->areaMask,
                                 agent->navFilter.value, c.world.Rng(), point)) {
        return BtResult::Failure;
    }
    *result = BbValue{};
    result->isSet = 1;
    std::memcpy(result->v, point, sizeof(point));
    return BtResult::Success;
}

// FindNearestTarget: 自分の percepts のうち sense に合う相手で lastSensedPos が一番近いものを書く。
// 同じ tick の知覚の結果を読む (BT はフェーズ 3.4a2 = 知覚の後)。名乗らない音 (target = null) は Entity キーを空にして Vector だけ書く
BtResult VisitFindNearestTarget(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    const AIPerceptionComponent* perception = c.world.GetComponent<AIPerceptionComponent>(c.inst.entity);
    BbValue* targetOut = FindBbOfType(c, node.keys[btnearestkey::kTarget], BbType::Entity);
    const std::string& positionKey = node.keys[btnearestkey::kPosition];
    BbValue* positionOut = positionKey.empty() ? nullptr : FindBbOfType(c, positionKey, BbType::Vector);
    float self[3] = {};
    if (perception == nullptr || targetOut == nullptr || (!positionKey.empty() && positionOut == nullptr)
        || !WorldPositionOf(c.world, c.inst.entity, self)) {
        return BtResult::Failure;
    }
    uint32_t senseMask = 0;
    senseMask |= node.params[btnearestparam::kSight].i != 0 ? perceptionsense::kSight : 0u;
    senseMask |= node.params[btnearestparam::kHearing].i != 0 ? perceptionsense::kHearing : 0u;
    senseMask |= node.params[btnearestparam::kDamage].i != 0 ? perceptionsense::kDamage : 0u;
    senseMask |= node.params[btnearestparam::kTouch].i != 0 ? perceptionsense::kTouch : 0u;
    const bool currentOnly = node.params[btnearestparam::kCurrentOnly].i != 0;

    const AIPercept* best = nullptr;
    float bestDistance = 0.0f;
    const int32_t count = (std::min)(perception->perceivedCount, kMaxPercepts);
    for (int32_t i = 0; i < count; ++i) {
        const AIPercept& percept = perception->percepts[i];
        const uint32_t senses = currentOnly ? percept.currentSenses : percept.lastSenses;
        if ((senses & senseMask) == 0 || (!percept.target.IsNull() && !c.world.IsAlive(percept.target))) {
            continue;
        }
        const float sensedAt[3] = { percept.lastSensedPos.x, percept.lastSensedPos.y, percept.lastSensedPos.z };
        const float distance = DistanceSquared3(self, sensedAt);
        if (best == nullptr || CloserThan(distance, percept.target, bestDistance, best->target)) {
            best = &percept;
            bestDistance = distance;
        }
    }
    if (best == nullptr) {
        return BtResult::Failure;
    }
    *targetOut = BbValue{};
    if (!best->target.IsNull()) {
        targetOut->isSet = 1;
        targetOut->entity = best->target;
    }
    if (positionOut != nullptr) {
        *positionOut = BbValue{};
        positionOut->isSet = 1;
        positionOut->v[0] = best->lastSensedPos.x;
        positionOut->v[1] = best->lastSensedPos.y;
        positionOut->v[2] = best->lastSensedPos.z;
    }
    return BtResult::Success;
}

// FindTarget: AIStimulusSource を持つ有効なエンティティのうち、陣営 (自分の AIPerception の態度)・タグ・範囲に合う一番近いものを書く。
// 見えているかは問わない。自分は除く
BtResult VisitFindTarget(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    const AIPerceptionComponent* perception = c.world.GetComponent<AIPerceptionComponent>(c.inst.entity);
    BbValue* targetOut = FindBbOfType(c, node.keys[btfindtargetkey::kTarget], BbType::Entity);
    float self[3] = {};
    if (perception == nullptr || targetOut == nullptr || !WorldPositionOf(c.world, c.inst.entity, self)) {
        return BtResult::Failure;
    }
    const float radius = node.params[btfindtargetparam::kRadius].f;
    const uint64_t tagMask = node.params[btfindtargetparam::kTagMask].u;
    std::vector<std::pair<EntityID, int32_t>> sources; // (相手, 陣営)
    {
        const ComponentTypeId req[] = { AIStimulusSourceComponent::sTypeId };
        c.world.ForEachArchetype(req, [&](Archetype& arch) {
            const int sourceIndex = arch.FindTypeIndex(AIStimulusSourceComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const auto* source = static_cast<const AIStimulusSourceComponent*>(arch.GetPtr(sourceIndex, row));
                sources.emplace_back(arch.EntityAt(row), source->faction);
            }
        });
    }
    EntityID best = kNullEntity;
    float bestDistance = 0.0f;
    for (const auto& [entity, faction] : sources) {
        if (entity == c.inst.entity || !IsEntityActive(c.world, entity)) {
            continue;
        }
        bool allowed = false;
        switch (PerceptionAttitudeOf(*perception, faction)) {
        case PerceptionAttitude::Hostile: allowed = node.params[btfindtargetparam::kEnemies].i != 0; break;
        case PerceptionAttitude::Neutral: allowed = node.params[btfindtargetparam::kNeutrals].i != 0; break;
        case PerceptionAttitude::Friendly: allowed = node.params[btfindtargetparam::kFriendlies].i != 0; break;
        }
        float at[3] = {};
        if (!allowed || !Tags::PassesFilter(Tags::OwnMask(c.world, entity), tagMask) || !WorldPositionOf(c.world, entity, at)) {
            continue;
        }
        const float distance = DistanceSquared3(self, at);
        if (distance > radius * radius) {
            continue;
        }
        if (best.IsNull() || CloserThan(distance, entity, bestDistance, best)) {
            best = entity;
            bestDistance = distance;
        }
    }
    if (best.IsNull()) {
        return BtResult::Failure;
    }
    *targetOut = BbValue{};
    targetOut->isSet = 1;
    targetOut->entity = best;
    return BtResult::Success;
}

// 自分の知覚の中の target の項目。無ければ nullptr
const AIPercept* FindPercept(const RunCtx& c, EntityID target)
{
    const AIPerceptionComponent* perception = c.world.GetComponent<AIPerceptionComponent>(c.inst.entity);
    if (perception == nullptr || target.IsNull()) {
        return nullptr;
    }
    const int32_t count = (std::min)(perception->perceivedCount, kMaxPercepts);
    for (int32_t i = 0; i < count; ++i) {
        if (perception->percepts[i].target == target) {
            return &perception->percepts[i];
        }
    }
    return nullptr;
}

// SearchArea: 起点 (吸着済み) へ向かい、着いたら起点の radius 内の点を pointCount 個順に回る。
// 点は向かい始める時に 1 つずつ FindRandomPoint と同じ方法で生成する (RNG もその時に引く = 状態は固定長)。
// 終了キーの相手が今視覚で見えたら Success、全部回ったら Failure。起点が引けない・届かない (NoPath) は Failure、
// 回る点が届かない・見つからないときはその点を飛ばす。目的地を書いた tick は status を読まない
BtResult VisitSearchArea(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    const bool starting = state.active == 0;
    const auto finish = [&](BtResult result) { return starting ? result : EndBody(c, index, result); };
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    float self[3] = {};
    if (c.nav == nullptr || agent == nullptr || !WorldPositionOf(c.world, c.inst.entity, self)) {
        return finish(BtResult::Failure);
    }

    EntityID endTarget = kNullEntity;
    const BbValue* endValue = FindBbOfType(c, node.keys[btsearchkey::kEndTarget], BbType::Entity);
    if (endValue != nullptr && endValue->isSet != 0 && c.world.IsAlive(endValue->entity)) {
        endTarget = endValue->entity;
    }
    const AIPercept* endPercept = FindPercept(c, endTarget);
    if (endPercept != nullptr && (endPercept->currentSenses & perceptionsense::kSight) != 0) {
        return finish(BtResult::Success);
    }

    BtSearchAreaState search;
    if (starting) {
        float raw[3] = {};
        bool haveOrigin = false;
        if (node.params[btsearchparam::kUsePrediction].i != 0 && endPercept != nullptr) {
            raw[0] = endPercept->predictedPos.x;
            raw[1] = endPercept->predictedPos.y;
            raw[2] = endPercept->predictedPos.z;
            haveOrigin = true;
        }
        haveOrigin = haveOrigin || ReadVectorKey(c, node.keys[btsearchkey::kOrigin], raw);
        const float extents[3] = { kSearchSnapHorizontal, kSearchSnapVertical, kSearchSnapHorizontal };
        if (!haveOrigin
            || !c.nav->QuerySamplePosition(c.world, agent->agentTypeId, raw, extents, agent->areaMask, agent->navFilter.value,
                                           search.origin)) {
            return BtResult::Failure;
        }
        std::memcpy(search.current, search.origin, sizeof(search.origin));
        search.remaining = node.params[btsearchparam::kPointCount].i;
        search.phase = btsearchphase::kToOrigin;
        state.active = 1;
    } else {
        search = LoadExtra<BtSearchAreaState>(c, node);
    }

    // 今向かっている点に着いたか。status は前の tick に目的地を書いたときだけ読める (始めた tick は読まない)
    bool arrived = HorizontalDistance(self, search.current) <= kSearchAcceptanceRadius;
    if (!starting && !arrived) {
        switch (agent->status) {
        case navagentstatus::kArrived: arrived = true; break;
        case navagentstatus::kNoPath:
        case navagentstatus::kInactive:
            if (search.phase == btsearchphase::kToOrigin) {
                return EndBody(c, index, BtResult::Failure);
            }
            arrived = true; // 届かない点は飛ばす
            break;
        case navagentstatus::kStuck:
            if (node.params[btsearchparam::kFailOnStuck].i != 0) {
                return EndBody(c, index, BtResult::Failure);
            }
            break;
        default: break;
        }
    }
    const auto writeDestination = [&](const float* point) {
        agent->destination = { point[0], point[1], point[2] };
        agent->hasDestination = true;
    };
    if (!arrived) {
        if (starting) {
            writeDestination(search.current);
        }
        StoreExtra(c, node, search);
        return BtResult::Running;
    }

    if (search.phase == btsearchphase::kToPoint && search.remaining <= 0) {
        return EndBody(c, index, BtResult::Failure); // 回り切った
    }
    search.phase = btsearchphase::kToPoint;
    --search.remaining;
    float point[3] = {};
    if (c.nav->QueryRandomPoint(c.world, agent->agentTypeId, search.origin, node.params[btsearchparam::kRadius].f, agent->areaMask,
                                agent->navFilter.value, c.world.Rng(), point)) {
        std::memcpy(search.current, point, sizeof(point));
        writeDestination(point);
    } else if (search.remaining <= 0) {
        return EndBody(c, index, BtResult::Failure); // 最後の点が見つからない
    }
    StoreExtra(c, node, search);
    return BtResult::Running;
}

// ルートのエンティティと PatrolRoute。キーが Entity 型で、生きていて、点が 1 つ以上ある PatrolRoute を持つときだけ返す
const PatrolRouteComponent* ResolveRoute(RunCtx& c, const BtNodeDef& node, EntityID& routeEntity)
{
    const BbValue* value = FindBbOfType(c, node.keys[btpatrolkey::kRoute], BbType::Entity);
    if (value == nullptr || value->isSet == 0 || !c.world.IsAlive(value->entity)) {
        return nullptr;
    }
    const PatrolRouteComponent* route = c.world.GetComponent<PatrolRouteComponent>(value->entity);
    if (route == nullptr || route->pointCount <= 0) {
        return nullptr;
    }
    routeEntity = value->entity;
    return route;
}

// ルートの点のワールド位置 (ワールド行列 x ローカル座標)。親が無ければ LocalTransform から組む
// (WorldMatrix は 1 tick 遅れで、置いた直後の tick が古い値になるため。WorldPositionOf と同じ規則)。得られなければ false
bool PatrolPointWorld(World& world, EntityID routeEntity, const PatrolRouteComponent& route, int32_t index, float (&out)[3])
{
    DirectX::XMFLOAT4X4 matrix;
    if (world.GetParent(routeEntity) == kNullEntity) {
        const LocalTransform* transform = world.GetComponent<LocalTransform>(routeEntity);
        if (transform == nullptr) {
            return false;
        }
        DirectX::XMStoreFloat4x4(&matrix,
                                 DirectX::XMMatrixScaling(transform->scale.x, transform->scale.y, transform->scale.z)
                                     * DirectX::XMMatrixRotationQuaternion(DirectX::XMLoadFloat4(&transform->rotation))
                                     * DirectX::XMMatrixTranslation(transform->position.x, transform->position.y, transform->position.z));
    } else {
        const WorldMatrixComponent* worldMatrix = world.GetComponent<WorldMatrixComponent>(routeEntity);
        if (worldMatrix == nullptr) {
            return false;
        }
        matrix = worldMatrix->value;
    }
    const DirectX::XMFLOAT3& p = route.points[index];
    out[0] = p.x * matrix.m[0][0] + p.y * matrix.m[1][0] + p.z * matrix.m[2][0] + matrix.m[3][0];
    out[1] = p.x * matrix.m[0][1] + p.y * matrix.m[1][1] + p.z * matrix.m[2][1] + matrix.m[3][1];
    out[2] = p.x * matrix.m[0][2] + p.y * matrix.m[1][2] + p.z * matrix.m[2][2] + matrix.m[3][2];
    return true;
}

// 点 index の次の点。Loop は一周、PingPong は端で折り返す (点が 1 つなら動かない)。Once は最後の点の先を呼び出し側が打ち切る
int32_t NextPatrolIndex(int32_t mode, int32_t count, int32_t index, int32_t& direction)
{
    if (mode == patrolmode::kPingPong) {
        if (count <= 1) {
            return 0;
        }
        int32_t next = index + direction;
        if (next < 0 || next >= count) {
            direction = -direction;
            next = index + direction;
        }
        return next;
    }
    return (index + 1) % count;
}

// Patrol: ルートの点を順に回る。入るたびに (Abort からの復帰も) 今いる位置から水平に一番近い点から始める (同距離は index 小)。
// 点へ向かい (Arrived・acceptanceRadius 内)、点の waitTicks を待ってから次の点へ。Once は最後の点で待ち終えたら Success、
// Loop / PingPong は終わらない。ルートが無い・点が 0 個・NavMeshAgent が無い・届かない (NoPath) は Failure、Stuck は failOnStuck 次第。
// 1 tick に進めるのは 1 段 (着く → 待つ → 次を書く を同じ tick に重ねない)。目的地を書いた tick は status を読まない
BtResult VisitPatrol(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    const bool starting = state.active == 0;
    NavMeshAgentComponent* agent = c.world.GetComponent<NavMeshAgentComponent>(c.inst.entity);
    EntityID routeEntity = kNullEntity;
    const PatrolRouteComponent* route = ResolveRoute(c, node, routeEntity);
    float self[3] = {};
    if (agent == nullptr || route == nullptr || !WorldPositionOf(c.world, c.inst.entity, self)) {
        return starting ? BtResult::Failure : EndBody(c, index, BtResult::Failure);
    }
    const int32_t count = (std::min)(route->pointCount, kMaxPatrolPoints);
    const float acceptance = node.params[btpatrolparam::kAcceptanceRadius].f;

    BtPatrolState patrol;
    if (starting) {
        int32_t best = -1;
        float bestDistance = 0.0f;
        for (int32_t i = 0; i < count; ++i) {
            float point[3] = {};
            if (!PatrolPointWorld(c.world, routeEntity, *route, i, point)) {
                return BtResult::Failure;
            }
            const float distance = HorizontalDistance(self, point);
            if (best < 0 || distance < bestDistance) {
                best = i;
                bestDistance = distance;
            }
        }
        patrol.nextIndex = best;
        state.active = 1;
    } else {
        patrol = LoadExtra<BtPatrolState>(c, node);
    }
    if (patrol.nextIndex < 0 || patrol.nextIndex >= count) {
        return EndBody(c, index, BtResult::Failure); // 実行中にルートの点が減った
    }

    float target[3] = {};
    if (!PatrolPointWorld(c.world, routeEntity, *route, patrol.nextIndex, target)) {
        return EndBody(c, index, BtResult::Failure);
    }
    const auto writeDestination = [&](const float* point) {
        agent->destination = { point[0], point[1], point[2] };
        agent->hasDestination = true;
    };

    if (patrol.phase == btpatrolphase::kMoving) {
        bool arrived = HorizontalDistance(self, target) <= acceptance;
        if (!starting && !arrived) {
            switch (agent->status) {
            case navagentstatus::kArrived: arrived = true; break;
            case navagentstatus::kNoPath:
            case navagentstatus::kInactive: return EndBody(c, index, BtResult::Failure);
            case navagentstatus::kStuck:
                if (node.params[btpatrolparam::kFailOnStuck].i != 0) {
                    return EndBody(c, index, BtResult::Failure);
                }
                break;
            default: break;
            }
        }
        if (!arrived) {
            if (starting) {
                writeDestination(target);
            }
            StoreExtra(c, node, patrol);
            return BtResult::Running;
        }
        patrol.phase = btpatrolphase::kWaiting;
        patrol.waitRemaining = (std::max)(route->waitTicks[patrol.nextIndex], 0);
    } else {
        --patrol.waitRemaining;
    }
    if (patrol.waitRemaining > 0) {
        StoreExtra(c, node, patrol);
        return BtResult::Running;
    }

    if (route->mode == patrolmode::kOnce && patrol.nextIndex == count - 1) {
        return EndBody(c, index, BtResult::Success);
    }
    patrol.nextIndex = NextPatrolIndex(route->mode, count, patrol.nextIndex, patrol.direction);
    patrol.phase = btpatrolphase::kMoving;
    PatrolPointWorld(c.world, routeEntity, *route, patrol.nextIndex, target);
    if (HorizontalDistance(self, target) > acceptance) {
        writeDestination(target);
    }
    StoreExtra(c, node, patrol);
    return BtResult::Running;
}

// SendEvent: イベントを積んで Success (配るのは次の tick)。名前が空・宛先の Entity が読めない・ペイロードの Vector が
// 読めない (キーを指定したのに未設定 / 型違い) は Failure。キューが溢れて捨てられても Success (警告は SendEvent が出す)
BtResult VisitSendEvent(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    const std::string& name = node.params[btsendparam::kEventName].s;
    if (name.empty() || c.events == nullptr) {
        return BtResult::Failure;
    }
    EntityID target = kNullEntity;
    switch (node.params[btsendparam::kTarget].i) {
    case btsendtarget::kSelf: target = c.inst.entity; break;
    case btsendtarget::kEntity: {
        const BbValue* value = FindBbOfType(c, node.keys[btsendkey::kTarget], BbType::Entity);
        if (value == nullptr || value->isSet == 0 || !c.world.IsAlive(value->entity)) {
            return BtResult::Failure;
        }
        target = value->entity;
        break;
    }
    default: break; // 全体宛て
    }
    float vec3[3] = {};
    const std::string& vectorKey = node.keys[btsendkey::kVector];
    if (!vectorKey.empty() && !ReadVectorKey(c, vectorKey, vec3)) {
        return BtResult::Failure;
    }
    c.events->SendEvent(c.tick, c.inst.entity, target, BtEventNameHash(name), vec3, node.params[btsendparam::kFloatValue].f,
                        node.params[btsendparam::kIntValue].i);
    return BtResult::Success;
}

// ステートのクリップが 1 周するのにかかる tick 数。クリップが無い・長さ 0 は 0 (待つものが無い)
int32_t PlayLengthTicks(const RunCtx& c, const ControllerState& stateDef)
{
    const AnimationClipAsset* clip = c.clips != nullptr ? c.clips->Get(stateDef.clipHash) : nullptr;
    if (clip == nullptr || clip->lengthTicks <= 0) {
        return 0;
    }
    const int32_t speed = (std::max)(stateDef.speed, 1); // 停止・逆再生のステートは 1 周が来ないので等速とみなす
    return (clip->lengthTicks + speed - 1) / speed;
}

// PlayAnimation: Animator のステートへ AnimatorPlay で遷移を始める。Animator・controller・ステート名のどれかが無ければ Failure。
// waitForEnd なら入った tick から数えてクリップ 1 周ぶんの tick 後に Success (Wait と同じ数え方)、でなければ即 Success。
// Abort しても再生は止めない (Animator には戻すものが無い)
BtResult VisitPlayAnimation(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    if (state.active != 0) {
        --state.counter;
        if (state.counter <= 0) {
            Finish(state);
            return BtResult::Success;
        }
        return BtResult::Running;
    }
    const std::string& name = node.params[btplayparam::kState].s;
    const AnimatorControllerComponent* animator = c.world.GetComponent<AnimatorControllerComponent>(c.inst.entity);
    const ControllerAsset* controller =
        (animator != nullptr && c.controllers != nullptr) ? c.controllers->Get(animator->controller.value) : nullptr;
    const int32_t stateIndex = (controller != nullptr && !name.empty()) ? FindControllerState(*controller, name) : -1;
    if (stateIndex < 0
        || !AnimatorPlay(c.world, c.inst.entity, stateIndex, node.params[btplayparam::kDurationTicks].i, *c.controllers)) {
        return BtResult::Failure;
    }
    if (node.params[btplayparam::kWaitForEnd].i == 0) {
        return BtResult::Success;
    }
    const int32_t waitTicks = PlayLengthTicks(c, controller->states[static_cast<size_t>(stateIndex)]);
    if (waitTicks <= 0) {
        return BtResult::Success;
    }
    state.active = 1;
    state.counter = waitTicks;
    return BtResult::Running;
}

// SubTree: 取り込まれた部分木 (子 1 つ) を訪れて結果をそのまま返す。子が無い = 取り込めなかった (BtExpandSubTrees が警告済み) ので Failure
BtResult VisitSubTree(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    if (node.children.empty()) {
        return BtResult::Failure;
    }
    state.active = 1;
    const BtResult result = Visit(c, node.children[0]);
    if (result != BtResult::Running) {
        Finish(state);
    }
    return result;
}

BtResult VisitBody(RunCtx& c, int32_t index)
{
    switch (c.tree.nodes[static_cast<size_t>(index)].kind) {
    case BtNodeKind::Selector: return VisitSequenceOrSelector(c, index, true);
    case BtNodeKind::Sequence: return VisitSequenceOrSelector(c, index, false);
    case BtNodeKind::SimpleParallel: return VisitSimpleParallel(c, index);
    case BtNodeKind::Wait: return VisitWait(c, index);
    case BtNodeKind::MoveTo: return VisitMoveTo(c, index);
    case BtNodeKind::RotateTo: return VisitRotateTo(c, index);
    case BtNodeKind::SetBlackboard: return VisitSetBlackboard(c, index);
    case BtNodeKind::ClearBlackboard: return VisitClearBlackboard(c, index);
    case BtNodeKind::FindRandomPoint: return VisitFindRandomPoint(c, index);
    case BtNodeKind::FindNearestTarget: return VisitFindNearestTarget(c, index);
    case BtNodeKind::SearchArea: return VisitSearchArea(c, index);
    case BtNodeKind::FindTarget: return VisitFindTarget(c, index);
    case BtNodeKind::SendEvent: return VisitSendEvent(c, index);
    case BtNodeKind::PlayAnimation: return VisitPlayAnimation(c, index);
    case BtNodeKind::SubTree: return VisitSubTree(c, index);
    case BtNodeKind::Patrol: return VisitPatrol(c, index);
    case BtNodeKind::Count: break;
    }
    return BtResult::Failure;
}

// BlackboardCondition の今の真偽。キーが無い・BB が無い・未設定のまま大小を比べる・Vector / Entity の大小比較は偽。
// IsSet: Bool は true、Entity は生きているハンドル、Vector / Int / Float は値が書かれている
bool EvalBlackboardCondition(const RunCtx& c, const BtDecoratorDef& deco)
{
    if (!c.inst.blackboardAsset) {
        return false;
    }
    const int key = c.inst.blackboardAsset->FindKey(deco.key);
    if (key < 0 || static_cast<size_t>(key) >= c.inst.blackboard.size()) {
        return false;
    }
    const BbType type = c.inst.blackboardAsset->keys[static_cast<size_t>(key)].type;
    const BbValue& value = c.inst.blackboard[static_cast<size_t>(key)];
    const bool isSet = value.isSet != 0;
    bool truthy = isSet;
    if (type == BbType::Bool) {
        truthy = isSet && value.i != 0;
    } else if (type == BbType::Entity) {
        truthy = isSet && c.world.IsAlive(value.entity);
    }
    const int32_t query = deco.params[btbbparam::kQuery].i;
    if (query == btquery::kIsSet) {
        return truthy;
    }
    if (query == btquery::kIsNotSet) {
        return !truthy;
    }
    if (!isSet || type == BbType::Vector || type == BbType::Entity) {
        return false;
    }
    const auto compare = [query](auto lhs, auto rhs) {
        switch (query) {
        case btquery::kEqual: return lhs == rhs;
        case btquery::kNotEqual: return lhs != rhs;
        case btquery::kLess: return lhs < rhs;
        case btquery::kLessEqual: return lhs <= rhs;
        case btquery::kGreater: return lhs > rhs;
        case btquery::kGreaterEqual: return lhs >= rhs;
        default: return false;
        }
    };
    return type == BbType::Float ? compare(value.f, deco.params[btbbparam::kFloatValue].f)
                                 : compare(value.i, deco.params[btbbparam::kIntValue].i);
}

// 入れる条件になる Decorator (BlackboardCondition / Cooldown) の今の真偽。それ以外は常に真
bool EvalCondition(const RunCtx& c, const BtDecoratorDef& deco)
{
    switch (deco.kind) {
    case BtDecoratorKind::BlackboardCondition: return EvalBlackboardCondition(c, deco);
    case BtDecoratorKind::Cooldown: return c.tick >= static_cast<uint64_t>(c.inst.nodes[static_cast<size_t>(deco.slot)].counter);
    default: return true;
    }
}

bool AllConditionsPass(const RunCtx& c, const BtNodeDef& node)
{
    return std::all_of(node.decorators.begin(), node.decorators.end(),
                       [&c](const BtDecoratorDef& deco) { return EvalCondition(c, deco); });
}

// ノードに入れるか。全部評価する (打ち切らない) — BlackboardCondition の最後の結果を必ず更新するため
bool CheckEntry(RunCtx& c, const BtNodeDef& node)
{
    bool pass = true;
    for (const BtDecoratorDef& deco : node.decorators) {
        const bool ok = EvalCondition(c, deco);
        if (deco.kind == BtDecoratorKind::BlackboardCondition) {
            SlotOf(c, deco).phase = ok ? 1 : 0;
        }
        pass = pass && ok;
    }
    return pass;
}

// level 番目より内側の Decorator と本体を進める。外側から順に包む (最初の Decorator が一番外側)
BtResult RunLevel(RunCtx& c, int32_t index, size_t level)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    if (level >= node.decorators.size()) {
        return VisitBody(c, index);
    }
    const BtDecoratorDef& deco = node.decorators[level];
    BtNodeState& slot = SlotOf(c, deco);
    switch (deco.kind) {
    case BtDecoratorKind::Invert: {
        const BtResult result = RunLevel(c, index, level + 1);
        return result == BtResult::Running ? result : (result == BtResult::Success ? BtResult::Failure : BtResult::Success);
    }
    case BtDecoratorKind::Timeout: {
        // 打ち切る tick に子がまだ終わっていなければ Abort して Failure (その tick に終わるなら終わりを優先する)
        const BtResult result = RunLevel(c, index, level + 1);
        if (result == BtResult::Running && c.tick >= static_cast<uint64_t>(slot.counter)) {
            RecordDecoratorAbort(c, node, index);
            AbortBody(c, index);
            return BtResult::Failure;
        }
        return result;
    }
    case BtDecoratorKind::Repeat: {
        const int32_t count = deco.params[0].i; // 0 = 無限
        for (;;) {
            const BtResult result = RunLevel(c, index, level + 1);
            if (result != BtResult::Success) {
                return result; // Running のまま待つか、Failure で抜ける
            }
            ++slot.counter;
            if (count > 0 && slot.counter >= count) {
                return BtResult::Success;
            }
            // 次の周回。1 周ごとに手数を 1 つ使い、上限なら次の tick に続ける
            if (c.steps >= kBtMaxStepsPerTick) {
                c.stepLimitHit = true;
                return BtResult::Running;
            }
            ++c.steps;
            for (size_t inner = level + 1; inner < node.decorators.size(); ++inner) {
                ArmDecorator(c, node.decorators[inner]);
            }
        }
    }
    case BtDecoratorKind::BlackboardCondition:
    case BtDecoratorKind::Cooldown:
    case BtDecoratorKind::Count: break; // 条件は入るときに評価済み
    }
    return RunLevel(c, index, level + 1);
}

// Decorator の付いたノードの訪問。入るときに条件を評価し、偽なら状態に触れず Failure
BtResult VisitDecorated(RunCtx& c, int32_t index)
{
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    if (!NodeActive(c.inst, c.tree, index)) {
        if (!CheckEntry(c, node)) {
            return BtResult::Failure;
        }
        for (const BtDecoratorDef& deco : node.decorators) {
            ArmDecorator(c, deco);
        }
    }
    const BtResult result = RunLevel(c, index, 0);
    if (result != BtResult::Running) {
        FinishDecorators(c, node);
    }
    return result;
}

BtResult Visit(RunCtx& c, int32_t index)
{
    if (c.steps >= kBtMaxStepsPerTick) {
        c.stepLimitHit = true; // 手を付けずに Running で返す。入っていないノードは次の tick に親が入り直す
        return BtResult::Running;
    }
    ++c.steps;
    return c.tree.nodes[static_cast<size_t>(index)].decorators.empty() ? VisitBody(c, index) : VisitDecorated(c, index);
}

// BlackboardCondition の abort がこのノードの部分木に効くか。LowerPriority / Both は親が Selector のときだけ兄弟へ効き、
// そうでなければ Self として扱う
bool ObservesSelf(const BtNodeDef& node, const BehaviorTreeAsset& tree, int32_t abort)
{
    const bool parentIsSelector = node.parent >= 0 && tree.nodes[static_cast<size_t>(node.parent)].kind == BtNodeKind::Selector;
    return abort == btabort::kSelf || abort == btabort::kBoth || (abort == btabort::kLowerPriority && !parentIsSelector);
}

// 毎 tick の監視 (木の前順)。条件の変化で Abort を決める。Abort しただけで、結果は同じ tick の Visit が返す
// (Self で止めたノードは入り直し → 条件が偽で Failure、LowerPriority で戻した Selector は優先側の子から入り直す)
void MonitorNode(RunCtx& c, int32_t index)
{
    if (!NodeActive(c.inst, c.tree, index)) {
        return;
    }
    const BtNodeDef& node = c.tree.nodes[static_cast<size_t>(index)];
    for (const BtDecoratorDef& deco : node.decorators) {
        if (deco.kind == BtDecoratorKind::BlackboardCondition && ObservesSelf(node, c.tree, deco.params[btbbparam::kAbort].i)) {
            const bool now = EvalBlackboardCondition(c, deco);
            SlotOf(c, deco).phase = now ? 1 : 0;
            if (!now) {
                RecordDecoratorAbort(c, node, index);
                AbortNode(c, index);
                return;
            }
        }
    }
    if (node.kind == BtNodeKind::Selector) {
        // 実行中の子より左 (優先度が高い) の子の条件が偽から真へ変わったら、右の実行中の子を Abort してそこから入り直す
        BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
        for (int32_t k = 0; k < state.child && static_cast<size_t>(k) < node.children.size(); ++k) {
            const BtNodeDef& sibling = c.tree.nodes[static_cast<size_t>(node.children[static_cast<size_t>(k)])];
            bool fired = false;
            for (const BtDecoratorDef& deco : sibling.decorators) {
                const int32_t abort = deco.kind == BtDecoratorKind::BlackboardCondition ? deco.params[btbbparam::kAbort].i : btabort::kNone;
                if (abort != btabort::kLowerPriority && abort != btabort::kBoth) {
                    continue;
                }
                BtNodeState& slot = SlotOf(c, deco);
                const bool now = EvalBlackboardCondition(c, deco);
                const bool was = slot.phase != 0;
                if (now && !was && !AllConditionsPass(c, sibling)) {
                    continue; // 他の条件が通らない間は変化を見送る (通ったときに改めて変化として拾う)
                }
                slot.phase = now ? 1 : 0;
                if (now && !was) {
                    fired = true;
                }
            }
            if (fired) {
                RecordDecoratorAbort(c, sibling, node.children[static_cast<size_t>(state.child)]);
                AbortNode(c, node.children[static_cast<size_t>(state.child)]);
                state.child = k;
                break;
            }
        }
    }
    for (const int32_t child : node.children) {
        MonitorNode(c, child);
    }
}

// 実行中の一番深いノードの id。何も実行していなければ -1
int32_t ActiveLeafId(const BtInstance& inst, const BehaviorTreeAsset& tree)
{
    return ActiveLeafIdFrom(inst, tree, tree.rootIndex);
}

bool AnyNodeActive(const BtInstance& inst)
{
    return std::any_of(inst.nodes.begin(), inst.nodes.end(), [](const BtNodeState& s) { return s.active != 0; });
}

// ノードの状態を全部初期値にする (木の形に合わせて長さも直す)
void ResetNodes(BtInstance& inst)
{
    inst.nodes.assign(static_cast<size_t>(inst.tree->stateSlotCount), BtNodeState{});
    inst.extra.assign(static_cast<size_t>(inst.tree->extraStateBytes), 0);
}

// ブラックボードを初期値にする。BB を使わない木は空
void InitBlackboard(BtInstance& inst)
{
    inst.blackboard.clear();
    if (inst.blackboardAsset) {
        for (const BbKeyDef& key : inst.blackboardAsset->keys) {
            inst.blackboard.push_back(key.initial);
        }
    }
}

// BehaviorTreeComponent の Entity キーの初期値を BB へ書く。BB に無い名前・Entity 型でないキー・null の値の組は書かない
// (無効な組は warnedEntityInit に載せて、同じ組の警告を 1 回に抑える)
void ApplyEntityInitials(World& world, const BehaviorTreeComponent& comp, BtInstance& inst,
                         std::set<std::pair<uint64_t, int>>& warnedEntityInit)
{
    if (!inst.blackboardAsset) {
        return;
    }
    for (int i = 0; i < kBtEntityInitialCount; ++i) {
        const size_t length = strnlen(comp.bbEntityKey[i], kBtEntityKeyBytes);
        if (length == 0 || comp.bbEntityValue[i].IsNull()) {
            continue;
        }
        const std::string name(comp.bbEntityKey[i], length);
        const int key = inst.blackboardAsset->FindKey(name);
        if (key < 0 || inst.blackboardAsset->keys[static_cast<size_t>(key)].type != BbType::Entity
            || static_cast<size_t>(key) >= inst.blackboard.size()) {
            const uint64_t owner = (static_cast<uint64_t>(inst.entity.index) << 32) | inst.entity.generation;
            if (warnedEntityInit.insert({ owner, i }).second) {
                MYE_LOG_WARN("[behaviortree] '%s': initial Entity key '%s' is not an Entity key of the blackboard; ignored",
                             world.GetName(inst.entity), name.c_str());
            }
            continue;
        }
        BbValue value;
        value.isSet = 1;
        value.entity = comp.bbEntityValue[i];
        inst.blackboard[static_cast<size_t>(key)] = value;
    }
}

// inst の木に対して根から Abort する。Abort したノードがあれば true
bool AbortTree(World& world, uint64_t tick, std::vector<int32_t>* abortTrace, BtInstance& inst)
{
    if (!inst.tree || inst.tree->rootIndex < 0 || !AnyNodeActive(inst)) {
        return false;
    }
    RunCtx ctx{ world, inst, *inst.tree, tick, abortTrace, nullptr };
    AbortNode(ctx, inst.tree->rootIndex);
    return ctx.aborted;
}

// 配られたイベントのうち inst 宛て (自分 or 全体) を、eventName が一致するキーへ書く。配送順に書くので同じキーなら最後が勝つ。
// 型ごとの書き方は spec 4.1.6。送信元の無い (null) イベントの Entity キーは未設定にする
void ApplyEventsToBlackboard(BtInstance& inst, const std::vector<BtEvent>& delivered)
{
    if (delivered.empty() || !inst.blackboardAsset) {
        return;
    }
    const std::vector<BbKeyDef>& keys = inst.blackboardAsset->keys;
    for (const BtEvent& event : delivered) {
        if (!event.target.IsNull() && !(event.target == inst.entity)) {
            continue;
        }
        for (size_t k = 0; k < keys.size() && k < inst.blackboard.size(); ++k) {
            if (keys[k].eventName.empty() || BtEventNameHash(keys[k].eventName) != event.nameHash) {
                continue;
            }
            BbValue value;
            value.isSet = 1;
            switch (keys[k].type) {
            case BbType::Bool: value.i = 1; break;
            case BbType::Int: value.i = event.intValue; break;
            case BbType::Float: value.f = event.value; break;
            case BbType::Vector: std::memcpy(value.v, event.vec3, sizeof(value.v)); break;
            case BbType::Entity:
                if (event.sender.IsNull()) {
                    value.isSet = 0;
                } else {
                    value.entity = event.sender;
                }
                break;
            }
            inst.blackboard[k] = value;
        }
    }
}

void WriteEntity(ByteWriter& w, EntityID e)
{
    w.U32(e.index);
    w.U32(e.generation);
}

EntityID ReadEntity(ByteReader& r)
{
    EntityID e;
    e.index = r.U32();
    e.generation = r.U32();
    return e;
}

} // namespace

const BtInstance* BehaviorTreeSystem::FindInstance(EntityID entity) const
{
    const auto it = std::lower_bound(instances_.begin(), instances_.end(), entity,
                                     [](const BtInstance& inst, EntityID key) { return KeyLess(inst.entity, key); });
    return it != instances_.end() && it->entity == entity ? &*it : nullptr;
}

void BehaviorTreeSystem::Reset()
{
    instances_.clear();
    pending_.clear();
    delivered_.clear();
    eventOverflowWarned_ = false;
    warnedMissing_.clear();
    warnedEntityInit_.clear();
}

bool BehaviorTreeSystem::SendEvent(uint64_t tick, EntityID sender, EntityID target, uint64_t nameHash, const float (&vec3)[3],
                                   float value, int32_t intValue)
{
    if (pending_.size() >= static_cast<size_t>(kBtMaxEventsPerTick)) {
        if (!eventOverflowWarned_) {
            eventOverflowWarned_ = true;
            MYE_LOG_WARN("[behaviortree] event queue is full (%d per tick); the rest are dropped", kBtMaxEventsPerTick);
        }
        return false;
    }
    BtEvent event;
    event.nameHash = nameHash;
    event.sender = sender;
    event.target = target;
    std::memcpy(event.vec3, vec3, sizeof(event.vec3));
    event.value = value;
    event.intValue = intValue;
    event.seq = static_cast<uint32_t>(pending_.size());
    event.sentTick = tick;
    pending_.push_back(event);
    return true;
}

int BehaviorTreeSystem::EventCount(EntityID self) const
{
    int count = 0;
    for (const BtEvent& event : delivered_) {
        if (event.target.IsNull() || event.target == self) {
            ++count;
        }
    }
    return count;
}

bool BehaviorTreeSystem::GetEvent(EntityID self, int index, BtEvent& out) const
{
    int seen = 0;
    for (const BtEvent& event : delivered_) {
        if (event.target.IsNull() || event.target == self) {
            if (seen++ == index) {
                out = event;
                return true;
            }
        }
    }
    return false;
}

// tick より前に積まれた分を配達済みへ移す (前の配達済みはここで捨てる)。配送順 = 送信元キー → seq。
// 同じ tick に積んだ分は残して次の tick に回し、seq は 0 から振り直す
void BehaviorTreeSystem::DeliverPending(uint64_t tick)
{
    delivered_.clear();
    if (pending_.empty()) {
        return;
    }
    const auto boundary = std::stable_partition(pending_.begin(), pending_.end(),
                                                [tick](const BtEvent& e) { return e.sentTick < tick; });
    delivered_.assign(pending_.begin(), boundary);
    pending_.erase(pending_.begin(), boundary);
    for (size_t i = 0; i < pending_.size(); ++i) {
        pending_[i].seq = static_cast<uint32_t>(i);
    }
    std::sort(delivered_.begin(), delivered_.end(), [](const BtEvent& a, const BtEvent& b) {
        return a.sender == b.sender ? a.seq < b.seq : KeyLess(a.sender, b.sender);
    });
}

void BehaviorTreeSystem::Update(World& world, uint64_t tick, const NavSystem* nav, const ControllerLibrary* controllers,
                                const AnimationLibrary* clips)
{
    std::vector<EntityID> owners;
    {
        const ComponentTypeId req[] = { BehaviorTreeComponent::sTypeId };
        world.ForEachArchetype(req, [&](Archetype& arch) {
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                owners.push_back(arch.EntityAt(row));
            }
        });
    }
    if (owners.empty() && instances_.empty()) {
        return;
    }
    std::sort(owners.begin(), owners.end(), KeyLess);

    // 表もキー昇順なので、owners と並べて走査する。コンポーネントが外れた / エンティティが消えたものは表から落ちる
    std::vector<BtInstance> next;
    next.reserve(owners.size());
    size_t oldAt = 0;
    // コンポーネントが外れたエンティティの表を落とす。エンティティが生きていれば、木が Agent へ書いたもの
    // (目的地・navFilter・updateRotation) を戻すために先に Abort する (消えたエンティティには戻す先が無い)
    const auto dropInstance = [&](BtInstance& dropped) {
        if (world.IsAlive(dropped.entity)) {
            AbortTree(world, tick, abortTrace_, dropped);
        }
    };
    for (const EntityID owner : owners) {
        while (oldAt < instances_.size() && KeyLess(instances_[oldAt].entity, owner)) {
            dropInstance(instances_[oldAt]);
            ++oldAt;
        }
        BtInstance inst;
        if (oldAt < instances_.size() && instances_[oldAt].entity == owner) {
            inst = std::move(instances_[oldAt]);
            ++oldAt;
        }
        if (StepOwner(world, tick, nav, controllers, clips, owner, inst)) {
            next.push_back(std::move(inst));
        }
    }
    for (; oldAt < instances_.size(); ++oldAt) {
        dropInstance(instances_[oldAt]);
    }
    instances_ = std::move(next);
}

std::shared_ptr<const BehaviorTreeAsset> BehaviorTreeSystem::ResolveTree(uint64_t guid)
{
    const BehaviorTreeLibrary* library = behaviortree::Library();
    std::shared_ptr<const BehaviorTreeAsset> registered = (library != nullptr && guid != 0) ? library->GetShared(guid) : nullptr;
    if (!registered) {
        expansions_.erase(guid);
        return nullptr;
    }
    std::shared_ptr<BtExpansion>& entry = expansions_[guid];
    if (!entry || !entry->IsCurrent(*library, registered)) {
        entry = std::make_shared<BtExpansion>(BtExpandSubTrees(*library, std::move(registered)));
    }
    return entry->tree;
}

bool BehaviorTreeSystem::StepOwner(World& world, uint64_t tick, const NavSystem* nav, const ControllerLibrary* controllers,
                                   const AnimationLibrary* clips, EntityID owner, BtInstance& inst)
{
    BehaviorTreeComponent* comp = world.GetComponent<BehaviorTreeComponent>(owner);
    bool hasInstance = !inst.entity.IsNull();

    // 木と BB を引く (置き換えられたものは shared_ptr の同一性で分かる)
    const uint64_t guid = comp->tree.value;
    BlackboardLibrary* bbLibrary = blackboard::Library();
    std::shared_ptr<const BehaviorTreeAsset> tree = ResolveTree(guid); // SubTree を取り込み済みの木
    std::shared_ptr<const BlackboardAsset> bbAsset;
    bool bbMissing = false;
    if (tree && tree->blackboard != 0) {
        bbAsset = bbLibrary != nullptr ? bbLibrary->GetShared(tree->blackboard) : nullptr;
        bbMissing = !bbAsset;
    }

    // 木が替わった (別の木に変えた・読み直された)・BB が読み直された: 古い木の形のまま Abort してから作り直す。
    // 同じ GUID の木の読み直しで BB が同じなら BB の値は保つ
    if (hasInstance) {
        const bool treeChanged = inst.treeGuid != guid || inst.tree != tree;
        const bool bbChanged = inst.treeGuid != guid || inst.blackboardAsset != bbAsset;
        if (treeChanged || bbChanged) {
            if (AbortTree(world, tick, abortTrace_, inst)) {
                comp->lastAbortTick = TickToField(tick);
            }
            if (tree && !bbMissing) {
                inst.treeGuid = guid;
                inst.tree = tree;
                inst.blackboardAsset = bbAsset;
                ResetNodes(inst);
                inst.rootStatus = btroot::kRunning;
                if (bbChanged) {
                    InitBlackboard(inst);
                }
                ApplyEntityInitials(world, *comp, inst, warnedEntityInit_); // 木のやり直しでも初期値を書き直す
            } else {
                hasInstance = false;
            }
        }
    }

    if (guid == 0 || !tree || bbMissing) {
        if (guid != 0 && warnedMissing_.insert(guid).second) {
            MYE_LOG_WARN("[behaviortree] '%s': tree (or its blackboard) is not registered", world.GetName(owner));
        }
        comp->status = guid != 0 ? btstatus::kAssetMissing : btstatus::kIdle;
        comp->activeNodeId = -1;
        return false;
    }

    // 無効: 実行中なら Abort して止める。ブラックボードは保つ (再び有効になったら根から)
    if (!comp->enabled || !IsEntityActive(world, owner)) {
        if (!hasInstance) {
            comp->status = btstatus::kIdle;
            comp->activeNodeId = -1;
            return false;
        }
        if (AbortTree(world, tick, abortTrace_, inst)) {
            comp->lastAbortTick = TickToField(tick);
        }
        inst.rootStatus = btroot::kRunning;
        comp->status = btstatus::kIdle;
        comp->activeNodeId = -1;
        return true;
    }

    if (!hasInstance) {
        inst = BtInstance{};
        inst.entity = owner;
        inst.treeGuid = guid;
        inst.tree = tree;
        inst.blackboardAsset = bbAsset;
        ResetNodes(inst);
        InitBlackboard(inst);
        ApplyEntityInitials(world, *comp, inst, warnedEntityInit_);
    }
    inst.entity = owner;

    if (tree->rootIndex < 0) {
        comp->status = btstatus::kIdle;
        comp->activeNodeId = -1;
        return true;
    }

    // 根が終わった次の tick: 根からやり直す
    // Decorator の欄 (Cooldown の計時・BlackboardCondition の最後の結果) は残す
    if (inst.rootStatus != btroot::kRunning) {
        std::fill_n(inst.nodes.begin(), tree->nodes.size(), BtNodeState{});
        std::fill(inst.extra.begin(), inst.extra.end(), static_cast<uint8_t>(0));
        inst.rootStatus = btroot::kRunning;
    }

    // 配られたイベントの BB 反映は Abort の監視より前 (spec 4.1.1 の (1))
    ApplyEventsToBlackboard(inst, delivered_);
    RunCtx ctx{ world, inst, *tree, tick, abortTrace_, nav, this, controllers, clips };
    MonitorNode(ctx, tree->rootIndex);
    const BtResult result = Visit(ctx, tree->rootIndex);
    if (result != BtResult::Running) {
        inst.rootStatus = result == BtResult::Success ? btroot::kSucceeded : btroot::kFailed;
    }
    if (ctx.stepLimitHit && !inst.stepLimitWarned) {
        inst.stepLimitWarned = true;
        MYE_LOG_WARN("[behaviortree] '%s': hit the per-tick step limit (%d); the rest continues next tick",
                     world.GetName(owner), kBtMaxStepsPerTick);
    }
    comp->status = inst.rootStatus == btroot::kRunning ? btstatus::kRunning
                   : inst.rootStatus == btroot::kSucceeded ? btstatus::kSucceeded
                                                           : btstatus::kFailed;
    comp->activeNodeId = ActiveLeafId(inst, *tree);
    if (ctx.aborted) {
        comp->lastAbortTick = TickToField(tick);
    }
    return true;
}

void BehaviorTreeSystem::ActiveNodeIndices(const BtInstance& inst, std::vector<int32_t>& out)
{
    if (!inst.tree || inst.nodes.size() < inst.tree->nodes.size()) {
        return;
    }
    for (size_t i = 0; i < inst.tree->nodes.size(); ++i) {
        if (NodeActive(inst, *inst.tree, static_cast<int32_t>(i))) {
            out.push_back(static_cast<int32_t>(i));
        }
    }
}

namespace {

// 追加状態を読む (領域が足りなければ false。復元直後の不整合な表で範囲外を読まない)
template <typename T>
bool ReadExtra(const BtInstance& inst, const BtNodeDef& node, T& out)
{
    if (node.extraOffset < 0 || static_cast<size_t>(node.extraOffset) + sizeof(T) > inst.extra.size()) {
        return false;
    }
    std::memcpy(&out, inst.extra.data() + node.extraOffset, sizeof(T));
    return true;
}

// Patrol ノードの route キーが指す、生きているエンティティ。読めなければ kNullEntity
EntityID PatrolRouteEntityOf(World& world, const BtInstance& inst, const BtNodeDef& node)
{
    if (!inst.blackboardAsset || node.keys.size() <= static_cast<size_t>(btpatrolkey::kRoute)) {
        return kNullEntity;
    }
    const int key = inst.blackboardAsset->FindKey(node.keys[btpatrolkey::kRoute]);
    if (key < 0 || static_cast<size_t>(key) >= inst.blackboard.size()
        || inst.blackboardAsset->keys[static_cast<size_t>(key)].type != BbType::Entity) {
        return kNullEntity;
    }
    const BbValue& value = inst.blackboard[static_cast<size_t>(key)];
    return value.isSet != 0 && world.IsAlive(value.entity) ? value.entity : kNullEntity;
}

} // namespace

void BehaviorTreeSystem::AppendDebugLines(World& world, std::vector<DebugLineCmd>& out) const
{
    // ナビメッシュの塗り (水色) と、選択中の Agent の経路線 (淡い黄緑) に埋もれない色
    constexpr uint32_t kMoveColor = 0xFF58F0FFu;   // MoveTo の目的地への線 (桃)
    constexpr uint32_t kSearchColor = 0xFF9A20FFu; // SearchArea の起点・円・今の点 (橙)
    constexpr uint32_t kPatrolColor = 0xFFF050FFu; // Patrol の向かっている点 (黄)
    constexpr float kMark = 0.2f;
    constexpr float kPatrolMark = 0.45f;
    constexpr float kPatrolStake = 1.2f;
    constexpr int kRingSegments = 24;
    const auto cross = [&out](const float* c, float size, uint32_t rgba) {
        out.push_back({ c[0] - size, c[1], c[2], c[0] + size, c[1], c[2], rgba });
        out.push_back({ c[0], c[1] - size, c[2], c[0], c[1] + size, c[2], rgba });
        out.push_back({ c[0], c[1], c[2] - size, c[0], c[1], c[2] + size, rgba });
    };
    const auto line = [&out](const float* a, const float* b, uint32_t rgba) {
        out.push_back({ a[0], a[1], a[2], b[0], b[1], b[2], rgba });
    };
    for (const BtInstance& inst : instances_) {
        if (!inst.tree || !world.IsAlive(inst.entity) || inst.nodes.size() < inst.tree->nodes.size()) {
            continue;
        }
        const BehaviorTreeComponent* comp = world.GetComponent<BehaviorTreeComponent>(inst.entity);
        const NavMeshAgentComponent* agent = world.GetComponent<NavMeshAgentComponent>(inst.entity);
        float self[3] = {};
        if (comp == nullptr || !comp->drawDebug || !WorldPositionOf(world, inst.entity, self)) {
            continue;
        }
        const float destination[3] = { agent != nullptr ? agent->destination.x : 0.0f, agent != nullptr ? agent->destination.y : 0.0f,
                                       agent != nullptr ? agent->destination.z : 0.0f };
        const bool hasDestination = agent != nullptr && agent->hasDestination;
        for (size_t i = 0; i < inst.tree->nodes.size(); ++i) {
            if (inst.nodes[i].active == 0) {
                continue;
            }
            const BtNodeDef& node = inst.tree->nodes[i];
            switch (node.kind) {
            case BtNodeKind::MoveTo:
                if (hasDestination) {
                    line(self, destination, kMoveColor);
                    cross(destination, kMark, kMoveColor);
                }
                break;
            case BtNodeKind::SearchArea: {
                BtSearchAreaState search;
                if (!ReadExtra(inst, node, search)) {
                    break;
                }
                cross(search.origin, kMark, kSearchColor);
                const float radius = node.params[btsearchparam::kRadius].f;
                for (int k = 0; k < kRingSegments; ++k) {
                    const float a0 = 2.0f * kPi * static_cast<float>(k) / kRingSegments;
                    const float a1 = 2.0f * kPi * static_cast<float>(k + 1) / kRingSegments;
                    const float from[3] = { search.origin[0] + std::cos(a0) * radius, search.origin[1], search.origin[2] + std::sin(a0) * radius };
                    const float to[3] = { search.origin[0] + std::cos(a1) * radius, search.origin[1], search.origin[2] + std::sin(a1) * radius };
                    line(from, to, kSearchColor);
                }
                cross(search.current, kMark * 1.5f, kSearchColor);
                if (hasDestination) {
                    line(self, destination, kSearchColor);
                }
                break;
            }
            case BtNodeKind::Patrol: {
                BtPatrolState patrol;
                const EntityID routeEntity = PatrolRouteEntityOf(world, inst, node);
                const PatrolRouteComponent* route = routeEntity.IsNull() ? nullptr : world.GetComponent<PatrolRouteComponent>(routeEntity);
                float point[3] = {};
                if (route == nullptr || !ReadExtra(inst, node, patrol) || patrol.nextIndex < 0
                    || patrol.nextIndex >= (std::min)(route->pointCount, kMaxPatrolPoints)
                    || !PatrolPointWorld(world, routeEntity, *route, patrol.nextIndex, point)) {
                    break;
                }
                line(self, point, kPatrolColor);
                cross(point, kPatrolMark, kPatrolColor);
                const float top[3] = { point[0], point[1] + kPatrolStake, point[2] };
                line(point, top, kPatrolColor);
                break;
            }
            default: break;
            }
        }
    }
}

void BehaviorTreeSystem::SaveSnapshot(ByteWriter& w) const
{
    SaveInstances(w);
    SavePending(w);
}

void BehaviorTreeSystem::SavePending(ByteWriter& w) const
{
    w.Count(pending_.size());
    for (const BtEvent& event : pending_) {
        w.U64(event.nameHash);
        WriteEntity(w, event.sender);
        WriteEntity(w, event.target);
        w.F32(event.vec3[0]);
        w.F32(event.vec3[1]);
        w.F32(event.vec3[2]);
        w.F32(event.value);
        w.I32(event.intValue);
        w.U32(event.seq);
        w.U64(event.sentTick);
    }
}

void BehaviorTreeSystem::SaveInstances(ByteWriter& w) const
{
    w.Count(instances_.size());
    for (const BtInstance& inst : instances_) {
        WriteEntity(w, inst.entity);
        w.U64(inst.treeGuid);
        w.U8(inst.rootStatus);
        w.Count(inst.blackboard.size());
        for (const BbValue& value : inst.blackboard) {
            w.U8(value.isSet);
            w.I32(value.i);
            w.F32(value.f);
            w.F32(value.v[0]);
            w.F32(value.v[1]);
            w.F32(value.v[2]);
            WriteEntity(w, value.entity);
        }
        w.Count(inst.nodes.size());
        for (const BtNodeState& state : inst.nodes) {
            w.U8(state.active);
            w.U8(state.phase);
            w.I32(state.child);
            w.I32(state.counter);
        }
        w.Blob(inst.extra.data(), inst.extra.size()); // 種類別の追加状態 (生バイト)
    }
}

bool BehaviorTreeSystem::ReadSnapshot(ByteReader& r, BtSnapshot& out)
{
    out.instances.clear();
    const size_t count = r.Count(kInstanceMinBytes);
    out.instances.reserve(count);
    for (size_t i = 0; i < count && r.Ok(); ++i) {
        BtInstance inst;
        inst.entity = ReadEntity(r);
        inst.treeGuid = r.U64();
        inst.rootStatus = r.U8();
        const size_t bbCount = r.Count(kBbValueBytes);
        if (bbCount > static_cast<size_t>(kBbMaxKeys)) {
            r.Fail();
            break;
        }
        inst.blackboard.resize(bbCount);
        for (BbValue& value : inst.blackboard) {
            value.isSet = r.U8();
            value.i = r.I32();
            value.f = r.F32();
            value.v[0] = r.F32();
            value.v[1] = r.F32();
            value.v[2] = r.F32();
            value.entity = ReadEntity(r);
            if (value.isSet > 1) {
                r.Fail();
            }
        }
        const size_t nodeCount = r.Count(kNodeStateBytes);
        if (nodeCount > static_cast<size_t>(kBtMaxStateSlots)) {
            r.Fail();
            break;
        }
        inst.nodes.resize(nodeCount);
        for (BtNodeState& state : inst.nodes) {
            state.active = r.U8();
            state.phase = r.U8();
            state.child = r.I32();
            state.counter = r.I32();
            if (state.active > 1 || state.child < 0) {
                r.Fail();
            }
        }
        const size_t extraCount = r.Count(sizeof(uint8_t));
        if (extraCount > static_cast<size_t>(kBtMaxExtraBytes)) {
            r.Fail();
            break;
        }
        inst.extra.resize(extraCount);
        r.Raw(inst.extra.data(), extraCount);
        // 表はエンティティキー昇順で、同じキーは 2 つ無い (実行器の merge 走査の前提)
        const bool ordered = out.instances.empty() || KeyLess(out.instances.back().entity, inst.entity);
        if (inst.entity.IsNull() || inst.rootStatus > btroot::kFailed || !ordered) {
            r.Fail();
        }
        out.instances.push_back(std::move(inst));
    }
    if (!r.Ok()) {
        return false;
    }

    // 配送待ち。seq は 0 から連番 (積むときも配るときもそう振る)
    out.pending.clear();
    const size_t eventCount = r.Count(kEventBytes);
    if (eventCount > static_cast<size_t>(kBtMaxEventsPerTick)) {
        r.Fail();
        return false;
    }
    out.pending.reserve(eventCount);
    for (size_t i = 0; i < eventCount && r.Ok(); ++i) {
        BtEvent event;
        event.nameHash = r.U64();
        event.sender = ReadEntity(r);
        event.target = ReadEntity(r);
        event.vec3[0] = r.F32();
        event.vec3[1] = r.F32();
        event.vec3[2] = r.F32();
        event.value = r.F32();
        event.intValue = r.I32();
        event.seq = r.U32();
        event.sentTick = r.U64();
        if (event.seq != static_cast<uint32_t>(i)) {
            r.Fail();
        }
        out.pending.push_back(event);
    }
    return r.Ok();
}

void BehaviorTreeSystem::ApplySnapshot(World& world, BtSnapshot&& snapshot)
{
    BlackboardLibrary* bbLibrary = blackboard::Library();
    std::vector<BtInstance> restored;
    restored.reserve(snapshot.instances.size());
    for (BtInstance& inst : snapshot.instances) {
        if (!world.IsAlive(inst.entity) || !world.HasComponent(inst.entity, BehaviorTreeComponent::sTypeId)) {
            continue;
        }
        std::shared_ptr<const BehaviorTreeAsset> tree = ResolveTree(inst.treeGuid);
        if (!tree) {
            continue;
        }
        std::shared_ptr<const BlackboardAsset> bbAsset;
        if (tree->blackboard != 0) {
            bbAsset = bbLibrary != nullptr ? bbLibrary->GetShared(tree->blackboard) : nullptr;
            if (!bbAsset) {
                continue;
            }
        }
        inst.tree = std::move(tree);
        inst.blackboardAsset = std::move(bbAsset);
        const size_t bbKeys = inst.blackboardAsset ? inst.blackboardAsset->keys.size() : 0;
        if (inst.nodes.size() != static_cast<size_t>(inst.tree->stateSlotCount) || inst.blackboard.size() != bbKeys
            || inst.extra.size() != static_cast<size_t>(inst.tree->extraStateBytes)) {
            MYE_LOG_WARN("[behaviortree] '%s': the saved state does not fit the registered tree; restarting it",
                         world.GetName(inst.entity));
            ResetNodes(inst);
            InitBlackboard(inst);
            if (const auto* comp = world.GetComponent<BehaviorTreeComponent>(inst.entity)) {
                ApplyEntityInitials(world, *comp, inst, warnedEntityInit_);
            }
            inst.rootStatus = btroot::kRunning;
        }
        restored.push_back(std::move(inst));
    }
    instances_ = std::move(restored);
    pending_ = std::move(snapshot.pending);
    delivered_.clear(); // 配った分はスナップショットに入らない (撮るのは tick 末)
}

uint64_t BehaviorTreeSystem::StateHash() const
{
    std::vector<std::byte> bytes;
    ByteWriter w(bytes);
    SaveInstances(w);
    if (!pending_.empty()) {
        SavePending(w); // 配送待ちが空なら何も足さない (イベントを使わないシーンのハッシュは変わらない)
    }
    return HashBytes(bytes.data(), bytes.size());
}

} // namespace mye
