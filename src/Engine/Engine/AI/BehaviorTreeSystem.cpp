//====================================================================================
//                          BehaviorTreeSystem.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの実行器の実装
//====================================================================================
#include "Engine/Engine/AI/BehaviorTreeSystem.h"

#include <algorithm>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"

namespace mye {

namespace {

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

// BT 節の 1 要素あたりの最小バイト数 (ByteReader::Count が残りバイトで件数を検証するのに使う)
constexpr size_t kBbValueBytes = sizeof(uint8_t) + sizeof(int32_t) + sizeof(float) + 3 * sizeof(float) + 2 * sizeof(uint32_t);
constexpr size_t kNodeStateBytes = 2 * sizeof(uint8_t) + 2 * sizeof(int32_t);
constexpr size_t kInstanceMinBytes = 2 * sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint8_t) + 2 * sizeof(uint64_t);

bool KeyLess(EntityID a, EntityID b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// 1 体の 1 tick の作業記録
struct RunCtx {
    World& world;
    BtInstance& inst;
    const BehaviorTreeAsset& tree;
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

// 実行中のノードとその子孫を Abort する。子孫が先 (深い方から)。ノード固有の後始末は種類ごとにここへ足す
void AbortNode(RunCtx& c, int32_t index)
{
    BtNodeState& state = c.inst.nodes[static_cast<size_t>(index)];
    if (state.active == 0) {
        return;
    }
    for (const int32_t child : c.tree.nodes[static_cast<size_t>(index)].children) {
        AbortNode(c, child);
    }
    Finish(state);
    c.aborted = true;
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

BtResult Visit(RunCtx& c, int32_t index)
{
    if (c.steps >= kBtMaxStepsPerTick) {
        c.stepLimitHit = true; // 手を付けずに Running で返す。入っていないノードは次の tick に親が入り直す
        return BtResult::Running;
    }
    ++c.steps;
    switch (c.tree.nodes[static_cast<size_t>(index)].kind) {
    case BtNodeKind::Selector: return VisitSequenceOrSelector(c, index, true);
    case BtNodeKind::Sequence: return VisitSequenceOrSelector(c, index, false);
    case BtNodeKind::SimpleParallel: return VisitSimpleParallel(c, index);
    case BtNodeKind::Wait: return VisitWait(c, index);
    case BtNodeKind::Count: break;
    }
    return BtResult::Failure;
}

// 実行中の一番深いノードの id。何も実行していなければ -1
int32_t ActiveLeafId(const BtInstance& inst, const BehaviorTreeAsset& tree)
{
    int32_t at = tree.rootIndex;
    if (at < 0 || inst.nodes[static_cast<size_t>(at)].active == 0) {
        return -1;
    }
    for (;;) {
        int32_t next = -1;
        for (const int32_t child : tree.nodes[static_cast<size_t>(at)].children) {
            if (inst.nodes[static_cast<size_t>(child)].active != 0) {
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

bool AnyNodeActive(const BtInstance& inst)
{
    return std::any_of(inst.nodes.begin(), inst.nodes.end(), [](const BtNodeState& s) { return s.active != 0; });
}

// ノードの状態を全部初期値にする (木の形に合わせて長さも直す)
void ResetNodes(BtInstance& inst)
{
    inst.nodes.assign(static_cast<size_t>(inst.tree->stateSlotCount), BtNodeState{});
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

// inst の木に対して根から Abort する。Abort したノードがあれば true
bool AbortTree(World& world, BtInstance& inst)
{
    if (!inst.tree || inst.tree->rootIndex < 0 || !AnyNodeActive(inst)) {
        return false;
    }
    RunCtx ctx{ world, inst, *inst.tree };
    AbortNode(ctx, inst.tree->rootIndex);
    return ctx.aborted;
}

int32_t TickToField(uint64_t tick)
{
    return static_cast<int32_t>((std::min)(tick, static_cast<uint64_t>(INT32_MAX)));
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
    warnedMissing_.clear();
}

void BehaviorTreeSystem::Update(World& world, uint64_t tick)
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
    for (const EntityID owner : owners) {
        while (oldAt < instances_.size() && KeyLess(instances_[oldAt].entity, owner)) {
            ++oldAt;
        }
        BtInstance inst;
        if (oldAt < instances_.size() && instances_[oldAt].entity == owner) {
            inst = std::move(instances_[oldAt]);
            ++oldAt;
        }
        if (StepOwner(world, tick, owner, inst)) {
            next.push_back(std::move(inst));
        }
    }
    instances_ = std::move(next);
}

bool BehaviorTreeSystem::StepOwner(World& world, uint64_t tick, EntityID owner, BtInstance& inst)
{
    BehaviorTreeComponent* comp = world.GetComponent<BehaviorTreeComponent>(owner);
    bool hasInstance = !inst.entity.IsNull();

    // 木と BB を引く (置き換えられたものは shared_ptr の同一性で分かる)
    const uint64_t guid = comp->tree.value;
    BehaviorTreeLibrary* library = behaviortree::Library();
    BlackboardLibrary* bbLibrary = blackboard::Library();
    std::shared_ptr<const BehaviorTreeAsset> tree =
        (library != nullptr && guid != 0) ? library->GetShared(guid) : nullptr;
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
            if (AbortTree(world, inst)) {
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
        if (AbortTree(world, inst)) {
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
    }
    inst.entity = owner;

    if (tree->rootIndex < 0) {
        comp->status = btstatus::kIdle;
        comp->activeNodeId = -1;
        return true;
    }

    // 根が終わった次の tick: 根からやり直す
    if (inst.rootStatus != btroot::kRunning) {
        ResetNodes(inst);
        inst.rootStatus = btroot::kRunning;
    }

    RunCtx ctx{ world, inst, *tree };
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

void BehaviorTreeSystem::SaveSnapshot(ByteWriter& w) const
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
        if (nodeCount > static_cast<size_t>(kBtMaxNodes)) {
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
        // 表はエンティティキー昇順で、同じキーは 2 つ無い (実行器の merge 走査の前提)
        const bool ordered = out.instances.empty() || KeyLess(out.instances.back().entity, inst.entity);
        if (inst.entity.IsNull() || inst.rootStatus > btroot::kFailed || !ordered) {
            r.Fail();
        }
        out.instances.push_back(std::move(inst));
    }
    return r.Ok();
}

void BehaviorTreeSystem::ApplySnapshot(World& world, BtSnapshot&& snapshot)
{
    BehaviorTreeLibrary* library = behaviortree::Library();
    BlackboardLibrary* bbLibrary = blackboard::Library();
    std::vector<BtInstance> restored;
    restored.reserve(snapshot.instances.size());
    for (BtInstance& inst : snapshot.instances) {
        if (!world.IsAlive(inst.entity) || !world.HasComponent(inst.entity, BehaviorTreeComponent::sTypeId)) {
            continue;
        }
        std::shared_ptr<const BehaviorTreeAsset> tree = library != nullptr ? library->GetShared(inst.treeGuid) : nullptr;
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
        if (inst.nodes.size() != static_cast<size_t>(inst.tree->stateSlotCount) || inst.blackboard.size() != bbKeys) {
            MYE_LOG_WARN("[behaviortree] '%s': the saved state does not fit the registered tree; restarting it",
                         world.GetName(inst.entity));
            ResetNodes(inst);
            InitBlackboard(inst);
            inst.rootStatus = btroot::kRunning;
        }
        restored.push_back(std::move(inst));
    }
    instances_ = std::move(restored);
}

uint64_t BehaviorTreeSystem::StateHash() const
{
    std::vector<std::byte> bytes;
    ByteWriter w(bytes);
    SaveSnapshot(w);
    return HashBytes(bytes.data(), bytes.size());
}

} // namespace mye
