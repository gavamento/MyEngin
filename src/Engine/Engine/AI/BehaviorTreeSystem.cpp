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
    uint64_t tick;
    std::vector<int32_t>* abortTrace;
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

BtResult VisitBody(RunCtx& c, int32_t index)
{
    switch (c.tree.nodes[static_cast<size_t>(index)].kind) {
    case BtNodeKind::Selector: return VisitSequenceOrSelector(c, index, true);
    case BtNodeKind::Sequence: return VisitSequenceOrSelector(c, index, false);
    case BtNodeKind::SimpleParallel: return VisitSimpleParallel(c, index);
    case BtNodeKind::Wait: return VisitWait(c, index);
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
    int32_t at = tree.rootIndex;
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
bool AbortTree(World& world, uint64_t tick, std::vector<int32_t>* abortTrace, BtInstance& inst)
{
    if (!inst.tree || inst.tree->rootIndex < 0 || !AnyNodeActive(inst)) {
        return false;
    }
    RunCtx ctx{ world, inst, *inst.tree, tick, abortTrace };
    AbortNode(ctx, inst.tree->rootIndex);
    return ctx.aborted;
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
        inst.rootStatus = btroot::kRunning;
    }

    RunCtx ctx{ world, inst, *tree, tick, abortTrace_ };
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
