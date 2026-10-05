//====================================================================================
//                          BehaviorTreeSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの核の回帰テスト実装
//====================================================================================
#include "Engine/Engine/AI/BehaviorTreeSelfTest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Core/Util/ByteIo.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BehaviorTreeSystem.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Engine/Animation/Animation.h"
#include "Engine/Engine/Animation/AnimatorController.h"
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Navigation/NavBake.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Navigation/NavSystem.h"
#include "Engine/Engine/Perception/PerceptionSystem.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Rendering/DebugDraw.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/Tags.h"
#include "Engine/Engine/Scene/TransformSystem.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

using nlohmann::json;

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

// 試験用のライブラリを差し込み、終わったら元へ戻す (EngineLoop が入れたものを壊さない)
struct LibraryScope {
    BehaviorTreeLibrary trees;
    BlackboardLibrary boards;
    BehaviorTreeLibrary* prevTrees = behaviortree::Library();
    BlackboardLibrary* prevBoards = blackboard::Library();
    LibraryScope()
    {
        behaviortree::Install(&trees);
        blackboard::Install(&boards);
    }
    ~LibraryScope()
    {
        behaviortree::Install(prevTrees);
        blackboard::Install(prevBoards);
    }
    LibraryScope(const LibraryScope&) = delete;
    LibraryScope& operator=(const LibraryScope&) = delete;
};

// ---- JSON の組み立て ----

json Node(int id, const char* type, std::vector<int> children = {}, json params = json::object())
{
    json n;
    n["id"] = id;
    n["type"] = type;
    n["params"] = std::move(params);
    n["decorators"] = json::array();
    json kids = json::array();
    for (const int child : children) {
        kids.push_back(child);
    }
    n["children"] = std::move(kids);
    n["pos"] = json::array({ static_cast<float>(id) * 10.0f, static_cast<float>(id) * -5.0f });
    return n;
}

json Wait(int id, int ticks, int deviation = 0)
{
    return Node(id, "Wait", {}, json{ { "ticks", ticks }, { "randomDeviation", deviation } });
}

json Parallel(int id, int mainChild, int backgroundChild, const char* finishMode)
{
    return Node(id, "SimpleParallel", { mainChild, backgroundChild }, json{ { "finishMode", finishMode } });
}

json Deco(const char* type, json params = json::object(), const char* key = nullptr)
{
    json d;
    d["type"] = type;
    if (key != nullptr) {
        d["key"] = key;
    }
    d["params"] = std::move(params);
    return d;
}

json BbCond(const char* key, const char* query, const char* abort = "None", int intValue = 0, double floatValue = 0.0)
{
    return Deco("BlackboardCondition",
                json{ { "query", query }, { "abort", abort }, { "intValue", intValue }, { "floatValue", floatValue } }, key);
}

json Cooldown(int ticks)
{
    return Deco("Cooldown", json{ { "ticks", ticks } });
}

json Repeat(int count)
{
    return Deco("Repeat", json{ { "count", count } });
}

json Timeout(int ticks)
{
    return Deco("Timeout", json{ { "ticks", ticks } });
}

// ノードに Decorator を付ける (上から順 = 外側から順)
json Decorated(json node, const std::vector<json>& decorators)
{
    node["decorators"] = json::array();
    for (const json& d : decorators) {
        node["decorators"].push_back(d);
    }
    return node;
}

std::string GuidHex(uint64_t guid)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(guid));
    return std::string(buf);
}

json Tree(int root, const std::vector<json>& nodes, uint64_t blackboard = 0)
{
    json j;
    j["engine"] = "MyEngine";
    j["behaviortree"] = 1;
    j["blackboard"] = blackboard != 0 ? GuidHex(blackboard) : std::string();
    j["root"] = root;
    j["nodes"] = json::array();
    for (const json& n : nodes) {
        j["nodes"].push_back(n);
    }
    return j;
}

json BbKey(const char* name, const char* type, json initial = nullptr)
{
    json k;
    k["name"] = name;
    k["type"] = type;
    if (!initial.is_null()) {
        k["initial"] = std::move(initial);
    }
    return k;
}

json Board(const std::vector<json>& keys)
{
    json j;
    j["engine"] = "MyEngine";
    j["blackboard"] = 1;
    j["keys"] = json::array();
    for (const json& k : keys) {
        j["keys"].push_back(k);
    }
    return j;
}

std::wstring TreePath(const wchar_t* name)
{
    return std::wstring(L"selftest\\ai\\") + name + L".bt.json";
}

std::wstring BoardPath(const wchar_t* name)
{
    return std::wstring(L"selftest\\ai\\") + name + L".bb.json";
}

// 検査を通った木だけ登録する。登録できなければ 0
uint64_t RegisterTree(LibraryScope& lib, const wchar_t* name, const json& j)
{
    BehaviorTreeAsset asset;
    if (!BehaviorTreeLibrary::FromJson(j, asset)) {
        return 0;
    }
    return lib.trees.Register(TreePath(name), std::move(asset));
}

uint64_t RegisterBoard(LibraryScope& lib, const wchar_t* name, const json& j)
{
    BlackboardAsset asset;
    if (!BlackboardLibrary::FromJson(j, asset)) {
        return 0;
    }
    return lib.boards.Register(BoardPath(name), std::move(asset));
}

bool Loads(const json& j)
{
    BehaviorTreeAsset asset;
    return BehaviorTreeLibrary::FromJson(j, asset);
}

// ---- 最小の tick ----

constexpr int32_t kRunning = btstatus::kRunning;
constexpr int32_t kSucceeded = btstatus::kSucceeded;
constexpr int32_t kFailed = btstatus::kFailed;

struct Sim {
    Scene scene;
    BehaviorTreeSystem bt;
    uint64_t tick = 1;
    bool paused = false;                  // true: stepSim が偽の tick (配達も Update もしない)
    std::function<void()> beforeBt;       // 配達の後・BT の Update の前に呼ぶ (スクリプト層の代わり)
    const ControllerLibrary* controllers = nullptr; // PlayAnimation の引き先 (null = 渡さない)
    const AnimationLibrary* clips = nullptr;
    AnimatorControllerSystem* animator = nullptr;   // 非 null なら BT の後に Animator を進める (TickRunner のフェーズ順)

    World& GetWorld() { return scene.GetWorld(); }

    EntityID AddTreeEntity(uint64_t tree, const char* name = "Agent")
    {
        GameObject go = scene.CreateGameObjectTracked(name);
        go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ tree };
        return go.Id();
    }

    void Step()
    {
        if (!paused) {
            bt.DeliverPending(tick); // TickRunner と同じ: tick の頭 (スクリプト層より前)
            if (beforeBt) {
                beforeBt();
            }
            bt.Update(GetWorld(), tick, nullptr, controllers, clips);
            if (animator != nullptr && controllers != nullptr && clips != nullptr) {
                animator->Update(GetWorld(), *controllers, *clips);
            }
        }
        GetWorld().ApplyStructuralChanges();
        ++tick;
    }

    BehaviorTreeComponent* Comp(EntityID e) { return GetWorld().GetComponent<BehaviorTreeComponent>(e); }
    int32_t Status(EntityID e) { return Comp(e)->status; }
    uint64_t LastTick() const { return tick - 1; }

    // ブラックボードは今のところ書く経路が無い (ノードも ABI も後続サブ)。テストだけが直接書く
    BtInstance* Mutable(EntityID e) { return const_cast<BtInstance*>(bt.FindInstance(e)); }

    const BtNodeState* NodeState(EntityID e, int index)
    {
        const BtInstance* inst = bt.FindInstance(e);
        return inst != nullptr && static_cast<size_t>(index) < inst->nodes.size() ? &inst->nodes[static_cast<size_t>(index)]
                                                                                  : nullptr;
    }
};

// n tick 進め、毎 tick の status を返す
std::vector<int32_t> Run(Sim& sim, EntityID e, int ticks)
{
    std::vector<int32_t> statuses;
    for (int i = 0; i < ticks; ++i) {
        sim.Step();
        statuses.push_back(sim.Status(e));
    }
    return statuses;
}

bool Is(const std::vector<int32_t>& got, std::initializer_list<int32_t> want)
{
    return std::equal(got.begin(), got.end(), want.begin(), want.end());
}

// from 以降に出た警告のうち、本文に needle を含むものの数
int CountWarnings(uint64_t from, const char* needle)
{
    uint64_t cursor = from;
    LogEntry entries[64];
    int count = 0;
    for (;;) {
        const size_t got = logging::ReadSince(cursor, entries, 64);
        if (got == 0) {
            break;
        }
        for (size_t i = 0; i < got; ++i) {
            if (entries[i].level == LogLevel::Warn && std::strstr(entries[i].message, needle) != nullptr) {
                ++count;
            }
        }
    }
    return count;
}

// 100 体 x 30 ノードの計測用の木。Sequence の根の下に 4 群 (各 7 ノード) と末尾の Wait を置く
json PerfTree()
{
    std::vector<json> nodes;
    std::vector<int> rootChildren;
    int id = 1;
    for (int group = 0; group < 4; ++group) {
        const int selector = id++;
        const int failLeaf = id++;
        const int parallel = id++;
        const int mainWait = id++;
        const int backgroundSequence = id++;
        const int bgWaitA = id++;
        const int bgWaitB = id++;
        nodes.push_back(Node(selector, "Selector", { failLeaf, parallel }));
        nodes.push_back(Node(failLeaf, "Selector"));
        nodes.push_back(Parallel(parallel, mainWait, backgroundSequence, "Delayed"));
        nodes.push_back(Wait(mainWait, 5 + group));
        nodes.push_back(Node(backgroundSequence, "Sequence", { bgWaitA, bgWaitB }));
        nodes.push_back(Wait(bgWaitA, 7));
        nodes.push_back(Wait(bgWaitB, 9));
        rootChildren.push_back(selector);
    }
    const int tail = id++;
    rootChildren.push_back(tail);
    nodes.push_back(Wait(tail, 3));
    nodes.insert(nodes.begin(), Node(0, "Sequence", rootChildren));
    return Tree(0, nodes);
}

// ---- Task 4 種 (M85c) の部品 ----

json WithKeys(json node, json keys)
{
    node["keys"] = std::move(keys);
    return node;
}

json MoveTo(int id, const char* key, double radius = 0.5, bool observe = true, bool failOnStuck = false, uint64_t navFilter = 0)
{
    return WithKeys(Node(id, "MoveTo", {}, json{ { "acceptanceRadius", radius }, { "observeTarget", observe },
                                                  { "failOnStuck", failOnStuck },
                                                  { "navFilter", navFilter != 0 ? GuidHex(navFilter) : std::string() } }),
                    json{ { "target", key } });
}

json RotateTo(int id, const char* key, double speedDeg, double toleranceDeg)
{
    return WithKeys(Node(id, "RotateTo", {}, json{ { "angularSpeedDeg", speedDeg }, { "toleranceDeg", toleranceDeg } }),
                    json{ { "target", key } });
}

json SetBb(int id, const char* key, const char* source, json params = json::object(), const char* sourceKey = nullptr)
{
    params["source"] = source;
    json keys = json{ { "key", key } };
    if (sourceKey != nullptr) {
        keys["sourceKey"] = sourceKey;
    }
    return WithKeys(Node(id, "SetBlackboard", {}, std::move(params)), std::move(keys));
}

json ClearBb(int id, const char* key)
{
    return WithKeys(Node(id, "ClearBlackboard"), json{ { "key", key } });
}

// ---- AI ノード 4 種 (M85d) の部品 ----

json FindRandom(int id, const char* center, const char* result, double radius)
{
    json keys = json{ { "result", result } };
    if (center != nullptr) {
        keys["center"] = center;
    }
    return WithKeys(Node(id, "FindRandomPoint", {}, json{ { "radius", radius } }), std::move(keys));
}

json FindNearest(int id, bool sight, bool hearing, bool damage, bool touch, bool currentOnly, const char* target,
                 const char* position = nullptr)
{
    json keys = json{ { "target", target } };
    if (position != nullptr) {
        keys["position"] = position;
    }
    return WithKeys(Node(id, "FindNearestTarget", {},
                         json{ { "sight", sight }, { "hearing", hearing }, { "damage", damage }, { "touch", touch },
                               { "currentlySensedOnly", currentOnly } }),
                    std::move(keys));
}

json Search(int id, const char* origin, const char* endTarget, bool usePrediction, double radius, int points, bool failOnStuck = false)
{
    json keys = json{ { "origin", origin } };
    if (endTarget != nullptr) {
        keys["endTarget"] = endTarget;
    }
    return WithKeys(Node(id, "SearchArea", {}, json{ { "usePrediction", usePrediction }, { "radius", radius }, { "pointCount", points }, { "failOnStuck", failOnStuck } }),
                    std::move(keys));
}

json FindTargetNode(int id, double radius, bool enemies, bool neutrals, bool friendlies, uint64_t tagMask, const char* target)
{
    return WithKeys(Node(id, "FindTarget", {},
                         json{ { "radius", radius }, { "enemies", enemies }, { "neutrals", neutrals }, { "friendlies", friendlies },
                               { "tagMask", tagMask != 0 ? GuidHex(tagMask) : std::string() } }),
                    json{ { "target", target } });
}

// ---- PlayAnimation / SubTree (M85f) の部品 ----

json PlayAnim(int id, const char* state, int durationTicks, bool waitForEnd)
{
    return Node(id, "PlayAnimation", {}, json{ { "state", state }, { "durationTicks", durationTicks }, { "waitForEnd", waitForEnd } });
}

json SubTreeNode(int id, uint64_t tree)
{
    return Node(id, "SubTree", {}, json{ { "tree", tree != 0 ? GuidHex(tree) : std::string() } });
}

// ---- イベント (M85e) の部品 ----

json WithEventName(json key, const char* eventName)
{
    key["eventName"] = eventName;
    return key;
}

json SendEv(int id, const char* eventName, const char* target, int intValue = 0, double floatValue = 0.0,
            const char* targetKey = nullptr, const char* vectorKey = nullptr)
{
    json keys = json::object();
    if (targetKey != nullptr) {
        keys["target"] = targetKey;
    }
    if (vectorKey != nullptr) {
        keys["vector"] = vectorKey;
    }
    return WithKeys(Node(id, "SendEvent", {},
                         json{ { "eventName", eventName }, { "target", target }, { "floatValue", floatValue }, { "intValue", intValue } }),
                    std::move(keys));
}

float YawDegOf(const DirectX::XMFLOAT4& q)
{
    return 2.0f * std::atan2(q.y, q.w) * (180.0f / 3.14159265f);
}

float HorizontalGap(const LocalTransform& a, float x, float z)
{
    return std::sqrt((a.position.x - x) * (a.position.x - x) + (a.position.z - z) * (a.position.z - z));
}

// BT -> Nav -> 物理 -> Nav 後処理 -> Transform の順に回す最小の tick (フェーズ 3.4a2 -> 3.4b -> 3.6 -> 4)
struct NavSim {
    explicit NavSim(Scene& s) : scene(s) {}
    Scene& scene;
    BehaviorTreeSystem ownBt;
    BehaviorTreeSystem* bt = &ownBt;
    NavSystem nav;
    PhysicsSystem physics;
    TransformSystem transforms;
    PerceptionSystem perception;
    std::vector<SolidContact> contacts;
    bool perceive = true; // false: 知覚を回さない (percepts を手で書く試験用)
    uint64_t tick = 1;

    static constexpr float kDt = 1.0f / 60.0f;

    World& GetWorld() { return scene.GetWorld(); }
    void Step()
    {
        if (perceive) {
            perception.Update(GetWorld(), tick, kDt, contacts); // フェーズ 3.4a (BT の前)。AIPerception が無ければ何もしない
        }
        bt->DeliverPending(tick);
        bt->Update(GetWorld(), tick, &nav);
        nav.Update(GetWorld(), kDt);
        physics.Update(GetWorld(), kDt);
        nav.PostPhysics(GetWorld(), kDt);
        transforms.Update(GetWorld());
        GetWorld().ApplyStructuralChanges();
        ++tick;
    }
    BtInstance* Mutable(EntityID e) { return const_cast<BtInstance*>(bt->FindInstance(e)); }
    NavMeshAgentComponent* Agent(EntityID e) { return GetWorld().GetComponent<NavMeshAgentComponent>(e); }
    LocalTransform* Pos(EntityID e) { return GetWorld().GetComponent<LocalTransform>(e); }
    BehaviorTreeComponent* Comp(EntityID e) { return GetWorld().GetComponent<BehaviorTreeComponent>(e); }
};

constexpr uint64_t kFieldNavGuid = 0x42544E4156463031ull; // メモリ登録のナビメッシュ (ファイルを作らない)

// 24 x 24 m の床と、それを覆う Surface。ナビメッシュは最初の 1 回だけ焼き、以降は同じものを参照する
// (壁などベイク後に足す物は、物理にだけ効いてナビメッシュには無い = Agent は経路どおり進んで押し合う)
bool BuildField(Scene& scene, bool& baked)
{
    GameObject ground = scene.CreateGameObjectTracked("Ground");
    ground.SetLocalPosition(0.0f, -0.5f, 0.0f);
    auto* collider = ground.AddComponent<ColliderComponent>();
    collider->shape = collidershape::kBox;
    collider->halfExtents = { 12.0f, 0.5f, 12.0f };
    GameObject surfaceObject = scene.CreateGameObjectTracked("Surface");
    auto* surface = surfaceObject.AddComponent<NavMeshSurfaceComponent>();
    surface->center = { 0.0f, 3.0f, 0.0f };
    surface->size = { 26.0f, 10.0f, 26.0f };
    World& world = scene.GetWorld();
    if (!baked) {
        world.ApplyStructuralChanges();
        TransformSystem transforms;
        transforms.Update(world);
        NavBakeInputs in;
        if (!NavPrepareBakeInputs(world, surfaceObject.Id(), in)) {
            return false;
        }
        const NavBakeOutput out = NavBakeAsset(in.config, in.soup, in.clipBoxes, nullptr);
        if (out.status != NavBakeStatus::Ok) {
            return false;
        }
        NavMeshAsset::RegisterInMemory(kFieldNavGuid, out.data);
        baked = true;
    }
    world.GetComponent<NavMeshSurfaceComponent>(surfaceObject.Id())->navAsset = AssetID{ kFieldNavGuid };
    return true;
}

// 立っている Agent (CC のカプセルは足元 y = 0)。木を持つ
EntityID AddWalker(Scene& scene, uint64_t tree, float x, float z, const char* name = "Walker")
{
    GameObject go = scene.CreateGameObjectTracked(name);
    go.SetLocalPosition(x, 0.9f, z);
    go.AddComponent<CharacterControllerComponent>();
    go.AddComponent<NavMeshAgentComponent>();
    go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ tree };
    return go.Id();
}

} // namespace

bool RunBehaviorTreeSelfTest()
{
    MYE_LOG_INFO("==== BehaviorTree (M85a) self test ====");
    RegisterBuiltinComponents();
    Checker ck;
    LibraryScope lib;

    // 組込みコンポーネントは登録順 = TypeId。既存の型の後ろへ足す (途中へ挟むと既存シーンの型 ID が動く)
    MYE_LOG_INFO("  BehaviorTree TypeId = %u", BehaviorTreeComponent::sTypeId);
    ck.Check(BehaviorTreeComponent::sTypeId == AIStimulusSourceComponent::sTypeId + 1, "BehaviorTree は AIStimulusSource の次の TypeId");

    // ---- 1. アセット: 往復と防波堤 ----
    {
        const json full = Tree(0, { Node(0, "Selector", { 1, 2 }), Wait(1, 30, 5), Parallel(2, 3, 4, "Delayed"),
                                    Wait(3, 10), Node(4, "Sequence") });
        BehaviorTreeAsset a;
        BehaviorTreeAsset b;
        const bool loaded = BehaviorTreeLibrary::FromJson(full, a);
        const bool again = loaded && BehaviorTreeLibrary::FromJson(BehaviorTreeLibrary::ToJson(a), b);
        ck.Check(loaded && again && BehaviorTreeLibrary::ToJson(a) == BehaviorTreeLibrary::ToJson(b),
                 "木の JSON が往復で変わらない");
        ck.Check(loaded && a.nodes.size() == 5 && a.rootIndex == 0 && a.nodes[0].children.size() == 2
                     && a.nodes[2].parent == 0 && a.nodes[1].pos[0] == 10.0f && a.nodes[1].pos[1] == -5.0f
                     && a.nodes[1].params[0].i == 30 && a.nodes[1].params[1].i == 5
                     && a.nodes[2].params[0].i == btparallelfinish::kDelayed,
                 "ノードの位置・パラメータ・親子が読める");
        ck.Check(!Loads(json{ { "behaviortree_not", 1 } }) && !Loads(json::array()),
                 "種別キーの無い JSON は木として読まない");
        ck.Check(!Loads(Tree(0, { Node(0, "Teleport") })), "未知の type は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Selector"), Node(0, "Sequence") })), "id の重複は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Selector", { 7 }) })), "存在しない子への参照は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Selector", { 0 }) })), "自分自身を子にすると読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Selector", { 1 }), Node(1, "Sequence", { 2 }), Node(2, "Selector", { 1 }) })),
                 "循環は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Selector", { 2 }), Node(1, "Selector", { 2 }), Node(2, "Sequence") })),
                 "親が 2 つのノードは読み込み失敗");
        ck.Check(!Loads(Tree(1, { Node(0, "Selector", { 1 }), Node(1, "Sequence") })), "親を持つノードは根にできない");
        ck.Check(!Loads(Tree(9, { Node(0, "Selector") })), "存在しない根は読み込み失敗");
        ck.Check(Loads(Tree(-1, {})), "ノードの無い木 (根なし) は読み込める");
        ck.Check(!Loads(Tree(0, { Node(0, "SimpleParallel", { 1 }), Wait(1, 3) })), "SimpleParallel の子が 2 つでなければ読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Wait", { 1 }), Wait(1, 3) })), "葉に子を付けると読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "SimpleParallel", { 1, 2 }, json{ { "finishMode", "Sometimes" } }), Wait(1, 3), Wait(2, 3) })),
                 "未知の列挙名は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "Wait", {}, json{ { "ticks", "ten" } }) })), "型の違うパラメータは読み込み失敗");
        {
            ck.Check(!Loads(Tree(0, { Decorated(Node(0, "Selector"), { Deco("Teleport") }) })), "未知の Decorator は読み込み失敗");
        }
        BehaviorTreeAsset clamped;
        ck.Check(BehaviorTreeLibrary::FromJson(Tree(0, { Wait(0, -5, 99999999) }), clamped)
                     && clamped.nodes[0].params[0].i == 0 && clamped.nodes[0].params[1].i == kBtMaxTicksParam,
                 "範囲外の tick 数は範囲へ丸める");
        std::vector<json> deep;
        for (int i = 0; i < kBtMaxDepth + 2; ++i) {
            deep.push_back(Node(i, "Selector", i + 1 < kBtMaxDepth + 2 ? std::vector<int>{ i + 1 } : std::vector<int>{}));
        }
        ck.Check(!Loads(Tree(0, deep)), "深すぎる木は読み込み失敗");
        std::vector<json> wide;
        for (int i = 0; i < kBtMaxNodes + 1; ++i) {
            wide.push_back(Node(i, "Selector"));
        }
        ck.Check(!Loads(Tree(0, wide)), "ノード数の上限を超えると読み込み失敗");
    }
    {
        const json board = Board({ BbKey("Alert", "Bool", true), BbKey("Count", "Int", 7), BbKey("Speed", "Float", 1.5),
                                   BbKey("Home", "Vector", json::array({ 1.0, 2.0, 3.0 })), BbKey("Target", "Entity") });
        BlackboardAsset a;
        BlackboardAsset b;
        const bool loaded = BlackboardLibrary::FromJson(board, a);
        ck.Check(loaded && a.keys.size() == 5 && a.keys[0].initial.isSet == 1 && a.keys[0].initial.i == 1
                     && a.keys[1].initial.i == 7 && a.keys[2].initial.f == 1.5f && a.keys[3].initial.v[2] == 3.0f
                     && a.keys[4].initial.isSet == 0 && a.FindKey("Home") == 3 && a.FindKey("None") == -1,
                 "ブラックボードの型と初期値が読める");
        ck.Check(loaded && BlackboardLibrary::FromJson(BlackboardLibrary::ToJson(a), b)
                     && BlackboardLibrary::ToJson(a) == BlackboardLibrary::ToJson(b),
                 "ブラックボードの JSON が往復で変わらない");
        ck.Check(!BlackboardLibrary::FromJson(Board({ BbKey("A", "Bool"), BbKey("A", "Int") }), a), "キー名の重複は読み込み失敗");
        ck.Check(!BlackboardLibrary::FromJson(Board({ BbKey("", "Bool") }), a), "空のキー名は読み込み失敗");
        ck.Check(!BlackboardLibrary::FromJson(Board({ BbKey(std::string(64, 'x').c_str(), "Bool") }), a),
                 "長すぎるキー名は読み込み失敗");
        ck.Check(!BlackboardLibrary::FromJson(Board({ BbKey("A", "Rotator") }), a), "未知の型は読み込み失敗");
        std::vector<json> many;
        for (int i = 0; i <= kBbMaxKeys; ++i) {
            many.push_back(BbKey(("K" + std::to_string(i)).c_str(), "Int"));
        }
        ck.Check(!BlackboardLibrary::FromJson(Board(many), a), "キー数の上限を超えると読み込み失敗");
        ck.Check(!BlackboardLibrary::FromJson(json{ { "engine", "MyEngine" } }, a), "種別キーの無い JSON はブラックボードとして読まない");
    }
    {
        // ライブラリ: 同じ GUID の再登録は置き換わり、古い木は持っている側が使い続けられる
        const uint64_t first = RegisterTree(lib, L"reload_probe", Tree(0, { Wait(0, 5) }));
        const auto before = lib.trees.GetShared(first);
        const uint64_t second = RegisterTree(lib, L"reload_probe", Tree(0, { Wait(0, 9) }));
        const auto after = lib.trees.GetShared(second);
        ck.Check(first != 0 && first == second && before != after && before->nodes[0].params[0].i == 5
                     && after->nodes[0].params[0].i == 9,
                 "同じ GUID の再登録で置き換わり、古い木は shared_ptr が生かす");
        RegisterTree(lib, L"zeta", Tree(0, { Node(0, "Selector") }));
        RegisterTree(lib, L"alpha", Tree(0, { Node(0, "Selector") }));
        const std::vector<BehaviorTreeEntry> entries = lib.trees.Enumerate();
        bool sorted = entries.size() >= 3;
        for (size_t i = 1; i < entries.size(); ++i) {
            sorted = sorted && entries[i - 1].name <= entries[i].name;
        }
        ck.Check(sorted, "Enumerate は名前順");
    }
    {
        // ファイルからの読み込み: 名前はファイル名から、壊れたファイルは 0
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_bt_selftest";
        fs::create_directories(dir, ec);
        const fs::path good = dir / L"patrol.bt.json";
        const fs::path bad = dir / L"broken.bt.json";
        {
            std::ofstream f(good, std::ios::binary);
            f << BehaviorTreeLibrary::ToJson([] {
                BehaviorTreeAsset a;
                BehaviorTreeLibrary::FromJson(Tree(0, { Wait(0, 4) }), a);
                return a;
            }()).dump(2);
            std::ofstream g(bad, std::ios::binary);
            g << "{ not json";
        }
        const uint64_t loaded = lib.trees.LoadFromFile(good.wstring());
        const BehaviorTreeAsset* asset = lib.trees.Get(loaded);
        ck.Check(loaded != 0 && asset != nullptr && asset->name == "patrol" && asset->nodes.size() == 1,
                 ".bt.json をファイルから読める (名前はファイル名)");
        ck.Check(lib.trees.LoadFromFile(bad.wstring()) == 0 && lib.trees.LoadFromFile((dir / L"missing.bt.json").wstring()) == 0,
                 "壊れた・無いファイルは 0");
        fs::remove_all(dir, ec);
    }
    {
        // 起動走査 (Editor / Runtime / Server が共通で通る RegisterAssetLibraries) が両方の資産を登録する
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / L"mye_bt_scan_selftest";
        fs::remove_all(root, ec);
        fs::create_directories(root / L"ai", ec);
        const fs::path treeFile = root / L"ai" / L"scanned.bt.json";
        const fs::path boardFile = root / L"ai" / L"scanned.bb.json";
        {
            BehaviorTreeAsset tree;
            BehaviorTreeLibrary::FromJson(Tree(0, { Wait(0, 4) }), tree);
            std::ofstream(treeFile, std::ios::binary) << BehaviorTreeLibrary::ToJson(tree).dump(2);
            BlackboardAsset board;
            BlackboardLibrary::FromJson(Board({ BbKey("Alert", "Bool", true) }), board);
            std::ofstream(boardFile, std::ios::binary) << BlackboardLibrary::ToJson(board).dump(2);
        }
        EngineContext ctx;
        ctx.assetsRoot = root.wstring();
        RegisterAssetLibraries(ctx);
        ck.Check(lib.trees.Contains(BehaviorTreeLibrary::HashForPath(treeFile.wstring()))
                     && lib.boards.Contains(BlackboardLibrary::HashForPath(boardFile.wstring())),
                 "起動走査が .bt.json と .bb.json を登録する");
        fs::remove_all(root, ec);
    }

    // ---- 2. Composite と Wait ----
    {
        Sim sim;
        const uint64_t emptySelector = RegisterTree(lib, L"empty_selector", Tree(0, { Node(0, "Selector") }));
        const uint64_t emptySequence = RegisterTree(lib, L"empty_sequence", Tree(0, { Node(0, "Sequence") }));
        const EntityID s = sim.AddTreeEntity(emptySelector);
        const EntityID q = sim.AddTreeEntity(emptySequence);
        sim.Step();
        ck.Check(sim.Status(s) == kFailed && sim.Status(q) == kSucceeded, "空の Selector は Failure、空の Sequence は Success");
        sim.Step();
        ck.Check(sim.Status(s) == kFailed && sim.Status(q) == kSucceeded && sim.Comp(s)->activeNodeId == -1,
                 "根が終わった次の tick に根からやり直す (結果が毎 tick 出る)");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(
            RegisterTree(lib, L"selector_wait", Tree(0, { Node(0, "Selector", { 1, 2 }), Node(1, "Selector"), Wait(2, 3) })));
        const std::vector<int32_t> got = Run(sim, e, 4);
        ck.Check(Is(got, { kRunning, kRunning, kRunning, kSucceeded }), "Selector: 失敗した子の次の子 (Wait 3) が終わると Success");
        sim.Step();
        ck.Check(sim.Status(e) == kRunning && sim.Comp(e)->activeNodeId == 2, "Wait は入った tick に Running で、実行中のノード id が出る");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"sequence_fail", Tree(0, { Node(0, "Sequence", { 1, 2, 3 }), Wait(1, 2), Node(2, "Selector"), Wait(3, 5) })));
        const uint64_t rngBefore = sim.GetWorld().Rng().State();
        const std::vector<int32_t> got = Run(sim, e, 3);
        const BtNodeState* tail = sim.NodeState(e, 3);
        ck.Check(Is(got, { kRunning, kRunning, kFailed }) && tail != nullptr && tail->active == 0,
                 "Sequence: 子が Failure した tick で Failure、後ろの子には入らない");
        ck.Check(sim.GetWorld().Rng().State() == rngBefore, "偏差 0 の Wait は RNG を引かない");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"sequence_ok", Tree(0, { Node(0, "Sequence", { 1, 2, 3 }), Wait(1, 0), Wait(2, 0), Node(3, "Sequence") })));
        ck.Check(Is(Run(sim, e, 2), { kSucceeded, kSucceeded }), "Sequence: 全部 Success なら入った tick に Success (Wait 0 は即終了)");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(
            RegisterTree(lib, L"restart", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 2) })));
        ck.Check(Is(Run(sim, e, 6), { kRunning, kRunning, kSucceeded, kRunning, kRunning, kSucceeded }),
                 "Success の次の tick に根からやり直して同じ周期で回る");
    }
    {
        // SimpleParallel (子 0 = メイン、子 1 = 背景)
        Sim sim;
        const uint64_t immediate = RegisterTree(
            lib, L"par_immediate", Tree(0, { Parallel(0, 1, 2, "Immediate"), Wait(1, 2), Wait(2, 10) }));
        const EntityID e = sim.AddTreeEntity(immediate);
        const std::vector<int32_t> got = Run(sim, e, 3);
        const BtNodeState* background = sim.NodeState(e, 2);
        ck.Check(Is(got, { kRunning, kRunning, kSucceeded }) && background != nullptr && background->active == 0
                     && sim.Comp(e)->lastAbortTick == static_cast<int32_t>(sim.LastTick()),
                 "SimpleParallel Immediate: メインが終わったら背景を Abort して Success (Abort した tick が記録される)");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"par_delayed", Tree(0, { Parallel(0, 1, 2, "Delayed"), Wait(1, 2), Wait(2, 5) })));
        const std::vector<int32_t> got = Run(sim, e, 6);
        ck.Check(Is(got, { kRunning, kRunning, kRunning, kRunning, kRunning, kSucceeded }) && sim.Comp(e)->lastAbortTick == -1,
                 "SimpleParallel Delayed: メインが終わっても背景が終わるまで待ち、Abort しない");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"par_background_first", Tree(0, { Parallel(0, 1, 2, "Immediate"), Wait(1, 10), Wait(2, 2) })));
        Run(sim, e, 3);
        const BtNodeState* afterFinish = sim.NodeState(e, 2);
        const bool finishedAndIdle = afterFinish != nullptr && afterFinish->active == 0;
        sim.Step();
        const BtNodeState* restarted = sim.NodeState(e, 2);
        ck.Check(finishedAndIdle && restarted != nullptr && restarted->active == 1 && restarted->counter == 2
                     && sim.Status(e) == kRunning,
                 "SimpleParallel: 背景が先に終わったら次の tick に背景だけ最初からやり直す");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"par_main_fails", Tree(0, { Parallel(0, 1, 2, "Immediate"), Node(1, "Selector"), Wait(2, 5) })));
        ck.Check(Is(Run(sim, e, 1), { kFailed }) && sim.NodeState(e, 2)->active == 0 && sim.Comp(e)->lastAbortTick == -1,
                 "SimpleParallel: メインが Failure ならそのまま Failure (背景に入っていなければ Abort も無い)");
    }

    // ---- 3. Wait の偏差は World の RNG だけ ----
    {
        const uint64_t guid = RegisterTree(lib, L"wait_dev", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 10, 4) }));
        std::vector<int32_t> a;
        std::vector<int32_t> b;
        uint64_t rngA = 0;
        uint64_t rngStart = 0;
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(guid);
            rngStart = sim.GetWorld().Rng().State();
            a = Run(sim, e, 200);
            rngA = sim.GetWorld().Rng().State();
        }
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(guid);
            b = Run(sim, e, 200);
        }
        const int successes = static_cast<int>(std::count(a.begin(), a.end(), kSucceeded));
        ck.Check(a == b && rngA != rngStart && successes >= 12 && successes <= 30,
                 "偏差のある Wait は同じ入力で同じ結果 (RNG を引き、周期は 10 ± 4 の範囲)");
    }

    // ---- 4. 1 tick の手数の上限 ----
    {
        std::vector<json> nodes;
        std::vector<int> kids;
        for (int i = 1; i <= 300; ++i) {
            nodes.push_back(Node(i, "Sequence"));
            kids.push_back(i);
        }
        nodes.insert(nodes.begin(), Node(0, "Sequence", kids));
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(lib, L"many_instant", Tree(0, nodes)));
        sim.Step();
        const BtNodeState* root = sim.NodeState(e, 0);
        const bool stoppedAtLimit = sim.Status(e) == kRunning && root != nullptr && root->child == kBtMaxStepsPerTick - 1;
        sim.Step();
        ck.Check(stoppedAtLimit && sim.Status(e) == kSucceeded,
                 "即終了するノードが 256 手を超えると、その tick は止まって次の tick に続きから終わる");
    }

    // ---- 5. ライフサイクル ----
    {
        Sim sim;
        const uint64_t guid = RegisterTree(lib, L"life", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 20) }));
        const EntityID e = sim.AddTreeEntity(guid);
        Run(sim, e, 3);
        sim.Comp(e)->enabled = false;
        sim.Step();
        const BtInstance* inst = sim.bt.FindInstance(e);
        ck.Check(sim.Status(e) == btstatus::kIdle && inst != nullptr && sim.NodeState(e, 1)->active == 0
                     && sim.Comp(e)->lastAbortTick == static_cast<int32_t>(sim.LastTick()),
                 "enabled を落とすと実行中を Abort して Idle になり、表には残る");
        sim.Comp(e)->enabled = true;
        sim.Step();
        ck.Check(sim.Status(e) == kRunning && sim.NodeState(e, 1)->counter == 20, "再び有効にすると根から始まる");

        sim.GetWorld().RemoveComponent<BehaviorTreeComponent>(e);
        sim.GetWorld().ApplyStructuralChanges();
        sim.Step();
        ck.Check(sim.bt.InstanceCount() == 0 && !sim.bt.HasHashableState(), "コンポーネントが外れると表から消える");
    }
    {
        Sim sim;
        const uint64_t guid = RegisterTree(lib, L"gone", Tree(0, { Wait(0, 20) }));
        const EntityID gone = sim.AddTreeEntity(guid);
        const EntityID stays = sim.AddTreeEntity(guid);
        sim.Step();
        sim.GetWorld().DestroyEntity(gone);
        sim.GetWorld().ApplyStructuralChanges();
        sim.Step();
        ck.Check(sim.bt.InstanceCount() == 1 && sim.bt.FindInstance(stays) != nullptr && sim.bt.FindInstance(gone) == nullptr,
                 "エンティティが消えるとその表だけが消える");
    }
    {
        Sim sim;
        const EntityID missing = sim.AddTreeEntity(0x1234567ull, "Missing");
        const EntityID none = sim.AddTreeEntity(0, "None");
        sim.Step();
        ck.Check(sim.Status(missing) == btstatus::kAssetMissing && sim.Status(none) == btstatus::kIdle
                     && sim.bt.InstanceCount() == 0 && !sim.bt.HasHashableState(),
                 "未登録の木は AssetMissing、未設定は Idle (どちらも表を作らない)");
    }
    {
        Sim sim;
        const uint64_t board = RegisterBoard(lib, L"life_bb", Board({ BbKey("Alert", "Bool", true), BbKey("Count", "Int", 7) }));
        const uint64_t guid = RegisterTree(lib, L"life_bt", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 20) }, board));
        const EntityID e = sim.AddTreeEntity(guid);
        sim.Step();
        const BtInstance* inst = sim.bt.FindInstance(e);
        ck.Check(inst != nullptr && inst->blackboard.size() == 2 && inst->blackboard[0].isSet == 1 && inst->blackboard[0].i == 1
                     && inst->blackboard[1].i == 7,
                 "ブラックボードは入った tick に初期値で作られる");
        sim.Mutable(e)->blackboard[1].i = 99;
        const uint64_t uses = RegisterTree(lib, L"life_bt", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 5) }, board));
        sim.Step();
        ck.Check(uses == guid && sim.Status(e) == kRunning && sim.NodeState(e, 1)->counter == 5
                     && sim.bt.FindInstance(e)->blackboard[1].i == 99
                     && sim.Comp(e)->lastAbortTick == static_cast<int32_t>(sim.LastTick()),
                 "木が読み直されると Abort して根からやり直し、ブラックボードの値は保つ");
        RegisterBoard(lib, L"life_bb", Board({ BbKey("Alert", "Bool", false), BbKey("Count", "Int", 3), BbKey("Extra", "Float") }));
        sim.Step();
        const BtInstance* reloaded = sim.bt.FindInstance(e);
        ck.Check(reloaded != nullptr && reloaded->blackboard.size() == 3 && reloaded->blackboard[1].i == 3
                     && reloaded->blackboard[0].i == 0 && reloaded->blackboard[2].isSet == 0,
                 "ブラックボードが読み直されると定義の初期値へ戻る");
        sim.Comp(e)->tree = AssetID{ 0x7777ull };
        sim.Step();
        ck.Check(sim.Status(e) == btstatus::kAssetMissing && sim.bt.InstanceCount() == 0, "木を未登録の GUID へ差し替えると表を捨てる");
        const uint64_t noBoard = RegisterTree(lib, L"needs_missing_bb", Tree(0, { Wait(0, 3) }, 0xABCDEFull));
        sim.Comp(e)->tree = AssetID{ noBoard };
        sim.Step();
        ck.Check(sim.Status(e) == btstatus::kAssetMissing, "木が使うブラックボードが未登録なら AssetMissing");
    }
    {
        // Scene::Clear (Play の開始・終了のシーン読み直し) は世代を進めるので、同じ index でも前の表のキーとは一致しない
        const uint64_t guid = RegisterTree(lib, L"cleared", Tree(0, { Wait(0, 10) }));
        Sim sim;
        const EntityID before = sim.AddTreeEntity(guid);
        for (int i = 0; i < 5; ++i) {
            sim.Step();
        }
        sim.scene.Clear();
        const EntityID after = sim.AddTreeEntity(guid);
        sim.Step();
        const BtInstance* inst = sim.bt.FindInstance(after);
        ck.Check(before.index == after.index && before.generation != after.generation && sim.bt.InstanceCount() == 1
                     && inst != nullptr && inst->nodes[0].counter == 10,
                 "シーンを読み直す (Clear) と前の表は引き継がれず、新しいエンティティは根から始まる");
    }
    {
        Sim sim;
        const uint64_t guid = RegisterTree(lib, L"order", Tree(0, { Wait(0, 50) }));
        std::vector<EntityID> made;
        for (int i = 0; i < 6; ++i) {
            made.push_back(sim.AddTreeEntity(guid));
        }
        sim.Step();
        sim.GetWorld().DestroyEntity(made[2]);
        sim.GetWorld().ApplyStructuralChanges();
        made[2] = sim.AddTreeEntity(guid); // 空いた index を再利用する (世代が上がる)
        sim.GetWorld().ApplyStructuralChanges();
        sim.Step();
        bool ascending = true;
        const std::vector<BtInstance>& list = sim.bt.Instances();
        for (size_t i = 1; i < list.size(); ++i) {
            const EntityID a = list[i - 1].entity;
            const EntityID b = list[i].entity;
            ascending = ascending && (a.index != b.index ? a.index < b.index : a.generation < b.generation);
        }
        ck.Check(list.size() == 6 && ascending, "表はエンティティキー昇順");
    }

    // ---- 6. BT が無いシーンを動かさない ----
    {
        Sim sim;
        sim.GetWorld().ApplyStructuralChanges();
        const uint64_t before = HashWorld(sim.GetWorld());
        const uint64_t rngBefore = sim.GetWorld().Rng().State();
        SimSources withBt;
        withBt.behaviorTree = &sim.bt;
        for (int i = 0; i < 10; ++i) {
            sim.Step();
        }
        ck.Check(HashWorld(sim.GetWorld()) == before && HashWorld(sim.GetWorld(), withBt) == before
                     && sim.GetWorld().Rng().State() == rngBefore && !sim.bt.HasHashableState(),
                 "BehaviorTree が無ければハッシュも RNG も動かない (BT 節も畳まない)");
    }

    // ---- 7. SimSnapshot の BT 節 ----
    {
        const uint64_t board = RegisterBoard(lib, L"snap_bb", Board({ BbKey("Count", "Int", 1), BbKey("Home", "Vector", json::array({ 1.0, 0.0, 2.0 })) }));
        const uint64_t waiter = RegisterTree(lib, L"snap_wait", Tree(0, { Node(0, "Sequence", { 1, 2 }), Wait(1, 12, 5), Wait(2, 7) }, board));
        const uint64_t parallel = RegisterTree(
            lib, L"snap_parallel",
            Tree(0, { Parallel(0, 1, 2, "Delayed"), Wait(1, 9), Node(2, "Sequence", { 3, 4 }), Wait(3, 4), Wait(4, 6, 2) }));
        const uint64_t chooser = RegisterTree(
            lib, L"snap_selector", Tree(0, { Node(0, "Selector", { 1, 2 }), Node(1, "Selector"), Node(2, "Sequence", { 3 }), Wait(3, 15) }));

        Scene scene;
        BehaviorTreeSystem first;
        std::vector<EntityID> agents;
        const uint64_t trees[] = { waiter, parallel, chooser, waiter, parallel, chooser };
        for (const uint64_t tree : trees) {
            GameObject go = scene.CreateGameObjectTracked("Agent");
            go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ tree };
            agents.push_back(go.Id());
        }
        scene.GetWorld().ApplyStructuralChanges();

        SimRefs refs;
        refs.scene = &scene;
        refs.behaviorTree = &first;
        uint64_t tickRef = 0;
        refs.tickIndex = &tickRef;

        // 毎 tick ブラックボードを直接書く (ノードも ABI もまだ書かないので、テストの入力として決定的に動かす)
        const auto drive = [&](BehaviorTreeSystem& bt, uint64_t t) {
            for (size_t i = 0; i < agents.size(); ++i) {
                BtInstance* inst = const_cast<BtInstance*>(bt.FindInstance(agents[i]));
                if (inst != nullptr && !inst->blackboard.empty() && t % 6 == i) {
                    inst->blackboard[0].i += static_cast<int32_t>(t);
                }
            }
        };
        uint64_t tick = 1;
        const auto step = [&](BehaviorTreeSystem& bt) {
            bt.DeliverPending(tick);
            bt.Update(scene.GetWorld(), tick, nullptr);
            drive(bt, tick);
            scene.GetWorld().ApplyStructuralChanges();
            ++tick;
        };
        for (int i = 0; i < 50; ++i) {
            step(first);
        }
        tickRef = tick;
        std::vector<std::byte> blob;
        ck.Check(CaptureSimSnapshot(refs, blob) && first.InstanceCount() == 6, "木が動いている途中で撮影できる");

        const uint64_t startTick = tick;
        std::vector<uint64_t> continuous;
        for (int i = 0; i < 50; ++i) {
            step(first);
            continuous.push_back(HashWorld(scene.GetWorld(), refs.HashSources()));
        }
        const uint64_t firstEndState = first.StateHash();
        ck.Check(continuous.front() != continuous.back(), "(前提) 連続実行でハッシュが動く");

        BehaviorTreeSystem second; // 新しいシステム (空の表) へ復元する
        SimRefs refsSecond = refs;
        refsSecond.behaviorTree = &second;
        ck.Check(RestoreSimSnapshot(refsSecond, blob.data(), blob.size()) && second.InstanceCount() == 6,
                 "新しいシステムへ復元できる");
        tick = startTick;
        bool same = true;
        for (int i = 0; i < 50; ++i) {
            step(second);
            same = same && HashWorld(scene.GetWorld(), refsSecond.HashSources()) == continuous[static_cast<size_t>(i)];
        }
        ck.Check(same && second.StateHash() == firstEndState,
                 "復元して 50 tick 進めた毎 tick のハッシュが連続実行と一致 (BB・実行中ノード・Wait の残り・Parallel の状態・RNG)");

        // BT 節の無い参照束へも読み捨てで戻せる
        SimRefs bare = refs;
        bare.behaviorTree = nullptr;
        ck.Check(RestoreSimSnapshot(bare, blob.data(), blob.size()), "BehaviorTreeSystem の無い構成へは BT 節を読み捨てて戻せる");

        // 表だけを書いて、同じ構成のコンポーネントを持つ World で読み直す
        std::vector<std::byte> bytes;
        ByteWriter writer(bytes);
        second.SaveSnapshot(writer);
        std::vector<std::byte> bytesAgain;
        ByteWriter writerAgain(bytesAgain);
        second.SaveSnapshot(writerAgain);
        ck.Check(bytes == bytesAgain && !bytes.empty(), "同じ状態なら同じバイト列 (決定論)");
        BtSnapshot parsed;
        ByteReader reader(bytes.data(), bytes.size());
        ck.Check(BehaviorTreeSystem::ReadSnapshot(reader, parsed) && parsed.instances.size() == 6, "書いた BT 節をそのまま読める");

        // 壊れた節は拒否する
        ByteReader truncated(bytes.data(), bytes.size() - 3);
        BtSnapshot junk;
        ck.Check(!BehaviorTreeSystem::ReadSnapshot(truncated, junk), "途中で切れた BT 節は拒否する");
        const auto craft = [](uint32_t firstIndex, uint32_t secondIndex, uint8_t rootStatus, uint8_t active, uint64_t extraCount = 0) {
            std::vector<std::byte> out;
            ByteWriter w(out);
            w.Count(2);
            const uint32_t indices[2] = { firstIndex, secondIndex };
            for (const uint32_t index : indices) {
                w.U32(index);
                w.U32(0);
                w.U64(1);
                w.U8(rootStatus);
                w.Count(0);
                w.Count(1);
                w.U8(active);
                w.U8(0);
                w.I32(0);
                w.I32(0);
                w.Count(extraCount); // 種類別の追加状態 (生バイト、ここでは中身を持たない)
                w.Raw(std::vector<uint8_t>(extraCount, 0).data(), extraCount);
            }
            w.Count(0); // 配送待ち 0 件
            return out;
        };
        const auto accepts = [&](const std::vector<std::byte>& data) {
            ByteReader r(data.data(), data.size());
            BtSnapshot s;
            return BehaviorTreeSystem::ReadSnapshot(r, s);
        };
        ck.Check(accepts(craft(1, 2, 0, 1)), "(前提) 作った BT 節は読める");
        ck.Check(!accepts(craft(2, 1, 0, 1)) && !accepts(craft(2, 2, 0, 1)), "エンティティキーが昇順でない・重複する BT 節は拒否する");
        ck.Check(!accepts(craft(1, 2, 3, 1)) && !accepts(craft(1, 2, 0, 2)), "範囲外の rootStatus / active を持つ BT 節は拒否する");
        ck.Check(accepts(craft(1, 2, 0, 1, 24)) && !accepts(craft(1, 2, 0, 1, static_cast<uint64_t>(kBtMaxExtraBytes) + 1)),
                 "種類別の追加状態は上限まで読め、上限を超える長さの BT 節は拒否する");
    }

    // ---- 9. Decorator と Abort (M85b) ----
    {
        const json full = Tree(0, { Decorated(Node(0, "Selector", { 1, 2 }), { Cooldown(30), Repeat(0), Timeout(9) }),
                                    Decorated(Wait(1, 5), { BbCond("Flag", "GreaterEqual", "Both", 12, 2.5), Deco("Invert") }), Wait(2, 3) });
        BehaviorTreeAsset a;
        BehaviorTreeAsset b;
        const bool loaded = BehaviorTreeLibrary::FromJson(full, a);
        ck.Check(loaded && BehaviorTreeLibrary::FromJson(BehaviorTreeLibrary::ToJson(a), b)
                     && BehaviorTreeLibrary::ToJson(a) == BehaviorTreeLibrary::ToJson(b),
                 "Decorator 付きの木の JSON が往復で変わらない");
        ck.Check(loaded && a.nodes[0].decorators.size() == 3 && a.nodes[1].decorators.size() == 2
                     && a.nodes[1].decorators[0].key == "Flag"
                     && a.nodes[1].decorators[0].params[btbbparam::kQuery].i == btquery::kGreaterEqual
                     && a.nodes[1].decorators[0].params[btbbparam::kAbort].i == btabort::kBoth
                     && a.nodes[1].decorators[0].params[btbbparam::kIntValue].i == 12
                     && a.nodes[1].decorators[0].params[btbbparam::kFloatValue].f == 2.5f
                     && a.nodes[0].decorators[2].params[0].i == 9,
                 "Decorator の種類・キー・パラメータが読める");
        ck.Check(loaded && a.stateSlotCount == 3 + 5 && a.nodes[0].decorators[0].slot == 3 && a.nodes[1].decorators[1].slot == 7,
                 "実行状態の欄はノード数に Decorator の数を足した数 (ノードの後ろに Decorator が並ぶ)");
        ck.Check(!Loads(Tree(0, { Decorated(Wait(0, 3), { Deco("BlackboardCondition", json::object()) }) })),
                 "BlackboardCondition にキー名が無ければ読み込み失敗");
        ck.Check(!Loads(Tree(0, { Decorated(Wait(0, 3), { Deco("BlackboardCondition", json::object(), "") }) }))
                     && !Loads(Tree(0, { Decorated(Wait(0, 3), { Deco("BlackboardCondition", json::object(), std::string(64, 'x').c_str()) }) })),
                 "空・長すぎるキー名は読み込み失敗");
        ck.Check(Loads(Tree(0, { Decorated(Wait(0, 3), { Cooldown(-1) }) }))
                     && !Loads(Tree(0, { Decorated(Wait(0, 3), { Deco("Cooldown", json{ { "ticks", "soon" } }) }) })),
                 "型の違う Decorator のパラメータは読み込み失敗");
        BehaviorTreeAsset clamped;
        ck.Check(BehaviorTreeLibrary::FromJson(Tree(0, { Decorated(Wait(0, 3), { Timeout(0), Repeat(-4) }) }), clamped)
                     && clamped.nodes[0].decorators[0].params[0].i == 1 && clamped.nodes[0].decorators[1].params[0].i == 0,
                 "範囲外の Decorator のパラメータは範囲へ丸める (Timeout は 1 以上)");
        std::vector<json> crowded;
        for (int i = 0; i <= kBtMaxDecoratorsPerNode; ++i) {
            crowded.push_back(Deco("Invert"));
        }
        ck.Check(!Loads(Tree(0, { Decorated(Wait(0, 3), crowded) })), "1 ノードの Decorator の数の上限を超えると読み込み失敗");
    }

    {
        const uint64_t board = RegisterBoard(
            lib, L"deco_bb",
            Board({ BbKey("Flag", "Bool", false), BbKey("Count", "Int", 7), BbKey("Score", "Float", 1.5),
                    BbKey("Home", "Vector", json::array({ 1.0, 2.0, 3.0 })), BbKey("Target", "Entity"), BbKey("Blank", "Int") }));
        enum { kFlag, kCount, kScore, kHome, kTarget, kBlank };

        // 空の Sequence (Success) に Decorator を 1 つ付けた木を 1 tick 動かし、BB を書き換えてもう 1 tick 動かした結果。
        // 根が終わった次の tick は根からやり直すので、2 tick 目は書き換えた値で評価される
        const auto probe = [&](const std::vector<json>& decorators, const std::function<void(Sim&, BtInstance&)>& edit) {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(
                RegisterTree(lib, L"deco_probe", Tree(0, { Decorated(Node(0, "Sequence"), decorators) }, board)));
            sim.Step();
            if (edit) {
                edit(sim, *sim.Mutable(e));
            }
            sim.Step();
            return sim.Status(e);
        };
        const auto passes = [&](const json& cond, const std::function<void(Sim&, BtInstance&)>& edit = nullptr) {
            return probe({ cond }, edit) == kSucceeded;
        };
        ck.Check(!passes(BbCond("Flag", "IsSet")) && passes(BbCond("Flag", "IsNotSet"))
                     && passes(BbCond("Flag", "IsSet"), [](Sim&, BtInstance& i) { i.blackboard[kFlag].i = 1; }),
                 "BlackboardCondition: Bool は true のとき IsSet");
        ck.Check(passes(BbCond("Count", "Equal", "None", 7)) && !passes(BbCond("Count", "NotEqual", "None", 7))
                     && passes(BbCond("Count", "Less", "None", 8)) && !passes(BbCond("Count", "Less", "None", 7))
                     && passes(BbCond("Count", "LessEqual", "None", 7)) && !passes(BbCond("Count", "Greater", "None", 7))
                     && passes(BbCond("Count", "Greater", "None", 6)) && passes(BbCond("Count", "GreaterEqual", "None", 7)),
                 "BlackboardCondition: Int の 6 つの比較");
        ck.Check(passes(BbCond("Score", "Greater", "None", 0, 1.0)) && !passes(BbCond("Score", "Less", "None", 0, 1.5))
                     && passes(BbCond("Score", "Equal", "None", 0, 1.5)),
                 "BlackboardCondition: Float は floatValue と比べる");
        ck.Check(!passes(BbCond("Blank", "IsSet")) && passes(BbCond("Blank", "IsNotSet")) && !passes(BbCond("Blank", "Equal", "None", 0))
                     && !passes(BbCond("Blank", "NotEqual", "None", 1)),
                 "BlackboardCondition: 未設定の値は IsNotSet だけが真 (大小の比較は偽)");
        ck.Check(passes(BbCond("Home", "IsSet")) && !passes(BbCond("Home", "Equal", "None", 0))
                     && !passes(BbCond("Home", "IsNotSet")),
                 "BlackboardCondition: Vector は書かれていれば IsSet");
        ck.Check(!passes(BbCond("Nothing", "IsNotSet")) && !passes(BbCond("Nothing", "IsSet")), "BlackboardCondition: 無いキーは常に偽");
        {
            EntityID alive = kNullEntity;
            EntityID dead = kNullEntity;
            const auto withTarget = [&](EntityID& target) {
                return [&target](Sim& sim, BtInstance& inst) {
                    if (target.IsNull()) {
                        target = sim.scene.CreateGameObjectTracked("Target").Id();
                        sim.GetWorld().ApplyStructuralChanges();
                    }
                    inst.blackboard[kTarget].isSet = 1;
                    inst.blackboard[kTarget].entity = target;
                };
            };
            const bool aliveIsSet = passes(BbCond("Target", "IsSet"), withTarget(alive));
            const bool deadIsSet = passes(BbCond("Target", "IsSet"), [&](Sim& sim, BtInstance& inst) {
                const EntityID doomed = sim.scene.CreateGameObjectTracked("Doomed").Id();
                sim.GetWorld().ApplyStructuralChanges();
                sim.GetWorld().DestroyEntity(doomed);
                sim.GetWorld().ApplyStructuralChanges();
                inst.blackboard[kTarget].isSet = 1;
                inst.blackboard[kTarget].entity = doomed;
                dead = doomed;
            });
            ck.Check(!dead.IsNull() && aliveIsSet && !deadIsSet && !passes(BbCond("Target", "IsSet")) && passes(BbCond("Target", "IsNotSet")),
                     "BlackboardCondition: Entity は生きているハンドルのときだけ IsSet");
        }
        ck.Check(probe({ BbCond("Count", "Equal", "None", 7), BbCond("Flag", "IsSet") }, nullptr) == kFailed
                     && probe({ BbCond("Count", "Equal", "None", 7), BbCond("Flag", "IsSet") },
                              [](Sim&, BtInstance& i) { i.blackboard[kFlag].i = 1; }) == kSucceeded,
                 "条件の Decorator が 2 つあれば両方が真のときだけ入れる");

        // 偽の条件ではノードに入らない (状態に触れず Failure)
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(RegisterTree(
                lib, L"deco_blocked", Tree(0, { Decorated(Wait(0, 5), { BbCond("Flag", "IsSet") }) }, board)));
            sim.Step();
            const BtNodeState* wait = sim.NodeState(e, 0);
            ck.Check(sim.Status(e) == kFailed && wait != nullptr && wait->active == 0 && sim.Comp(e)->lastAbortTick == -1,
                     "条件が偽なら入らずに Failure (Abort ではない)");
            sim.Mutable(e)->blackboard[kFlag].i = 1;
            ck.Check(Is(Run(sim, e, 3), { kRunning, kRunning, kRunning }), "条件が真になると次の tick の入り直しで入る");
        }
    }

    {
        // Invert
        Sim sim;
        const EntityID a = sim.AddTreeEntity(RegisterTree(lib, L"inv_a", Tree(0, { Decorated(Node(0, "Sequence"), { Deco("Invert") }) })));
        const EntityID b = sim.AddTreeEntity(RegisterTree(lib, L"inv_b", Tree(0, { Decorated(Node(0, "Selector"), { Deco("Invert") }) })));
        const EntityID c = sim.AddTreeEntity(RegisterTree(lib, L"inv_c", Tree(0, { Decorated(Wait(0, 5), { Deco("Invert") }) })));
        const EntityID d = sim.AddTreeEntity(RegisterTree(lib, L"inv_d", Tree(0, { Decorated(Node(0, "Sequence"), { Deco("Invert"), Deco("Invert") }) })));
        sim.Step();
        ck.Check(sim.Status(a) == kFailed && sim.Status(b) == kSucceeded && sim.Status(c) == kRunning && sim.Status(d) == kSucceeded,
                 "Invert: Success と Failure を入れ替え、Running はそのまま (2 重なら元に戻る)");
    }

    {
        // Cooldown: 終わった tick から ticks の間は入れない
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"cooldown", Tree(0, { Node(0, "Selector", { 1 }), Decorated(Wait(1, 2), { Cooldown(5) }) })));
        ck.Check(Is(Run(sim, e, 9), { kRunning, kRunning, kSucceeded, kFailed, kFailed, kFailed, kFailed, kRunning, kRunning }),
                 "Cooldown: Success で終わった tick から 5 tick は入れず (Failure)、明けると入る");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"cooldown_failure", Tree(0, { Node(0, "Selector", { 1 }), Decorated(Node(1, "Selector"), { Cooldown(3) }) })));
        // 空の Selector は入った tick に Failure で終わる (= 終了なので計時が始まる)。tick 1 に入って終わり、tick 2・3 は入れず、tick 4 に入り直す
        sim.Step();
        const int32_t first = sim.Status(e);
        sim.Step();
        ck.Check(first == kFailed && sim.Status(e) == kFailed
                     && sim.bt.FindInstance(e)->nodes[2].counter == 4,
                 "Cooldown: Failure で終わっても計時が始まる (入れるようになる tick = 終了 tick + ticks)");
    }
    {
        // Cooldown は Abort でも計時を始める
        const uint64_t board = RegisterBoard(lib, L"cooldown_bb", Board({ BbKey("Flag", "Bool", true) }));
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"cooldown_abort",
            Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 100), { BbCond("Flag", "IsSet", "Self"), Cooldown(6) }), Wait(2, 2) }, board)));
        sim.Step();
        const int32_t leafBefore = sim.Comp(e)->activeNodeId;
        sim.Mutable(e)->blackboard[0].i = 0;
        sim.Step(); // tick 2: Abort (計時 tick 2 + 6 = 8)、B (Wait 2) に入る
        sim.Mutable(e)->blackboard[0].i = 1;
        const int32_t leafAfterAbort = sim.Comp(e)->activeNodeId;
        std::vector<int32_t> leaves;
        for (int i = 0; i < 6; ++i) {
            sim.Step(); // tick 3..8
            leaves.push_back(sim.Comp(e)->activeNodeId);
        }
        // tick 3: B。tick 4: B が終わって根が Success (実行中なし = -1)。tick 5: 根からやり直し、条件は真だが Cooldown 中 (5 < 8) なので B。
        // tick 7: B が終わる。tick 8: Cooldown が明けて A へ入る
        ck.Check(leafBefore == 1 && leafAfterAbort == 2 && leaves == std::vector<int32_t>{ 2, -1, 2, 2, -1, 1 },
                 "Cooldown: Abort された tick からも計時が始まり、明けるまで条件が真でも入れない");
    }

    {
        // Repeat
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(lib, L"repeat3", Tree(0, { Decorated(Wait(0, 2), { Repeat(3) }) })));
        ck.Check(Is(Run(sim, e, 8), { kRunning, kRunning, kRunning, kRunning, kRunning, kRunning, kSucceeded, kRunning }),
                 "Repeat 3: 子が 3 回 Success したら Success (終わった tick に次の周回へ入る)");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"repeat_inf", Tree(0, { Decorated(Node(0, "Sequence", { 1 }), { Repeat(0) }), Wait(1, 1) })));
        std::vector<int32_t> got = Run(sim, e, 30);
        ck.Check(std::all_of(got.begin(), got.end(), [](int32_t s) { return s == kRunning; }) && sim.Comp(e)->activeNodeId == 1,
                 "Repeat 0 = 無限: Success し続けても終わらず Running のまま");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"repeat_fail",
            Tree(0, { Decorated(Node(0, "Sequence", { 1, 2 }), { Repeat(0) }), Wait(1, 1), Node(2, "Selector") })));
        ck.Check(Is(Run(sim, e, 4), { kRunning, kFailed, kRunning, kFailed }), "Repeat: 子が Failure したら Failure で抜ける (無限でも)");
    }
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"repeat_timeout", Tree(0, { Decorated(Wait(0, 2), { Repeat(0), Timeout(5) }) })));
        // Timeout は周回ごとに掛け直す: Wait 2 は 3 tick で終わるので、5 tick の Timeout は一度も切れない
        std::vector<int32_t> got = Run(sim, e, 20);
        ck.Check(std::all_of(got.begin(), got.end(), [](int32_t s) { return s == kRunning; }), "Repeat の内側の Timeout は周回ごとに計り直す");
    }

    {
        // Timeout
        Sim sim;
        std::vector<int32_t> trace;
        sim.bt.SetAbortTrace(&trace);
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"timeout", Tree(0, { Decorated(Node(0, "Sequence", { 1 }), { Timeout(5) }), Wait(1, 100) })));
        const std::vector<int32_t> got = Run(sim, e, 7);
        ck.Check(Is(got, { kRunning, kRunning, kRunning, kRunning, kRunning, kFailed, kRunning }) && trace == std::vector<int32_t>{ 1, 0 }
                     && sim.Comp(e)->lastAbortTick == 6,
                 "Timeout: 5 tick 経っても子が終わらなければ子を Abort (OnAbort は子が先) して Failure");
        Sim quick;
        const EntityID q = quick.AddTreeEntity(RegisterTree(
            lib, L"timeout_quick", Tree(0, { Decorated(Node(0, "Sequence", { 1 }), { Timeout(5) }), Wait(1, 2) })));
        ck.Check(Is(Run(quick, q, 4), { kRunning, kRunning, kSucceeded, kRunning }) && quick.Comp(q)->lastAbortTick == -1,
                 "Timeout: 間に合えば普通に終わる (Abort しない)");
        Sim tie;
        const EntityID t = tie.AddTreeEntity(RegisterTree(
            lib, L"timeout_tie", Tree(0, { Decorated(Wait(0, 5), { Timeout(5) }) })));
        ck.Check(Is(Run(tie, t, 6), { kRunning, kRunning, kRunning, kRunning, kRunning, kSucceeded }),
                 "Timeout: 切れる tick に子が終わるなら終わりを優先する");
    }

    {
        // Abort 4 種
        const uint64_t board = RegisterBoard(lib, L"abort_bb", Board({ BbKey("Flag", "Bool", false) }));
        const auto setFlag = [](Sim& sim, EntityID e, bool on) { sim.Mutable(e)->blackboard[0].i = on ? 1 : 0; };

        // Self: 部分木が実行中に条件が偽になったら、子孫から順に Abort して Failure、右の兄弟へ進む
        const auto selfTree = [&](const char* mode, const wchar_t* name) {
            return RegisterTree(
                lib, name,
                Tree(0, { Node(0, "Selector", { 1, 4 }),
                          Decorated(Node(1, "Sequence", { 2 }), { BbCond("Flag", "IsSet", mode) }),
                          Node(2, "Sequence", { 3 }), Wait(3, 100), Wait(4, 100) },
                     board));
        };
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(selfTree("Self", L"abort_self"));
            sim.Step();
            const int32_t before = sim.Comp(e)->activeNodeId; // Flag = false なので最初から右の兄弟 (4)
            setFlag(sim, e, true);
            sim.Step();
            sim.Step();
            const int32_t entered = sim.Comp(e)->activeNodeId;
            ck.Check(before == 4 && entered == 4, "(前提) 条件が真になっても Self は右の兄弟を止めない");
        }
        {
            Sim sim;
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            const uint64_t guid = selfTree("Self", L"abort_self2");
            const EntityID e = sim.AddTreeEntity(guid);
            // 初期値が false なので、表を作らせてから true にして入り直す
            sim.Step();
            setFlag(sim, e, true);
            sim.GetWorld().GetComponent<BehaviorTreeComponent>(e)->enabled = false; // Abort して根へ戻す
            sim.Step();
            sim.GetWorld().GetComponent<BehaviorTreeComponent>(e)->enabled = true;
            trace.clear();
            sim.Step(); // Flag = true で入り直す → 1 > 2 > 3 が実行中
            const int32_t running = sim.Comp(e)->activeNodeId;
            setFlag(sim, e, false);
            sim.Step();
            ck.Check(running == 3 && sim.Comp(e)->activeNodeId == 4 && trace == std::vector<int32_t>{ 3, 2, 1 }
                         && sim.Comp(e)->lastAbortTick == static_cast<int32_t>(sim.LastTick()),
                     "Abort Self: 条件が偽になった tick に子孫から順に (深い方から) Abort され、右の兄弟へ進む");
        }
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(selfTree("None", L"abort_none"));
            sim.Step();
            setFlag(sim, e, true);
            sim.GetWorld().GetComponent<BehaviorTreeComponent>(e)->enabled = false;
            sim.Step();
            sim.GetWorld().GetComponent<BehaviorTreeComponent>(e)->enabled = true;
            sim.Step();
            setFlag(sim, e, false);
            sim.Step();
            ck.Check(sim.Comp(e)->activeNodeId == 3, "Abort None: 入った後に条件が偽になっても部分木は止まらない");
        }

        // LowerPriority: 左の兄弟の条件が真になったら右の実行中の子を Abort して左から実行し直す
        const auto lowerTree = [&](const char* mode, const wchar_t* name) {
            return RegisterTree(
                lib, name,
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 100), { BbCond("Flag", "IsSet", mode) }), Wait(2, 100) }, board));
        };
        {
            Sim sim;
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            const EntityID e = sim.AddTreeEntity(lowerTree("LowerPriority", L"abort_lower"));
            sim.Step();
            sim.Step();
            const int32_t before = sim.Comp(e)->activeNodeId; // 条件が偽なので右 (2)
            setFlag(sim, e, true);
            sim.Step();
            const int32_t after = sim.Comp(e)->activeNodeId;
            const std::vector<int32_t> abortedRight = trace;
            setFlag(sim, e, false);
            sim.Step();
            ck.Check(before == 2 && after == 1 && abortedRight == std::vector<int32_t>{ 2 } && sim.Comp(e)->activeNodeId == 1,
                     "Abort LowerPriority: 条件が真になった tick に右の兄弟が止まって左が走る (走り出した後に偽になっても Self ではない)");
        }
        {
            // 左の子が入っても Failure で終わる木で、条件が真のまま変わらないなら右を止めない (変化のときだけ働く)
            Sim sim;
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            const uint64_t guid = RegisterTree(
                lib, L"abort_lower_stable",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Node(1, "Selector"), { BbCond("Flag", "IsNotSet", "LowerPriority") }), Wait(2, 100) },
                     board));
            const EntityID e = sim.AddTreeEntity(guid);
            const std::vector<int32_t> got = Run(sim, e, 6);
            ck.Check(std::all_of(got.begin(), got.end(), [](int32_t s) { return s == kRunning; }) && sim.Comp(e)->activeNodeId == 2 && trace.empty(),
                     "Abort LowerPriority: 条件が真のまま変わらなければ (左の子は Failure で終わる) 右の子を止め続けない");
        }
        {
            Sim sim;
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            const EntityID e = sim.AddTreeEntity(lowerTree("Both", L"abort_both"));
            sim.Step();
            const int32_t before = sim.Comp(e)->activeNodeId;
            setFlag(sim, e, true);
            sim.Step();
            const int32_t up = sim.Comp(e)->activeNodeId;
            const std::vector<int32_t> afterUp = trace;
            setFlag(sim, e, false);
            sim.Step();
            ck.Check(before == 2 && up == 1 && afterUp == std::vector<int32_t>{ 2 } && sim.Comp(e)->activeNodeId == 2
                         && trace == std::vector<int32_t>{ 2, 1 },
                     "Abort Both: 条件が真で右を止めて左が走り、偽になれば左を止めて右へ戻る");
        }
        {
            // 親が Sequence の LowerPriority は Self として働く
            Sim sim;
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            const uint64_t guid = RegisterTree(
                lib, L"abort_lower_in_sequence",
                Tree(0, { Node(0, "Sequence", { 1 }), Decorated(Wait(1, 100), { BbCond("Flag", "IsSet", "LowerPriority") }) }, board));
            const EntityID e = sim.AddTreeEntity(guid);
            sim.Step();
            const int32_t status0 = sim.Status(e);
            ck.Check(status0 == kFailed, "(前提) 親が Sequence で条件が偽の間は子に入れず Failure");
            setFlag(sim, e, true);
            sim.Step(); // 根からやり直して入る
            const int32_t leaf = sim.Comp(e)->activeNodeId;
            setFlag(sim, e, false);
            sim.Step();
            ck.Check(leaf == 1 && sim.Status(e) == kFailed && trace == std::vector<int32_t>{ 1 },
                     "親が Selector でない LowerPriority は Self として働く (偽になったら子を Abort して Failure)");
        }
    }

    // ---- 10. 手数の上限 (Repeat 無限 + 即 Success) ----
    {
        Sim sim;
        const EntityID e = sim.AddTreeEntity(RegisterTree(
            lib, L"repeat_runaway", Tree(0, { Decorated(Node(0, "Sequence"), { Repeat(0) }) })));
        const uint64_t logFrom = logging::TotalWritten();
        sim.Step();
        const int32_t first = sim.Status(e);
        const bool partway = sim.bt.FindInstance(e) != nullptr && sim.bt.FindInstance(e)->nodes[1].active != 0;
        sim.Step();
        sim.Step();
        ck.Check(first == kRunning && partway && sim.Status(e) == kRunning && CountWarnings(logFrom, "step limit") == 1,
                 "Repeat 無限 + 即 Success の木は 1 tick 256 手で止まって次の tick に続き、警告は 1 回だけ");
        Sim finite;
        const EntityID f = finite.AddTreeEntity(RegisterTree(
            lib, L"repeat_300", Tree(0, { Decorated(Node(0, "Sequence"), { Repeat(300) }) })));
        finite.Step();
        const int32_t partial = finite.Status(f);
        finite.Step();
        ck.Check(partial == kRunning && finite.Status(f) == kSucceeded, "有限の Repeat も手数の上限をまたいで続きから終わる");
    }

    // ---- 11. Decorator の途中状態の保存 / 復元 ----
    {
        const uint64_t board = RegisterBoard(lib, L"deco_snap_bb", Board({ BbKey("Flag", "Bool", false), BbKey("Count", "Int", 0) }));
        const uint64_t guid = RegisterTree(
            lib, L"deco_snap",
            Tree(0, { Node(0, "Selector", { 1, 2, 5 }),
                      Decorated(Wait(1, 30), { BbCond("Flag", "IsSet", "Both") }),
                      Decorated(Node(2, "Sequence", { 3 }), { Cooldown(15), Repeat(2) }), Wait(3, 4, 1),
                      Decorated(Node(5, "Sequence", { 6 }), { Timeout(10) }), Wait(6, 40) },
                 board));
        const auto roundTrip = [&](const char* label, const std::function<bool(const BehaviorTreeSystem&, const BehaviorTreeAsset&, uint64_t)>& atCapture) {
            Scene scene;
            BehaviorTreeSystem first;
            std::vector<EntityID> agents;
            for (int i = 0; i < 3; ++i) {
                GameObject go = scene.CreateGameObjectTracked("Agent");
                go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ guid };
                agents.push_back(go.Id());
            }
            scene.GetWorld().ApplyStructuralChanges();
            SimRefs refs;
            refs.scene = &scene;
            refs.behaviorTree = &first;
            uint64_t tickRef = 0;
            refs.tickIndex = &tickRef;
            uint64_t tick = 1;
            const auto step = [&](BehaviorTreeSystem& bt) {
                bt.DeliverPending(tick);
                bt.Update(scene.GetWorld(), tick, nullptr);
                for (size_t i = 0; i < agents.size(); ++i) {
                    BtInstance* inst = const_cast<BtInstance*>(bt.FindInstance(agents[i]));
                    if (inst != nullptr && !inst->blackboard.empty()) {
                        inst->blackboard[0].i = ((tick + i * 5) / 17) % 2 == 0 ? 0 : 1; // Flag を周期的に反転する
                        inst->blackboard[0].isSet = 1;
                    }
                }
                scene.GetWorld().ApplyStructuralChanges();
                ++tick;
            };
            const BehaviorTreeAsset* asset = behaviortree::Library()->Get(guid);
            bool captured = false;
            for (int i = 0; i < 400 && !captured; ++i) {
                step(first);
                captured = atCapture(first, *asset, tick);
            }
            if (!captured) {
                ck.Check(false, label);
                return;
            }
            tickRef = tick;
            std::vector<std::byte> blob;
            const bool ok = CaptureSimSnapshot(refs, blob);
            const uint64_t startTick = tick;
            std::vector<uint64_t> continuous;
            for (int i = 0; i < 120; ++i) {
                step(first);
                continuous.push_back(HashWorld(scene.GetWorld(), refs.HashSources()));
            }
            BehaviorTreeSystem second;
            SimRefs refsSecond = refs;
            refsSecond.behaviorTree = &second;
            bool same = ok && RestoreSimSnapshot(refsSecond, blob.data(), blob.size());
            tick = startTick;
            for (int i = 0; i < 120 && same; ++i) {
                step(second);
                same = HashWorld(scene.GetWorld(), refsSecond.HashSources()) == continuous[static_cast<size_t>(i)];
            }
            ck.Check(same && continuous.front() != continuous.back(), label);
        };
        const auto slotOf = [](const BehaviorTreeAsset& asset, int32_t nodeId, size_t decoIndex) {
            return asset.nodes[static_cast<size_t>(asset.FindNode(nodeId))].decorators[decoIndex].slot;
        };
        roundTrip("Cooldown の計時の途中 (入れない間) で保存 → 復元 → 連続実行と毎 tick のハッシュが一致",
                  [&](const BehaviorTreeSystem& bt, const BehaviorTreeAsset& asset, uint64_t tick) {
                      const BtInstance& inst = bt.Instances().front();
                      return static_cast<uint64_t>(inst.nodes[static_cast<size_t>(slotOf(asset, 2, 0))].counter) > tick + 3
                             && inst.nodes[static_cast<size_t>(slotOf(asset, 2, 0))].active == 0;
                  });
        roundTrip("Timeout の計時の途中で保存 → 復元 → 連続実行と毎 tick のハッシュが一致",
                  [&](const BehaviorTreeSystem& bt, const BehaviorTreeAsset& asset, uint64_t tick) {
                      const BtInstance& inst = bt.Instances().front();
                      const BtNodeState& timeout = inst.nodes[static_cast<size_t>(slotOf(asset, 5, 0))];
                      return timeout.active != 0 && static_cast<uint64_t>(timeout.counter) > tick + 3;
                  });
        roundTrip("Repeat の周回の途中で保存 → 復元 → 連続実行と毎 tick のハッシュが一致",
                  [&](const BehaviorTreeSystem& bt, const BehaviorTreeAsset& asset, uint64_t) {
                      const BtInstance& inst = bt.Instances().front();
                      return inst.nodes[static_cast<size_t>(slotOf(asset, 2, 1))].counter == 1;
                  });
    }
    // ---- 12. Task 4 種 (M85c): アセット ----
    {
        const uint64_t kFilter = 0xABCD1234ull;
        const json full = Tree(0, { Node(0, "Sequence", { 1, 2, 3, 4, 5 }), MoveTo(1, "Goal", 1.5, false, true, kFilter),
                                    RotateTo(2, "Goal", 90.0, 2.0), SetBb(3, "Flag", "Constant", json{ { "boolValue", true } }),
                                    SetBb(4, "Home2", "Copy", json::object(), "Home"), ClearBb(5, "Home") });
        BehaviorTreeAsset a;
        BehaviorTreeAsset b;
        const bool loaded = BehaviorTreeLibrary::FromJson(full, a);
        ck.Check(loaded && BehaviorTreeLibrary::FromJson(BehaviorTreeLibrary::ToJson(a), b)
                     && BehaviorTreeLibrary::ToJson(a) == BehaviorTreeLibrary::ToJson(b),
                 "Task 4 種の JSON が往復で変わらない (キー・Guid・列挙を含む)");
        ck.Check(loaded && a.nodes[1].params[btmoveparam::kAcceptanceRadius].f == 1.5f && a.nodes[1].params[btmoveparam::kObserveTarget].i == 0
                     && a.nodes[1].params[btmoveparam::kFailOnStuck].i == 1 && a.nodes[1].params[btmoveparam::kNavFilter].u == kFilter
                     && a.nodes[1].keys.size() == 1 && a.nodes[1].keys[0] == "Goal" && a.nodes[3].params[btsetparam::kBoolValue].i == 1
                     && a.nodes[4].params[btsetparam::kSource].i == btsetparam::kCopy && a.nodes[4].keys[btnodekey::kSourceKey] == "Home",
                 "MoveTo / SetBlackboard のパラメータとキー名が読める");
        ck.Check(loaded && a.extraStateBytes == static_cast<int32_t>(sizeof(BtMoveToState) + sizeof(BtRotateToState))
                     && a.nodes[1].extraOffset == 0 && a.nodes[2].extraOffset == static_cast<int32_t>(sizeof(BtMoveToState)),
                 "種類別の追加状態はノードの並びに固定長の領域が割り当てられる (MoveTo 24 + RotateTo 4)");
        BehaviorTreeAsset defaults;
        ck.Check(BehaviorTreeLibrary::FromJson(Tree(0, { WithKeys(Node(0, "MoveTo"), json::object()) }), defaults)
                     && defaults.nodes[0].params[btmoveparam::kFailOnStuck].i == 0 && defaults.nodes[0].params[btmoveparam::kObserveTarget].i == 1
                     && defaults.nodes[0].params[btmoveparam::kNavFilter].u == 0 && defaults.nodes[0].keys[0].empty(),
                 "MoveTo の既定は failOnStuck = false・observeTarget = true・navFilter なし");
        ck.Check(!Loads(Tree(0, { WithKeys(Node(0, "MoveTo"), json{ { "target", 5 } }) })), "文字列でないキー名は読み込み失敗");
        ck.Check(!Loads(Tree(0, { WithKeys(Node(0, "MoveTo"), json{ { "target", std::string(kBbMaxNameBytes + 1, 'k') } }) })),
                 "長すぎるキー名は読み込み失敗");
        ck.Check(!Loads(Tree(0, { SetBb(0, "Flag", "Weird") })), "未知の source は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "MoveTo", {}, json{ { "navFilter", 5 } }) })), "文字列でない navFilter は読み込み失敗");
        ck.Check(!Loads(Tree(0, { Node(0, "MoveTo", {}, json{ { "navFilter", "not-hex" } }) })), "16 進でない navFilter は読み込み失敗");
        BehaviorTreeAsset clamped;
        ck.Check(BehaviorTreeLibrary::FromJson(Tree(0, { RotateTo(0, "Goal", 99999.0, 999.0) }), clamped)
                     && clamped.nodes[0].params[btrotateparam::kAngularSpeedDeg].f == 3600.0f
                     && clamped.nodes[0].params[btrotateparam::kToleranceDeg].f == 180.0f,
                 "RotateTo の範囲外の角度は範囲へ丸める");
    }

    // ---- 12. Task 4 種 (M85c): SetBlackboard / ClearBlackboard ----
    {
        const uint64_t board = RegisterBoard(
            lib, L"set_bb",
            Board({ BbKey("Flag", "Bool"), BbKey("Count", "Int", 5), BbKey("Speed", "Float"), BbKey("Home", "Vector"),
                    BbKey("Foe", "Entity"), BbKey("Spot", "Vector"), BbKey("Count2", "Int"), BbKey("Foe2", "Entity"),
                    BbKey("Home2", "Vector"), BbKey("Blank", "Vector") }));
        const uint64_t guid = RegisterTree(
            lib, L"set_all",
            Tree(0, { Node(0, "Sequence", { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }),
                      SetBb(1, "Flag", "Constant", json{ { "boolValue", true } }), SetBb(2, "Count", "Constant", json{ { "intValue", 42 } }),
                      SetBb(3, "Speed", "Constant", json{ { "floatValue", 2.5 } }),
                      SetBb(4, "Home", "Constant", json{ { "vectorX", 1.0 }, { "vectorY", 2.0 }, { "vectorZ", 3.0 } }),
                      SetBb(5, "Foe", "Self"), SetBb(6, "Spot", "Self"), SetBb(7, "Count2", "Copy", json::object(), "Count"),
                      SetBb(8, "Foe2", "Copy", json::object(), "Foe"), SetBb(9, "Home2", "Copy", json::object(), "Home"),
                      ClearBb(10, "Count") },
                 board));
        Sim sim;
        const EntityID e = sim.AddTreeEntity(guid, "Setter");
        sim.GetWorld().GetComponent<LocalTransform>(e)->position = { 4.0f, 5.0f, 6.0f };
        sim.Step();
        const BtInstance* inst = sim.bt.FindInstance(e);
        const auto value = [&](int key) -> const BbValue& { return inst->blackboard[static_cast<size_t>(key)]; };
        ck.Check(sim.Status(e) == kSucceeded && value(0).isSet == 1 && value(0).i == 1 && value(2).isSet == 1 && value(2).f == 2.5f
                     && value(3).v[0] == 1.0f && value(3).v[1] == 2.0f && value(3).v[2] == 3.0f,
                 "SetBlackboard: Bool / Float / Vector の定数を書いて 1 tick で Success");
        ck.Check(value(4).isSet == 1 && value(4).entity == e && value(5).isSet == 1 && value(5).v[0] == 4.0f && value(5).v[1] == 5.0f
                     && value(5).v[2] == 6.0f,
                 "SetBlackboard: Self は Entity = 自分、Vector = 自分のワールド位置");
        ck.Check(value(6).isSet == 1 && value(6).i == 42 && value(7).entity == e && value(8).v[2] == 3.0f,
                 "SetBlackboard: Copy は Int / Entity / Vector のコピー元の値を写す");
        ck.Check(value(1).isSet == 0 && value(1).i == 0, "ClearBlackboard: キーが未設定へ戻る (値も 0)");

        // 1 tick 動かして Failure になることを確かめ、そのときのブラックボードを返す
        const auto failsWith = [&](const char* name, const json& node, const char* what) {
            const uint64_t g = RegisterTree(lib, std::wstring(L"set_fail_").append(std::wstring(name, name + std::strlen(name))).c_str(),
                                            Tree(0, { node }, board));
            Sim s;
            const EntityID f = s.AddTreeEntity(g);
            s.Step();
            ck.Check(s.Status(f) == kFailed, what);
            const BtInstance* failed = s.bt.FindInstance(f);
            return failed != nullptr ? failed->blackboard : std::vector<BbValue>{};
        };
        const std::vector<BbValue> constEntity = failsWith("const_entity", SetBb(0, "Foe", "Constant"), "SetBlackboard: Entity へ定数は書けず Failure");
        ck.Check(constEntity.size() == 10 && constEntity[4].isSet == 0, "Failure のとき書き先は変わらない");
        failsWith("copy_unset", SetBb(0, "Home2", "Copy", json::object(), "Blank"), "SetBlackboard: 未設定のキーからのコピーは Failure");
        const std::vector<BbValue> mismatch =
            failsWith("copy_type", SetBb(0, "Count", "Copy", json::object(), "Speed"), "SetBlackboard: 型の違うキーからのコピーは Failure");
        ck.Check(mismatch.size() == 10 && mismatch[1].i == 5, "型違いで Failure のとき書き先の初期値が残る");
        failsWith("self_int", SetBb(0, "Count", "Self"), "SetBlackboard: Int へ Self は Failure");
        failsWith("no_key", SetBb(0, "Nope", "Constant"), "SetBlackboard: 無いキーは Failure");
        failsWith("clear_no_key", ClearBb(0, "Nope"), "ClearBlackboard: 無いキーは Failure");
        failsWith("empty_key", SetBb(0, "", "Constant"), "SetBlackboard: キー名が空 (未指定) は Failure");
    }

    // ---- 12. Task 4 種 (M85c): RotateTo ----
    {
        const uint64_t board = RegisterBoard(lib, L"rot_bb", Board({ BbKey("Goal", "Vector", json::array({ 10.0, 0.0, 0.0 })),
                                                                     BbKey("Look", "Entity"), BbKey("Flag", "Bool", true) }));
        const auto spawn = [](Sim& sim, uint64_t tree, bool withAgent, float agentSpeedDeg) {
            GameObject go = sim.scene.CreateGameObjectTracked("Rotor");
            go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ tree };
            if (withAgent) {
                go.AddComponent<NavMeshAgentComponent>()->angularSpeedDeg = agentSpeedDeg;
            }
            return go.Id();
        };
        // 目標は +X (= y 軸まわり +90 度)。成功までの tick 数と、そのときの向きを返す
        const auto rotate = [&](const wchar_t* name, double speed, bool withAgent, float agentSpeedDeg, int& ticks, float& yawDeg, bool& updateRotationWhileRunning) {
            const uint64_t guid = RegisterTree(lib, name, Tree(0, { RotateTo(0, "Goal", speed, 1.0) }, board));
            Sim sim;
            const EntityID e = spawn(sim, guid, withAgent, agentSpeedDeg);
            ticks = 0;
            updateRotationWhileRunning = true;
            for (int i = 1; i <= 200; ++i) {
                sim.Step();
                if (withAgent && i == 3) {
                    updateRotationWhileRunning = sim.GetWorld().GetComponent<NavMeshAgentComponent>(e)->updateRotation;
                }
                if (sim.Status(e) == kSucceeded) {
                    ticks = i;
                    break;
                }
            }
            yawDeg = YawDegOf(sim.GetWorld().GetComponent<LocalTransform>(e)->rotation);
            return e;
        };
        int ticks = 0;
        float yaw = 0.0f;
        bool running = true;
        rotate(L"rot_90", 90.0, false, 0.0f, ticks, yaw, running);
        ck.Check(ticks >= 59 && ticks <= 61 && std::fabs(yaw - 90.0f) <= 1.01f,
                 "RotateTo: 90 度/秒で 90 度回り、許容 1 度以内で約 60 tick に Success");
        rotate(L"rot_agent", 0.0, true, 180.0f, ticks, yaw, running);
        ck.Check(ticks >= 29 && ticks <= 31 && std::fabs(yaw - 90.0f) <= 1.01f && !running,
                 "RotateTo: angularSpeedDeg = 0 は Agent の値 (180) を使い、実行中の updateRotation は false");
        rotate(L"rot_default", 0.0, false, 0.0f, ticks, yaw, running);
        ck.Check(ticks >= 14 && ticks <= 16, "RotateTo: angularSpeedDeg = 0 で Agent も無ければ 360 度/秒");
        rotate(L"rot_zero_agent", 0.0, true, 0.0f, ticks, yaw, running);
        ck.Check(ticks >= 14 && ticks <= 16, "RotateTo: Agent の角速度が 0 のときも 360 度/秒 (終わらない RotateTo を作らない)");

        // 終了で updateRotation が元の値へ戻る (true だった Agent も false だった Agent も)
        for (const bool original : { true, false }) {
            const uint64_t guid = RegisterTree(lib, L"rot_restore", Tree(0, { RotateTo(0, "Goal", 360.0, 1.0) }, board));
            Sim sim;
            const EntityID e = spawn(sim, guid, true, 360.0f);
            sim.GetWorld().GetComponent<NavMeshAgentComponent>(e)->updateRotation = original;
            for (int i = 0; i < 20 && sim.Status(e) != kSucceeded; ++i) {
                sim.Step();
            }
            ck.Check(sim.Status(e) == kSucceeded && sim.GetWorld().GetComponent<NavMeshAgentComponent>(e)->updateRotation == original,
                     original ? "RotateTo: 終了で updateRotation (true) が戻る" : "RotateTo: 終了で updateRotation (false) が戻る");
        }

        // Abort でも戻り、向きはそこで止まる
        {
            std::vector<int32_t> trace;
            const uint64_t guid = RegisterTree(
                lib, L"rot_abort",
                Tree(0, { Node(0, "Selector", { 1, 3 }), Decorated(Node(1, "Sequence", { 2 }), { BbCond("Flag", "IsSet", "Self") }),
                          RotateTo(2, "Goal", 10.0, 1.0), Wait(3, 1000) },
                     board));
            Sim sim;
            sim.bt.SetAbortTrace(&trace);
            const EntityID e = spawn(sim, guid, true, 360.0f);
            for (int i = 0; i < 6; ++i) {
                sim.Step();
            }
            const bool heldDuring = !sim.GetWorld().GetComponent<NavMeshAgentComponent>(e)->updateRotation;
            const float yawAtAbort = YawDegOf(sim.GetWorld().GetComponent<LocalTransform>(e)->rotation);
            sim.Mutable(e)->blackboard[2].i = 0;
            sim.Step();
            const float yawAfter = YawDegOf(sim.GetWorld().GetComponent<LocalTransform>(e)->rotation);
            for (int i = 0; i < 10; ++i) {
                sim.Step();
            }
            ck.Check(heldDuring && sim.GetWorld().GetComponent<NavMeshAgentComponent>(e)->updateRotation && trace == std::vector<int32_t>{ 2, 1 }
                         && std::fabs(YawDegOf(sim.GetWorld().GetComponent<LocalTransform>(e)->rotation) - yawAfter) < 0.001f && yawAtAbort > 0.5f
                         && yawAfter < 5.0f,
                     "RotateTo: Abort で updateRotation が戻り (後始末は子が先)、向きはそこで止まる");
        }

        // 目標が Entity (-X 側 = -90 度)。目標が未設定の間は Failure、書いてから回る
        {
            const uint64_t guid = RegisterTree(lib, L"rot_entity", Tree(0, { RotateTo(0, "Look", 360.0, 1.0) }, board));
            Sim sim;
            const EntityID e = spawn(sim, guid, false, 0.0f);
            sim.Step();
            const bool failedUnset = sim.Status(e) == kFailed;
            GameObject target = sim.scene.CreateGameObjectTracked("Look");
            target.SetLocalPosition(-10.0f, 0.0f, 0.0f);
            sim.Step(); // BB はこの tick に作られた後
            sim.Mutable(e)->blackboard[1] = BbValue{ 1, 0, 0.0f, { 0.0f, 0.0f, 0.0f }, target.Id() };
            for (int i = 0; i < 30 && sim.Status(e) != kSucceeded; ++i) {
                sim.Step();
            }
            ck.Check(failedUnset && sim.Status(e) == kSucceeded
                         && std::fabs(YawDegOf(sim.GetWorld().GetComponent<LocalTransform>(e)->rotation) + 90.0f) <= 1.01f,
                     "RotateTo: 目標 (Entity) が未設定なら Failure、書くとその方向へ回る");
        }
    }

    // ---- 12. Task 4 種 (M85c): MoveTo (NavMesh の固定ジオメトリ) ----
    bool fieldBaked = false;
    {
        constexpr uint64_t kFilterA = 0x1111AAAAull; // MoveTo が実行中だけ差し込む navFilter (未登録の GUID でも Nav は Surface のまま歩く)
        constexpr uint64_t kFilterOriginal = 0x2222BBBBull;
        const uint64_t board = RegisterBoard(
            lib, L"move_bb",
            Board({ BbKey("Goal", "Vector", json::array({ 6.0, 0.0, 0.0 })), BbKey("Far", "Vector", json::array({ 40.0, 0.0, 0.0 })),
                    BbKey("Quarry", "Entity"), BbKey("Alarm", "Bool", true), BbKey("Alert", "Bool", false), BbKey("AlertOn", "Bool", true),
                    BbKey("Block", "Bool", false), BbKey("Home", "Vector", json::array({ -6.0, 0.0, 3.0 })), BbKey("Tmp", "Vector") }));
        {
            Scene probe; // 先にナビメッシュを 1 回焼く (以降の場面は同じものを参照する)
            BuildField(probe, fieldBaked);
        }
        ck.Check(fieldBaked, "(前提) 24 x 24 m の床のナビメッシュを焼ける");

        // 1 つの場面を作る: 床 + Surface + Agent。Agent は (-6, 0, 0) に立つ
        struct Field {
            Scene scene;
            EntityID walker;
        };
        const auto makeField = [&](uint64_t tree, std::unique_ptr<Field>& out) {
            out = std::make_unique<Field>();
            BuildField(out->scene, fieldBaked);
            out->walker = AddWalker(out->scene, tree, -6.0f, 0.0f);
            out->scene.GetWorld().ApplyStructuralChanges();
        };
        const auto runUntil = [](NavSim& sim, EntityID e, int32_t status, int maxTicks) {
            for (int i = 0; i < maxTicks; ++i) {
                sim.Step();
                if (sim.Comp(e)->status == status) {
                    return sim.tick - 1;
                }
            }
            return static_cast<uint64_t>(0);
        };

        // ---- Arrived で Success / 同じ地点へ 2 回 ----
        uint64_t firstArrival = 0;
        {
            const uint64_t guid = RegisterTree(lib, L"move_arrive", Tree(0, { MoveTo(0, "Goal", 0.0, false) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            firstArrival = runUntil(sim, f->walker, kSucceeded, 900);
            const NavMeshAgentComponent* agent = sim.Agent(f->walker);
            ck.Check(firstArrival != 0 && !agent->hasDestination && HorizontalGap(*sim.Pos(f->walker), 6.0f, 0.0f) < 0.4f,
                     "MoveTo: Agent が Arrived になると Success、目的地は倒れて Goal の近くにいる");
            // 到着済みの Agent へ同じ地点をもう 1 回 (根のやり直し = Nav が hasDestination の偽を 1 tick 見る場合)
            const uint64_t second = runUntil(sim, f->walker, kSucceeded, 300);
            MYE_LOG_INFO("  [move] same-point twice (root restart): first Success at tick %llu, second after %llu ticks",
                         static_cast<unsigned long long>(firstArrival), static_cast<unsigned long long>(second - firstArrival));
            ck.Check(second != 0 && second - firstArrival <= 60, "MoveTo: 到着済みの Agent への同じ地点の 2 回目も再探索されて Success で終わる");
        }
        {
            // 同じ tick の中で続けて同じ地点 (Nav は hasDestination の偽を見ない)
            const uint64_t guid = RegisterTree(lib, L"move_twice",
                                               Tree(0, { Node(0, "Sequence", { 1, 2 }), MoveTo(1, "Goal", 0.0, false), MoveTo(2, "Goal", 0.0, false) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            const uint64_t both = runUntil(sim, f->walker, kSucceeded, 900);
            MYE_LOG_INFO("  [move] same-point twice (same tick): both MoveTo done at tick %llu (single MoveTo: %llu)",
                         static_cast<unsigned long long>(both), static_cast<unsigned long long>(firstArrival));
            ck.Check(both != 0 && both >= firstArrival && both - firstArrival <= 5,
                     "MoveTo: 同じ tick の中で続けて同じ地点へ出しても 2 つ目が Success で終わる");
        }

        // ---- 届かない目的地 (ナビメッシュの外) ----
        {
            const uint64_t guid = RegisterTree(lib, L"move_nopath", Tree(0, { MoveTo(0, "Far", 0.5, false) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            const uint64_t failedAt = runUntil(sim, f->walker, kFailed, 60);
            ck.Check(failedAt != 0 && !sim.Agent(f->walker)->hasDestination, "MoveTo: ナビメッシュの外の目的地 (NoPath) は Failure で、目的地を倒す");
        }

        // ---- 目標が無い / Agent が無い ----
        {
            const uint64_t guid = RegisterTree(lib, L"move_notarget", Tree(0, { MoveTo(0, "Quarry", 0.5, false) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            sim.Step();
            ck.Check(sim.Comp(f->walker)->status == kFailed && !sim.Agent(f->walker)->hasDestination,
                     "MoveTo: 目標 (Entity) が未設定なら Failure、Agent には何も書かない");
            const uint64_t noAgent = RegisterTree(lib, L"move_noagent", Tree(0, { MoveTo(0, "Goal", 0.5, false) }, board));
            Sim bare;
            const EntityID e = bare.AddTreeEntity(noAgent);
            bare.Step();
            ck.Check(bare.Status(e) == kFailed, "MoveTo: NavMeshAgent が無ければ Failure");
        }

        // ---- 近くにいれば歩かず Success / acceptanceRadius で途中でも Success ----
        {
            const uint64_t near = RegisterTree(lib, L"move_near", Tree(0, { MoveTo(0, "Goal", 20.0, false) }, board));
            std::unique_ptr<Field> f;
            makeField(near, f);
            NavSim sim(f->scene);
            sim.Step();
            ck.Check(sim.Comp(f->walker)->status == kSucceeded && !sim.Agent(f->walker)->hasDestination,
                     "MoveTo: 最初から acceptanceRadius の内側なら Agent に何も書かずに Success");

            const uint64_t early = RegisterTree(lib, L"move_early", Tree(0, { MoveTo(0, "Goal", 1.5, false) }, board));
            std::unique_ptr<Field> g;
            makeField(early, g);
            NavSim walk(g->scene);
            const uint64_t at = runUntil(walk, g->walker, kSucceeded, 900);
            const float gap = HorizontalGap(*walk.Pos(g->walker), 6.0f, 0.0f);
            ck.Check(at != 0 && at < firstArrival && gap > 1.3f && gap <= 1.51f && !walk.Agent(g->walker)->hasDestination,
                     "MoveTo: acceptanceRadius (1.5 m) に入った tick に Success、Agent は止まる (Arrived を待たない)");
        }

        // ---- navFilter の差し替えと戻し ----
        {
            const uint64_t guid = RegisterTree(lib, L"move_filter", Tree(0, { MoveTo(0, "Goal", 0.0, false, false, kFilterA) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            f->scene.GetWorld().GetComponent<NavMeshAgentComponent>(f->walker)->navFilter = AssetID{ kFilterOriginal };
            NavSim sim(f->scene);
            for (int i = 0; i < 30; ++i) {
                sim.Step();
            }
            const bool swapped = sim.Agent(f->walker)->navFilter.value == kFilterA;
            const uint64_t done = runUntil(sim, f->walker, kSucceeded, 900);
            ck.Check(swapped && done != 0 && sim.Agent(f->walker)->navFilter.value == kFilterOriginal,
                     "MoveTo: navFilter は実行中だけ差し替わり、Success で元の値へ戻る");
        }

        // ---- Abort で Agent が止まる (後始末は子が先、navFilter も戻る) ----
        {
            std::vector<int32_t> trace;
            const uint64_t guid = RegisterTree(
                lib, L"move_abort",
                Tree(0, { Node(0, "Selector", { 1, 3 }), Decorated(Node(1, "Sequence", { 2 }), { BbCond("Alarm", "IsSet", "Self") }),
                          MoveTo(2, "Goal", 0.0, false, false, kFilterA), Wait(3, 5000) },
                     board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            f->scene.GetWorld().GetComponent<NavMeshAgentComponent>(f->walker)->navFilter = AssetID{ kFilterOriginal };
            NavSim sim(f->scene);
            sim.bt->SetAbortTrace(&trace);
            for (int i = 0; i < 40; ++i) {
                sim.Step();
            }
            const NavMeshAgentComponent* agent = sim.Agent(f->walker);
            const bool walking = agent->hasDestination && agent->status == navagentstatus::kMoving && sim.Pos(f->walker)->position.x > -5.9f;
            sim.Mutable(f->walker)->blackboard[3].i = 0; // Alarm を倒す = Self の Abort
            sim.Step();
            const bool stoppedAtOnce = !agent->hasDestination && agent->navFilter.value == kFilterOriginal;
            for (int i = 0; i < 60; ++i) { // 加速度で減速しきるまで
                sim.Step();
            }
            const float xAfter = sim.Pos(f->walker)->position.x;
            for (int i = 0; i < 60; ++i) {
                sim.Step();
            }
            ck.Check(walking && stoppedAtOnce && trace == std::vector<int32_t>{ 2, 1 } && agent->status == navagentstatus::kIdle
                         && std::fabs(sim.Pos(f->walker)->position.x - xAfter) < 0.01f && sim.Comp(f->walker)->activeNodeId == 3,
                     "MoveTo: Abort で目的地を倒して Agent が止まり (後始末は子が先)、navFilter も戻る");
        }

        // ---- ライブ表示の読み口 (M85j): MoveTo のデバッグ線と、Abort の記録 ----
        {
            const uint64_t guid = RegisterTree(
                lib, L"move_live",
                Tree(0, { Node(0, "Selector", { 1, 3 }), Decorated(Node(1, "Sequence", { 2 }), { BbCond("Alarm", "IsSet", "Self") }),
                          MoveTo(2, "Goal", 0.0, false, false, kFilterA), Wait(3, 5000) },
                     board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            for (int i = 0; i < 40; ++i) {
                sim.Step();
            }
            std::vector<DebugLineCmd> off;
            sim.bt->AppendDebugLines(sim.GetWorld(), off); // drawDebug が偽の間は何も積まない
            sim.Comp(f->walker)->drawDebug = true;
            const uint64_t hashBefore = sim.bt->StateHash();
            std::vector<DebugLineCmd> on;
            sim.bt->AppendDebugLines(sim.GetWorld(), on);
            const NavMeshAgentComponent* agent = sim.Agent(f->walker);
            bool toDestination = false;
            for (const DebugLineCmd& line : on) {
                toDestination = toDestination
                                || (std::fabs(line.bx - agent->destination.x) < 1e-4f && std::fabs(line.bz - agent->destination.z) < 1e-4f
                                    && std::fabs(line.ax - sim.Pos(f->walker)->position.x) < 1e-4f);
            }
            ck.Check(off.empty() && !on.empty() && toDestination && sim.bt->StateHash() == hashBefore,
                     "デバッグ線: drawDebug が真のときだけ、歩いている MoveTo の目的地へ自分から線を引く (sim のハッシュは変わらない)");
            sim.Mutable(f->walker)->blackboard[3].i = 0; // Alarm を倒す = Self の Abort
            sim.Step();
            const BtAbortRecord& record = sim.bt->FindInstance(f->walker)->lastAbort;
            std::vector<DebugLineCmd> after;
            sim.bt->AppendDebugLines(sim.GetWorld(), after);
            ck.Check(record.sourceId == 1 && record.targetId == 2 && record.tick == sim.tick - 1 && after.empty(),
                     "Abort の記録: Self の Abort が Decorator の付いたノード (1) と止めた MoveTo (2) を残し、止まった後は線も消える");
        }

        // ---- 実行中にコンポーネントが外れても戻す ----
        {
            const uint64_t guid = RegisterTree(lib, L"move_detach", Tree(0, { MoveTo(0, "Goal", 0.0, false, false, kFilterA) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            f->scene.GetWorld().GetComponent<NavMeshAgentComponent>(f->walker)->navFilter = AssetID{ kFilterOriginal };
            NavSim sim(f->scene);
            for (int i = 0; i < 20; ++i) {
                sim.Step();
            }
            const bool running = sim.Agent(f->walker)->hasDestination && sim.Agent(f->walker)->navFilter.value == kFilterA;
            sim.GetWorld().RemoveComponent<BehaviorTreeComponent>(f->walker);
            sim.GetWorld().ApplyStructuralChanges();
            sim.Step();
            ck.Check(running && !sim.Agent(f->walker)->hasDestination && sim.Agent(f->walker)->navFilter.value == kFilterOriginal
                         && sim.bt->InstanceCount() == 0,
                     "MoveTo: 実行中に BehaviorTree コンポーネントが外れると、Agent の目的地と navFilter を戻してから表を捨てる");
        }

        // ---- LowerPriority: 偽 -> 真で MoveTo を Abort して止める / 真のままなら Abort しない ----
        {
            std::vector<int32_t> trace;
            const uint64_t guid = RegisterTree(
                lib, L"move_lower",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Node(1, "Sequence", { 4 }), { BbCond("Alert", "IsSet", "LowerPriority") }),
                          Node(2, "Sequence", { 3 }), MoveTo(3, "Goal", 0.0, false), Wait(4, 5000) },
                     board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            sim.bt->SetAbortTrace(&trace);
            for (int i = 0; i < 40; ++i) {
                sim.Step();
            }
            const bool walking = sim.Agent(f->walker)->hasDestination && sim.Comp(f->walker)->activeNodeId == 3 && trace.empty();
            sim.Mutable(f->walker)->blackboard[4].i = 1; // Alert が偽 -> 真
            sim.Step();
            ck.Check(walking && !sim.Agent(f->walker)->hasDestination && trace == std::vector<int32_t>{ 3, 2 } && sim.Comp(f->walker)->activeNodeId == 4,
                     "LowerPriority: 条件が偽から真へ変わると、優先度の低い MoveTo を Abort して (子が先に) 止め、高い側へ移る");
        }
        {
            std::vector<int32_t> trace;
            const uint64_t guid = RegisterTree(
                lib, L"move_lower_steady",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Node(1, "Sequence", { 4 }), { BbCond("AlertOn", "IsSet", "LowerPriority") }),
                          Node(2, "Sequence", { 3 }), MoveTo(3, "Goal", 0.0, false), Node(4, "Selector") },
                     board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            sim.bt->SetAbortTrace(&trace);
            const uint64_t done = runUntil(sim, f->walker, kSucceeded, 900);
            ck.Check(done != 0 && trace.empty(),
                     "LowerPriority: 条件が真のままで高い側がすぐ失敗する木では、MoveTo を Abort せず最後まで歩く (変化した tick だけ働く)");
        }

        // ---- Stuck: ベイク後に置いた壁で経路が塞がれる (経路は完全なまま前へ進めない) ----
        const auto stuckRun = [&](bool failOnStuck, bool& sawStuck, bool& failed, bool& destinationAtEnd, int32_t& statusAtEnd, int32_t& activeAtEnd) {
            const uint64_t guid = RegisterTree(
                lib, failOnStuck ? L"move_stuck_fail" : L"move_stuck_wait",
                Tree(0, { Node(0, "Selector", { 1, 3 }), Node(1, "Sequence", { 2 }), MoveTo(2, "Goal", 0.0, false, failOnStuck), Wait(3, 5000) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            GameObject wall = f->scene.CreateGameObjectTracked("LateWall");
            wall.SetLocalPosition(0.0f, 1.0f, 0.0f);
            auto* collider = wall.AddComponent<ColliderComponent>();
            collider->shape = collidershape::kBox;
            collider->halfExtents = { 0.5f, 1.0f, 13.0f };
            f->scene.GetWorld().ApplyStructuralChanges();
            NavSim sim(f->scene);
            sawStuck = false;
            failed = false;
            for (int i = 0; i < 700; ++i) {
                sim.Step();
                sawStuck = sawStuck || sim.Agent(f->walker)->status == navagentstatus::kStuck;
                failed = failed || sim.Comp(f->walker)->activeNodeId == 3;
            }
            destinationAtEnd = sim.Agent(f->walker)->hasDestination;
            statusAtEnd = sim.Agent(f->walker)->status;
            activeAtEnd = sim.Comp(f->walker)->activeNodeId;
        };
        {
            bool sawStuck = false;
            bool failed = false;
            bool destination = false;
            int32_t status = 0;
            int32_t active = 0;
            stuckRun(false, sawStuck, failed, destination, status, active);
            MYE_LOG_INFO("  [move] failOnStuck = false: sawStuck %d, failed %d, hasDestination %d, nav status %d, active node %d",
                         sawStuck ? 1 : 0, failed ? 1 : 0, destination ? 1 : 0, status, active);
            ck.Check(sawStuck && !failed && destination && status == navagentstatus::kStuck && active == 2,
                     "MoveTo: failOnStuck = false (既定) は Stuck の間も Running のまま押し続ける");
            stuckRun(true, sawStuck, failed, destination, status, active);
            MYE_LOG_INFO("  [move] failOnStuck = true: sawStuck %d, failed %d, hasDestination %d, nav status %d, active node %d",
                         sawStuck ? 1 : 0, failed ? 1 : 0, destination ? 1 : 0, status, active);
            ck.Check(sawStuck && failed && !destination && status == navagentstatus::kIdle && active == 3,
                     "MoveTo: failOnStuck = true は Stuck になった tick に Failure、目的地を倒して Agent が止まり、次の枝へ移る");
        }

        // ---- SearchArea の Stuck (MoveTo と同じ壁): failOnStuck の false は Running のまま、true は Failure で目的地を倒す ----
        for (int failOnStuck = 0; failOnStuck < 2; ++failOnStuck) {
            const uint64_t guid = RegisterTree(
                lib, failOnStuck != 0 ? L"search_stuck_fail" : L"search_stuck_wait",
                Tree(0, { Node(0, "Selector", { 1, 3 }), Node(1, "Sequence", { 4, 2 }), Wait(4, 5), // 最初の数 tick は Surface が未読み込みで Failure になるので待つ
                          Search(2, "Goal", nullptr, false, 3.0, 2, failOnStuck != 0), Wait(3, 5000) },
                     board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            GameObject wall = f->scene.CreateGameObjectTracked("LateWall");
            wall.SetLocalPosition(0.0f, 1.0f, 0.0f);
            auto* collider = wall.AddComponent<ColliderComponent>();
            collider->shape = collidershape::kBox;
            collider->halfExtents = { 0.5f, 1.0f, 13.0f };
            f->scene.GetWorld().ApplyStructuralChanges();
            NavSim sim(f->scene);
            bool sawStuck = false;
            bool failed = false;
            for (int i = 0; i < 700; ++i) {
                sim.Step();
                sawStuck = sawStuck || sim.Agent(f->walker)->status == navagentstatus::kStuck;
                failed = failed || sim.Comp(f->walker)->activeNodeId == 3;
            }
            const bool destination = sim.Agent(f->walker)->hasDestination;
            const int32_t active = sim.Comp(f->walker)->activeNodeId;
            MYE_LOG_INFO("  [search] stuck %d: sawStuck %d failed %d dest %d active %d nav %d", failOnStuck, sawStuck, failed, destination, active, sim.Agent(f->walker)->status);
            if (failOnStuck == 0) {
                ck.Check(sawStuck && !failed && destination && active == 2, "SearchArea: failOnStuck = false (既定) は Stuck の間も Running のまま");
            } else {
                ck.Check(sawStuck && failed && !destination && active == 3,
                         "SearchArea: failOnStuck = true は Stuck になった tick に Failure、目的地を倒して次の枝へ移る");
            }
        }

        // ---- observeTarget: 動く Entity を追う / 追わない ----
        {
            const auto chase = [&](bool observe, float& destZ, float& runnerZ) {
                const uint64_t guid = RegisterTree(lib, observe ? L"move_observe" : L"move_fixed",
                                                   Tree(0, { MoveTo(0, "Quarry", 0.0, observe) }, board));
                std::unique_ptr<Field> f;
                makeField(guid, f);
                GameObject runner = f->scene.CreateGameObjectTracked("Runner");
                runner.SetLocalPosition(6.0f, 0.0f, -6.0f);
                f->scene.GetWorld().ApplyStructuralChanges();
                NavSim sim(f->scene);
                sim.Step(); // 表を作る (Quarry は未設定で Failure)
                sim.Mutable(f->walker)->blackboard[2] = BbValue{ 1, 0, 0.0f, { 0.0f, 0.0f, 0.0f }, runner.Id() };
                for (int i = 0; i < 150; ++i) {
                    sim.Pos(runner.Id())->position.z = -6.0f + 0.08f * static_cast<float>(i);
                    sim.Step();
                }
                destZ = sim.Agent(f->walker)->destination.z;
                runnerZ = sim.Pos(runner.Id())->position.z;
            };
            float destZ = 0.0f;
            float runnerZ = 0.0f;
            chase(true, destZ, runnerZ);
            MYE_LOG_INFO("  [move] observeTarget = true: destination z %.2f, runner z %.2f", destZ, runnerZ);
            ck.Check(std::fabs(destZ - runnerZ) < 0.7f && runnerZ > 3.0f, "MoveTo: observeTarget は動く Entity の位置へ目的地を書き直して追う");
            chase(false, destZ, runnerZ);
            MYE_LOG_INFO("  [move] observeTarget = false: destination z %.2f, runner z %.2f", destZ, runnerZ);
            ck.Check(destZ < -5.0f && runnerZ - destZ > 6.0f, "MoveTo: observeTarget = false は最初の位置へ向かい続け、書き直さない");
        }

        // 木 treeGuid の 1 体を保存 → 復元 → 連続実行と比べる (Nav 節と BT 節の両方)。atCapture が true を返した tick で撮る
        const auto roundTrip = [&](const char* label, uint64_t treeGuid, bool freshBt, const std::function<bool(const BtInstance&, uint64_t)>& atCapture) {
            std::unique_ptr<Field> f;
            makeField(treeGuid, f);
            NavSim sim(f->scene);
            SimRefs refs;
            refs.scene = &f->scene;
            refs.nav = &sim.nav;
            refs.behaviorTree = sim.bt;
            uint64_t tickRef = 0;
            refs.tickIndex = &tickRef;
            bool captured = false;
            for (int i = 0; i < 1200 && !captured; ++i) {
                sim.Step();
                const BtInstance* inst = sim.bt->FindInstance(f->walker);
                captured = inst != nullptr && atCapture(*inst, sim.tick);
            }
            if (!captured) {
                ck.Check(false, label);
                return;
            }
            tickRef = sim.tick;
            std::vector<std::byte> blob;
            const bool ok = CaptureSimSnapshot(refs, blob);
            const BtInstance* at = sim.bt->FindInstance(f->walker);
            const bool extraNonZero = at != nullptr && std::any_of(at->extra.begin(), at->extra.end(), [](uint8_t b) { return b != 0; });
            const uint64_t startTick = sim.tick;
            constexpr int kAhead = 400;
            std::vector<uint64_t> continuous;
            for (int i = 0; i < kAhead; ++i) {
                sim.Step();
                continuous.push_back(HashWorld(f->scene.GetWorld(), refs.HashSources()));
            }
            BehaviorTreeSystem fresh; // 新しいシステム (空の表) へ復元する場合
            if (freshBt) {
                sim.bt = &fresh;
                refs.behaviorTree = &fresh;
            }
            bool same = ok && RestoreSimSnapshot(refs, blob.data(), blob.size());
            sim.tick = startTick;
            for (int i = 0; i < kAhead && same; ++i) {
                sim.Step();
                same = HashWorld(f->scene.GetWorld(), refs.HashSources()) == continuous[static_cast<size_t>(i)];
            }
            ck.Check(same && extraNonZero && continuous.front() != continuous.back(), label);
        };
        // ---- 保存 -> 復元 -> 連続実行 (Nav 節と BT 節の両方) ----
        {
            const uint64_t guid = RegisterTree(
                lib, L"move_snap",
                Tree(0, { Node(0, "Sequence", { 1, 2, 3, 4, 5 }), MoveTo(1, "Goal", 0.2, true, false, kFilterA),
                          RotateTo(2, "Home", 45.0, 1.0), SetBb(3, "Tmp", "Copy", json::object(), "Goal"),
                          SetBb(4, "Goal", "Copy", json::object(), "Home"), SetBb(5, "Home", "Copy", json::object(), "Tmp") },
                     board));
            roundTrip("MoveTo の途中 (navFilter 差し替え中) で保存 → 復元 → 連続実行と毎 tick のハッシュが一致 (Nav 節と BT 節)", guid, false,
                      [](const BtInstance& inst, uint64_t tick) { return tick > 60 && inst.nodes[1].active != 0; });
            roundTrip("MoveTo の途中で保存 → 新しい BehaviorTreeSystem へ復元 → 連続実行と毎 tick のハッシュが一致", guid, true,
                      [](const BtInstance& inst, uint64_t tick) { return tick > 90 && inst.nodes[1].active != 0; });
            roundTrip("RotateTo の途中 (updateRotation を預かっている間) で保存 → 復元 → 連続実行と毎 tick のハッシュが一致", guid, false,
                      [](const BtInstance& inst, uint64_t) { return inst.nodes[2].active != 0; });
            roundTrip("RotateTo の途中で保存 → 新しい BehaviorTreeSystem へ復元 → 連続実行と毎 tick のハッシュが一致", guid, true,
                      [](const BtInstance& inst, uint64_t) { return inst.nodes[2].active != 0; });
        }

        // ---- 追加状態の長さが木と合わない保存は、その木を初期状態からやり直す ----
        {
            const uint64_t guid = RegisterTree(lib, L"move_mismatch", Tree(0, { MoveTo(0, "Goal", 0.0, false) }, board));
            std::unique_ptr<Field> f;
            makeField(guid, f);
            NavSim sim(f->scene);
            for (int i = 0; i < 20; ++i) {
                sim.Step();
            }
            std::vector<std::byte> bytes;
            ByteWriter writer(bytes);
            sim.bt->SaveSnapshot(writer);
            BtSnapshot parsed;
            ByteReader reader(bytes.data(), bytes.size());
            const bool read = BehaviorTreeSystem::ReadSnapshot(reader, parsed) && parsed.instances.size() == 1;
            if (read) {
                parsed.instances[0].extra.resize(parsed.instances[0].extra.size() + 4, 0);
            }
            const uint64_t logFrom = logging::TotalWritten();
            sim.bt->ApplySnapshot(f->scene.GetWorld(), std::move(parsed));
            const BtInstance* restored = sim.bt->FindInstance(f->walker);
            ck.Check(read && restored != nullptr && restored->extra.size() == sizeof(BtMoveToState) && restored->nodes[0].active == 0
                         && CountWarnings(logFrom, "does not fit") == 1,
                     "追加状態の長さが木と合わない保存は警告して初期状態からやり直す");
        }

        // ---- 14. Patrol (M85g) ----
        {
            const uint64_t patrolBoard =
                RegisterBoard(lib, L"patrol_bb", Board({ BbKey("Route", "Entity"), BbKey("Alarm", "Bool", true) }));
            constexpr int kPatrolNode = 2; // 下の木の Patrol ノードの id
            struct PatrolPoint {
                float x;
                float z;
                int wait;
            };
            const auto patrolNode = [](int id, bool failOnStuck = false) {
                return WithKeys(Node(id, "Patrol", {}, json{ { "acceptanceRadius", 0.5 }, { "failOnStuck", failOnStuck } }), json{ { "route", "Route" } });
            };
            // Surface が読み込まれる前の数 tick (Agent が Inactive) を避けるため、先に 5 tick 待ってから Patrol
            const uint64_t plainTree =
                RegisterTree(lib, L"patrol_plain", Tree(0, { Node(0, "Sequence", { 1, 2 }), Wait(1, 5), patrolNode(2) }, patrolBoard));
            const auto makePatrolField = [&](uint64_t treeGuid, int32_t mode, const std::vector<PatrolPoint>& points, std::unique_ptr<Field>& f,
                                             EntityID& route) {
                makeField(treeGuid, f);
                GameObject routeObject = f->scene.CreateGameObjectTracked("Route");
                auto* component = routeObject.AddComponent<PatrolRouteComponent>();
                component->mode = mode;
                component->pointCount = static_cast<int32_t>(points.size());
                for (size_t i = 0; i < points.size(); ++i) {
                    component->points[i] = { points[i].x, 0.0f, points[i].z };
                    component->waitTicks[i] = points[i].wait;
                }
                f->scene.GetWorld().ApplyStructuralChanges();
                route = routeObject.Id();
            };
            const auto setRoute = [](NavSim& sim, EntityID walker, EntityID route) {
                sim.Mutable(walker)->blackboard[0] = BbValue{ 1, 0, 0.0f, { 0.0f, 0.0f, 0.0f }, route };
            };
            const auto patrolStateOf = [](NavSim& sim, EntityID e, BtPatrolState& out) {
                const BtInstance* inst = sim.bt->FindInstance(e);
                if (inst == nullptr || !inst->tree) {
                    return false;
                }
                const int index = inst->tree->FindNode(kPatrolNode);
                if (index < 0 || inst->nodes[static_cast<size_t>(index)].active == 0) {
                    return false;
                }
                std::memcpy(&out, inst->extra.data() + inst->tree->nodes[static_cast<size_t>(index)].extraOffset, sizeof(out));
                return true;
            };
            // 向かっている点が変わるたびに記録する。waitSeen = その点で待ちに入ったと観測した tick (待ち 0 の点は 0)
            struct PatrolVisit {
                int32_t index;
                uint64_t firstTick;
                uint64_t waitSeen;
            };
            const auto follow = [&](NavSim& sim, EntityID walker, size_t wantVisits, int maxTicks, std::vector<PatrolVisit>& visits) {
                visits.clear();
                for (int i = 0; i < maxTicks && visits.size() < wantVisits; ++i) {
                    sim.Step();
                    BtPatrolState state;
                    if (!patrolStateOf(sim, walker, state)) {
                        continue;
                    }
                    const uint64_t now = sim.tick - 1;
                    if (visits.empty() || visits.back().index != state.nextIndex) {
                        visits.push_back({ state.nextIndex, now, 0 });
                    }
                    if (state.phase == btpatrolphase::kWaiting && visits.back().waitSeen == 0) {
                        visits.back().waitSeen = now;
                    }
                }
            };
            const auto indices = [](const std::vector<PatrolVisit>& visits) {
                std::vector<int32_t> out;
                for (const PatrolVisit& v : visits) {
                    out.push_back(v.index);
                }
                return out;
            };
            // 4 m 四方の小さな経路 (床の中。Agent は (-6, 0) から始まり、一番近い点は 0)
            const std::vector<PatrolPoint> square = { { -6.0f, -3.0f, 10 }, { -2.0f, -3.0f, 0 }, { -2.0f, 1.0f, 20 } };

            // ---- Loop: 一周して戻る / 待ち時間 ----
            {
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                NavSim sim(f->scene);
                sim.Step();
                setRoute(sim, f->walker, route);
                std::vector<PatrolVisit> visits;
                follow(sim, f->walker, 6, 2400, visits);
                const std::vector<int32_t> got = indices(visits);
                ck.Check(got == std::vector<int32_t>{ 0, 1, 2, 0, 1, 2 }, "Patrol Loop: 最後の点の次は最初の点へ戻り、0 -> 1 -> 2 -> 0 -> 1 -> 2 と回る");
                ck.Check(visits.size() >= 4 && visits[0].waitSeen != 0 && visits[1].firstTick - visits[0].waitSeen == 10 && visits[1].waitSeen == 0
                             && visits[2].waitSeen != 0 && visits[3].firstTick - visits[2].waitSeen == 20,
                         "Patrol: 点に着いてから waitTicks (10 / 0 / 20) の tick 後に次の点へ向かい始める (待ち 0 は待たない)");
                ck.Check(sim.Agent(f->walker)->hasDestination && sim.Comp(f->walker)->status == kRunning, "Patrol Loop: 終わらず Running のまま目的地を持つ");
            }

            // ---- PingPong: 端で折り返す ----
            {
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(plainTree, patrolmode::kPingPong, { { -6.0f, -3.0f, 0 }, { -2.0f, -3.0f, 0 }, { -2.0f, 1.0f, 0 } }, f, route);
                NavSim sim(f->scene);
                sim.Step();
                setRoute(sim, f->walker, route);
                std::vector<PatrolVisit> visits;
                follow(sim, f->walker, 8, 3000, visits);
                ck.Check(indices(visits) == std::vector<int32_t>{ 0, 1, 2, 1, 0, 1, 2, 1 }, "Patrol PingPong: 端で折り返して 0 -> 1 -> 2 -> 1 -> 0 -> 1 -> 2 -> 1 と往復する");
            }

            // ---- Once: 最後の点で待ち終えたら Success ----
            {
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(plainTree, patrolmode::kOnce, { { -6.0f, -3.0f, 0 }, { -2.0f, -3.0f, 0 }, { -2.0f, 1.0f, 15 } }, f, route);
                NavSim sim(f->scene);
                sim.Step();
                setRoute(sim, f->walker, route);
                std::vector<PatrolVisit> visits;
                follow(sim, f->walker, 3, 2400, visits);
                uint64_t lastWait = 0;
                uint64_t doneTick = 0;
                for (int i = 0; i < 600 && doneTick == 0; ++i) {
                    sim.Step();
                    BtPatrolState state;
                    if (lastWait == 0 && patrolStateOf(sim, f->walker, state) && state.phase == btpatrolphase::kWaiting && state.nextIndex == 2) {
                        lastWait = sim.tick - 1; // 最後の点に着いた tick
                    }
                    if (sim.Comp(f->walker)->status == kSucceeded) {
                        doneTick = sim.tick - 1;
                    }
                }
                ck.Check(indices(visits) == std::vector<int32_t>{ 0, 1, 2 } && lastWait != 0 && doneTick == lastWait + 15 && !sim.Agent(f->walker)->hasDestination,
                         "Patrol Once: 最後の点で waitTicks (15) 待ち終えた tick に Success、目的地は倒れる");
            }

            // ---- ルートが使えなければ Failure ----
            {
                const auto firstFailure = [&](NavSim& sim, EntityID walker, int maxTicks) {
                    for (int i = 0; i < maxTicks; ++i) {
                        sim.Step();
                        if (sim.Comp(walker)->status == kFailed) {
                            return sim.tick - 1;
                        }
                    }
                    return static_cast<uint64_t>(0);
                };
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, {}, f, route); // 点が 0 個
                    NavSim sim(f->scene);
                    sim.Step();
                    setRoute(sim, f->walker, route);
                    ck.Check(firstFailure(sim, f->walker, 20) == 6 && !sim.Agent(f->walker)->hasDestination, "Patrol: 点が 0 個のルートは Failure (目的地は書かない)");
                }
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                    NavSim sim(f->scene);
                    sim.Step(); // Route キーは未設定のまま
                    ck.Check(firstFailure(sim, f->walker, 20) == 6 && !sim.Agent(f->walker)->hasDestination, "Patrol: Route キーが未設定なら Failure");
                }
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                    NavSim sim(f->scene);
                    sim.Step();
                    setRoute(sim, f->walker, f->walker); // PatrolRoute を持たないエンティティ
                    ck.Check(firstFailure(sim, f->walker, 20) == 6, "Patrol: Route が PatrolRoute を持たないエンティティなら Failure");
                }
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                    NavSim sim(f->scene);
                    sim.Step();
                    setRoute(sim, f->walker, route);
                    f->scene.GetWorld().DestroyEntity(route);
                    f->scene.GetWorld().ApplyStructuralChanges();
                    ck.Check(firstFailure(sim, f->walker, 20) == 6, "Patrol: ルートのエンティティが消えていたら Failure");
                }
                {
                    const uint64_t noAgent = plainTree;
                    Scene scene;
                    BehaviorTreeSystem bt;
                    GameObject go = scene.CreateGameObjectTracked("NoAgent");
                    go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ noAgent };
                    GameObject routeObject = scene.CreateGameObjectTracked("Route");
                    auto* component = routeObject.AddComponent<PatrolRouteComponent>();
                    component->pointCount = 1;
                    scene.GetWorld().ApplyStructuralChanges();
                    bt.Update(scene.GetWorld(), 1, nullptr);
                    const_cast<BtInstance*>(bt.FindInstance(go.Id()))->blackboard[0] = BbValue{ 1, 0, 0.0f, { 0.0f, 0.0f, 0.0f }, routeObject.Id() };
                    bool failed = false;
                    for (uint64_t t = 2; t < 20 && !failed; ++t) {
                        bt.Update(scene.GetWorld(), t, nullptr);
                        failed = scene.GetWorld().GetComponent<BehaviorTreeComponent>(go.Id())->status == kFailed;
                    }
                    ck.Check(failed, "Patrol: NavMeshAgent が無ければ Failure");
                }
            }

            // ---- 同じルートを 2 体が別々の進み具合で共有する ----
            {
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(plainTree, patrolmode::kLoop, { { -6.0f, -3.0f, 0 }, { -2.0f, -3.0f, 0 }, { 4.0f, 3.0f, 0 } }, f, route);
                const EntityID second = AddWalker(f->scene, plainTree, 3.0f, 2.0f, "Walker2");
                f->scene.GetWorld().ApplyStructuralChanges();
                NavSim sim(f->scene);
                sim.Step();
                setRoute(sim, f->walker, route);
                setRoute(sim, second, route);
                std::vector<PatrolVisit> a;
                std::vector<PatrolVisit> b;
                for (int i = 0; i < 700; ++i) {
                    sim.Step();
                    for (const auto& who : { std::pair<EntityID, std::vector<PatrolVisit>*>{ f->walker, &a }, std::pair<EntityID, std::vector<PatrolVisit>*>{ second, &b } }) {
                        BtPatrolState state;
                        if (patrolStateOf(sim, who.first, state) && (who.second->empty() || who.second->back().index != state.nextIndex)) {
                            who.second->push_back({ state.nextIndex, sim.tick - 1, 0 });
                        }
                    }
                }
                const std::vector<int32_t> ia = indices(a);
                const std::vector<int32_t> ib = indices(b);
                MYE_LOG_INFO("  [patrol] shared route: walker A visits %zu, walker B visits %zu", ia.size(), ib.size());
                ck.Check(ia.size() >= 2 && ib.size() >= 2 && ia[0] == 0 && ib[0] == 2 && ia[1] == 1 && ib[1] == 0,
                         "Patrol: 同じルートを 2 体が共有し、それぞれ今いる位置の一番近い点 (0 と 2) から別々の順で進む");
                ck.Check(sim.GetWorld().GetComponent<PatrolRouteComponent>(route)->pointCount == 3, "Patrol: ルートのコンポーネントには進み具合を書かない");
            }

            // ---- 一番近い点 (同距離は index 小) ----
            {
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(plainTree, patrolmode::kLoop, { { 4.0f, 2.0f, 0 }, { -4.0f, 2.0f, 0 } }, f, route);
                NavSim sim(f->scene);
                sim.Step();
                sim.Pos(f->walker)->position = { 0.0f, 0.9f, 2.0f }; // 2 つの点から同じ距離
                setRoute(sim, f->walker, route);
                std::vector<PatrolVisit> visits;
                follow(sim, f->walker, 1, 40, visits);
                ck.Check(visits.size() == 1 && visits[0].index == 0, "Patrol: 同距離の点が複数あるときは index の小さい方から始める");
            }

            // ---- Abort の後は一番近い点から ----
            {
                std::vector<int32_t> trace;
                const uint64_t abortTree = RegisterTree(
                    lib, L"patrol_abort",
                    Tree(0, { Node(0, "Selector", { 1, 3 }), Decorated(Node(1, "Sequence", { 4, 2 }), { BbCond("Alarm", "IsSet", "Both") }),
                              patrolNode(2), Wait(3, 5000), Wait(4, 5) },
                         patrolBoard));
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(abortTree, patrolmode::kLoop, square, f, route);
                NavSim sim(f->scene);
                sim.Step();
                setRoute(sim, f->walker, route);
                sim.bt->SetAbortTrace(&trace);
                std::vector<PatrolVisit> visits;
                follow(sim, f->walker, 2, 1500, visits); // 点 0 に着いて待ち、点 1 へ向かっている
                const bool onTheWay = visits.size() == 2 && visits[1].index == 1 && sim.Agent(f->walker)->hasDestination;
                sim.Mutable(f->walker)->blackboard[1].i = 0; // Alarm を倒す = Self の Abort
                sim.Step();
                BtPatrolState dead;
                const bool stopped = !sim.Agent(f->walker)->hasDestination && !patrolStateOf(sim, f->walker, dead) && trace.size() >= 2 && trace.front() == kPatrolNode;
                for (int i = 0; i < 60; ++i) {
                    sim.Step();
                }
                sim.Pos(f->walker)->position = { -2.0f, 0.9f, 2.0f }; // 点 2 (-2, 1) のすぐそば。点 1 への道の途中ではない
                sim.Mutable(f->walker)->blackboard[1].i = 1; // Alarm を戻す = LowerPriority で Wait を Abort して入り直す
                BtPatrolState back;
                bool restarted = false;
                for (int i = 0; i < 30 && !restarted; ++i) {
                    sim.Step();
                    restarted = patrolStateOf(sim, f->walker, back);
                }
                sim.bt->SetAbortTrace(nullptr);
                ck.Check(onTheWay && stopped && restarted && back.nextIndex == 2,
                         "Patrol: Abort で目的地を倒して止まり、戻ってきたら (点 1 へ向かっていたのに) 今の位置から一番近い点 2 から再開する");
            }

            // ---- failOnStuck: ベイク後に置いた壁で塞がれる (MoveTo / SearchArea と同じ壁) ----
            for (int failOnStuck = 0; failOnStuck < 2; ++failOnStuck) {
                const uint64_t guid = RegisterTree(
                    lib, failOnStuck != 0 ? L"patrol_stuck_fail" : L"patrol_stuck_wait",
                    Tree(0, { Node(0, "Selector", { 1, 3 }), Node(1, "Sequence", { 4, 2 }), patrolNode(2, failOnStuck != 0), Wait(3, 5000), Wait(4, 5) },
                         patrolBoard));
                std::unique_ptr<Field> f;
                EntityID route;
                makePatrolField(guid, patrolmode::kLoop, { { 6.0f, 0.0f, 0 }, { 6.0f, 4.0f, 0 } }, f, route);
                GameObject wall = f->scene.CreateGameObjectTracked("LateWall");
                wall.SetLocalPosition(0.0f, 1.0f, 0.0f);
                auto* collider = wall.AddComponent<ColliderComponent>();
                collider->shape = collidershape::kBox;
                collider->halfExtents = { 0.5f, 1.0f, 13.0f };
                f->scene.GetWorld().ApplyStructuralChanges();
                NavSim sim(f->scene);
                sim.Step();
                setRoute(sim, f->walker, route);
                bool sawStuck = false;
                bool failed = false;
                for (int i = 0; i < 700; ++i) {
                    sim.Step();
                    sawStuck = sawStuck || sim.Agent(f->walker)->status == navagentstatus::kStuck;
                    failed = failed || sim.Comp(f->walker)->activeNodeId == 3;
                }
                const bool destination = sim.Agent(f->walker)->hasDestination;
                const int32_t active = sim.Comp(f->walker)->activeNodeId;
                MYE_LOG_INFO("  [patrol] stuck %d: sawStuck %d failed %d dest %d active %d", failOnStuck, sawStuck ? 1 : 0, failed ? 1 : 0, destination ? 1 : 0, active);
                if (failOnStuck == 0) {
                    ck.Check(sawStuck && !failed && destination && active == kPatrolNode, "Patrol: failOnStuck = false (既定) は Stuck の間も Running のまま");
                } else {
                    ck.Check(sawStuck && failed && !destination && active == 3,
                             "Patrol: failOnStuck = true は Stuck になった tick に Failure、目的地を倒して次の枝へ移る");
                }
            }

            // ---- アセット: 往復と既定値 ----
            {
                BehaviorTreeAsset a;
                BehaviorTreeAsset b;
                const json source = Tree(0, { patrolNode(0, true) }, patrolBoard);
                const bool loaded = BehaviorTreeLibrary::FromJson(source, a);
                const bool again = loaded && BehaviorTreeLibrary::FromJson(BehaviorTreeLibrary::ToJson(a), b);
                ck.Check(loaded && again && BehaviorTreeLibrary::ToJson(a) == BehaviorTreeLibrary::ToJson(b) && a.nodes[0].params[btpatrolparam::kFailOnStuck].i == 1
                             && a.nodes[0].keys[btpatrolkey::kRoute] == "Route" && a.extraStateBytes == static_cast<int32_t>(sizeof(BtPatrolState)),
                         "Patrol の JSON が往復で変わらず、failOnStuck と route キーと追加状態の大きさが読める");
                BehaviorTreeAsset defaults;
                ck.Check(BehaviorTreeLibrary::FromJson(Tree(0, { Node(0, "Patrol") }), defaults) && defaults.nodes[0].params[btpatrolparam::kFailOnStuck].i == 0
                             && defaults.nodes[0].params[btpatrolparam::kAcceptanceRadius].f == 0.5f,
                         "Patrol の既定は failOnStuck = false・acceptanceRadius = 0.5");
            }

            // ---- 同梱の assets\ai (patrol_only.bt.json / patrol.bb.json) が読めて、Patrol 1 個の木になっている ----
            {
                const fs::path btPath = L"assets/ai/patrol_only.bt.json";
                const fs::path bbPath = L"assets/ai/patrol.bb.json";
                std::error_code ec;
                if (fs::exists(btPath, ec) && fs::exists(bbPath, ec)) {
                    std::ifstream btFile(btPath, std::ios::binary);
                    std::ifstream bbFile(bbPath, std::ios::binary);
                    BehaviorTreeAsset shippedTree;
                    BlackboardAsset shippedBoard;
                    const bool ok = BehaviorTreeLibrary::FromJson(json::parse(btFile, nullptr, false), shippedTree)
                                    && BlackboardLibrary::FromJson(json::parse(bbFile, nullptr, false), shippedBoard);
                    ck.Check(ok && shippedTree.nodes.size() == 1 && shippedTree.nodes[0].kind == BtNodeKind::Patrol && shippedBoard.keys.size() == 1
                                 && shippedBoard.keys[0].type == BbType::Entity && shippedBoard.FindKey(shippedTree.nodes[0].keys[btpatrolkey::kRoute]) == 0,
                             "同梱の patrol_only.bt.json は Patrol 1 個の木で、patrol.bb.json の Entity キー route を指す");
                } else {
                    MYE_LOG_INFO("  SKIP: assets/ai が作業ディレクトリに無いので同梱アセットの検査を飛ばす");
                }
            }

            // ---- Entity キーの初期値 (spec 2. #20): コンポーネントへルートを割り当てるだけで BB に入り、巡回する ----
            {
                const auto setInitial = [](Field& field, int slot, const char* key, EntityID value) {
                    auto* component = field.scene.GetWorld().GetComponent<BehaviorTreeComponent>(field.walker);
                    std::snprintf(component->bbEntityKey[slot], kBtEntityKeyBytes, "%s", key);
                    component->bbEntityValue[slot] = value;
                };
                const auto routeKeyOf = [](NavSim& sim, EntityID e, size_t key) {
                    const BtInstance* inst = sim.bt->FindInstance(e);
                    return inst != nullptr && key < inst->blackboard.size() ? inst->blackboard[key] : BbValue{};
                };
                // 木を始めた tick に書かれ、setRoute 無しで Loop が 0 -> 1 -> 2 と回る
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                    setInitial(*f, 0, "Route", route);
                    NavSim sim(f->scene);
                    sim.Step();
                    const BbValue written = routeKeyOf(sim, f->walker, 0);
                    ck.Check(written.isSet == 1 && written.entity == route, "Entity キーの初期値: 木を始めた tick に BB の Route キーへ書かれる");
                    std::vector<PatrolVisit> visits;
                    follow(sim, f->walker, 4, 2400, visits);
                    ck.Check(indices(visits) == std::vector<int32_t>{ 0, 1, 2, 0 }, "Entity キーの初期値: コードで BB を書かなくても Patrol が回る");
                }
                // 名前が BB に無い・Entity 型でない・値が null の組は書かない。無効な組の警告は 1 組につき 1 回
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                    setInitial(*f, 0, "Nothing", route);  // BB に無い名前
                    setInitial(*f, 1, "Alarm", route);    // Bool のキー
                    setInitial(*f, 2, "Route", kNullEntity); // 値が null
                    const uint64_t before = logging::TotalWritten();
                    NavSim sim(f->scene);
                    for (int i = 0; i < 5; ++i) {
                        sim.Step();
                    }
                    const BbValue alarm = routeKeyOf(sim, f->walker, 1);
                    ck.Check(routeKeyOf(sim, f->walker, 0).isSet == 0 && alarm.isSet == 1 && alarm.i == 1 && alarm.entity.IsNull(),
                             "Entity キーの初期値: 無効な組 (名前が無い・Entity でない・null) は書かず、BB の初期値のまま");
                    ck.Check(CountWarnings(before, "initial Entity key") == 2, "Entity キーの初期値: 無効な組は 1 組につき警告 1 回 (null の値は黙って飛ばす)");
                }
                // 同梱の patrol_only.bt.json を、ルートのエンティティを割り当てるだけで動かす
                {
                    // 同梱ファイルの中身を使う。登録のキー (GUID) は試験の環境で変わるので、木が指す BB の GUID だけ登録後の値へ差し替える
                    std::ifstream btFile(L"assets/ai/patrol_only.bt.json", std::ios::binary);
                    std::ifstream bbFile(L"assets/ai/patrol.bb.json", std::ios::binary);
                    json shippedTree = json::parse(btFile, nullptr, false);
                    BlackboardAsset shippedBoard;
                    uint64_t boardGuid = 0;
                    uint64_t treeGuid = 0;
                    if (!shippedTree.is_discarded() && BlackboardLibrary::FromJson(json::parse(bbFile, nullptr, false), shippedBoard)) {
                        boardGuid = lib.boards.Register(BoardPath(L"patrol_shipped"), std::move(shippedBoard));
                        shippedTree["blackboard"] = GuidHex(boardGuid);
                        treeGuid = RegisterTree(lib, L"patrol_shipped", shippedTree);
                    }
                    if (boardGuid != 0 && treeGuid != 0) {
                        std::unique_ptr<Field> f;
                        EntityID route;
                        makePatrolField(treeGuid, patrolmode::kLoop, square, f, route);
                        setInitial(*f, 0, "route", route);
                        NavSim sim(f->scene);
                        for (int i = 0; i < 20; ++i) {
                            sim.Step(); // Surface が読み込まれる前の Agent は Inactive。目的地が立つまで少し回す
                        }
                        const bool moving = sim.Comp(f->walker)->status == kRunning && sim.Agent(f->walker)->hasDestination;
                        ck.Check(moving, "同梱の patrol_only.bt.json は、BehaviorTree の Entity キーの初期値へルートを割り当てるだけで巡回する");
                    } else {
                        MYE_LOG_INFO("  SKIP: assets/ai が作業ディレクトリに無いので patrol_only の実走を飛ばす");
                    }
                }
                // 保存 -> 復元で、書かれた値が戻る (初期値は復元のときに再び書かれない = 保存された値が勝つ)
                {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kLoop, square, f, route);
                    setInitial(*f, 0, "Route", route);
                    NavSim sim(f->scene);
                    sim.Step();
                    sim.Step();
                    const BtInstance* inst = sim.bt->FindInstance(f->walker);
                    std::vector<std::byte> bytes;
                    ByteWriter w(bytes);
                    sim.bt->SaveSnapshot(w);
                    ByteReader r(bytes.data(), bytes.size());
                    BtSnapshot snapshot;
                    const bool read = inst != nullptr && BehaviorTreeSystem::ReadSnapshot(r, snapshot);
                    BehaviorTreeSystem restoredBt;
                    if (read) {
                        restoredBt.ApplySnapshot(f->scene.GetWorld(), std::move(snapshot));
                    }
                    const BtInstance* back = restoredBt.FindInstance(f->walker);
                    ck.Check(read && back != nullptr && back->blackboard.size() == 2 && back->blackboard[0].entity == route && restoredBt.StateHash() == sim.bt->StateHash(),
                             "Entity キーの初期値: 保存 -> 復元で BB の値が戻り、ハッシュが一致する");
                }
            }

            // ---- 巡回の途中 (待ち中・移動中) で保存 -> 復元 -> 連続実行と毎 tick のハッシュが一致 ----
            {
                const auto patrolRoundTrip = [&](const char* label, bool freshBt, const std::function<bool(const BtPatrolState&)>& atCapture) {
                    std::unique_ptr<Field> f;
                    EntityID route;
                    makePatrolField(plainTree, patrolmode::kPingPong, { { -6.0f, -3.0f, 0 }, { -2.0f, -3.0f, 30 }, { -2.0f, 1.0f, 0 } }, f, route);
                    NavSim sim(f->scene);
                    sim.Step();
                    setRoute(sim, f->walker, route);
                    SimRefs refs;
                    refs.scene = &f->scene;
                    refs.nav = &sim.nav;
                    refs.behaviorTree = sim.bt;
                    uint64_t tickRef = 0;
                    refs.tickIndex = &tickRef;
                    bool captured = false;
                    for (int i = 0; i < 1500 && !captured; ++i) {
                        sim.Step();
                        BtPatrolState state;
                        captured = patrolStateOf(sim, f->walker, state) && atCapture(state);
                    }
                    if (!captured) {
                        ck.Check(false, label);
                        return;
                    }
                    tickRef = sim.tick;
                    std::vector<std::byte> blob;
                    const bool ok = CaptureSimSnapshot(refs, blob);
                    BtPatrolState atState;
                    patrolStateOf(sim, f->walker, atState);
                    const uint64_t startTick = sim.tick;
                    constexpr int kAhead = 500;
                    std::vector<uint64_t> continuous;
                    for (int i = 0; i < kAhead; ++i) {
                        sim.Step();
                        continuous.push_back(HashWorld(f->scene.GetWorld(), refs.HashSources()));
                    }
                    BehaviorTreeSystem fresh;
                    if (freshBt) {
                        sim.bt = &fresh;
                        refs.behaviorTree = &fresh;
                    }
                    bool same = ok && RestoreSimSnapshot(refs, blob.data(), blob.size());
                    BtPatrolState restored;
                    const bool restoredSame = same && patrolStateOf(sim, f->walker, restored) && restored.nextIndex == atState.nextIndex
                                              && restored.phase == atState.phase && restored.waitRemaining == atState.waitRemaining
                                              && restored.direction == atState.direction;
                    sim.tick = startTick;
                    for (int i = 0; i < kAhead && same; ++i) {
                        sim.Step();
                        same = HashWorld(f->scene.GetWorld(), refs.HashSources()) == continuous[static_cast<size_t>(i)];
                    }
                    ck.Check(same && restoredSame && continuous.front() != continuous.back(), label);
                };
                patrolRoundTrip("Patrol の待ち中 (点 1 で waitTicks 30 の途中) で保存 -> 復元 -> 連続実行と毎 tick のハッシュが一致", false,
                                [](const BtPatrolState& s) { return s.phase == btpatrolphase::kWaiting && s.nextIndex == 1 && s.waitRemaining == 20; });
                patrolRoundTrip("Patrol の待ち中で保存 -> 新しい BehaviorTreeSystem へ復元 -> 連続実行と毎 tick のハッシュが一致", true,
                                [](const BtPatrolState& s) { return s.phase == btpatrolphase::kWaiting && s.nextIndex == 1 && s.waitRemaining == 20; });
                patrolRoundTrip("Patrol の移動中 (PingPong の折り返し後の向き -1) で保存 -> 復元 -> 連続実行と毎 tick のハッシュが一致", true,
                                [](const BtPatrolState& s) { return s.phase == btpatrolphase::kMoving && s.direction == -1 && s.nextIndex == 1; });
            }
        }

        // ---- 13. AI ノード 4 種 (M85d) ----
        {
            const uint64_t aiBoard = RegisterBoard(
                lib, L"ai_bb",
                Board({ BbKey("Target", "Entity"), BbKey("Pos", "Vector"), BbKey("Quarry", "Entity"),
                        BbKey("Last", "Vector", json::array({ 6.0, 0.0, 6.0 })), BbKey("Rand", "Vector"), BbKey("Unset", "Vector"),
                        BbKey("Center", "Vector", json::array({ -6.0, 0.0, 6.0 })),
                        BbKey("FarOrigin", "Vector", json::array({ 40.0, 0.0, 0.0 })) }));
            const auto bbOf = [](NavSim& sim, EntityID e, const char* name) -> const BbValue* {
                const BtInstance* inst = sim.bt->FindInstance(e);
                if (inst == nullptr || !inst->blackboardAsset) {
                    return nullptr;
                }
                const int key = inst->blackboardAsset->FindKey(name);
                return key >= 0 ? &inst->blackboard[static_cast<size_t>(key)] : nullptr;
            };
            const auto setBbEntity = [&](NavSim& sim, EntityID owner, const char* name, EntityID value) {
                const BbValue* slot = bbOf(sim, owner, name);
                if (slot != nullptr) {
                    BbValue* writable = const_cast<BbValue*>(slot);
                    writable->isSet = 1;
                    writable->entity = value;
                }
            };
            struct AiRun {
                std::unique_ptr<Field> f;
                std::unique_ptr<NavSim> sim;
            };
            // 見る側 = 歩く Agent (-6, 0, 0)。+Z を向く。陣営 1 で、陣営 0 だけが敵
            const auto makeRun = [&](uint64_t tree, float fovDeg, AiRun& run) {
                makeField(tree, run.f);
                World& world = run.f->scene.GetWorld();
                auto* perception = world.AddComponent<AIPerceptionComponent>(run.f->walker);
                perception->fovDeg = fovDeg;
                perception->sightRadius = 12.0f;
                perception->loseSightRadius = 14.0f;
                perception->hostileMask = 1u; // 陣営 0 だけが敵。陣営 2 は中立
                world.ApplyStructuralChanges();
                run.sim = std::make_unique<NavSim>(run.f->scene);
            };
            const auto addStimulus = [](Scene& scene, float x, float z, int faction, const char* name) {
                GameObject go = scene.CreateGameObjectTracked(name);
                go.SetLocalPosition(x, 0.9f, z);
                auto* source = go.AddComponent<AIStimulusSourceComponent>();
                source->faction = faction;
                source->targetHeight = 1.6f; // 目の高さ (0.9 + 1.6) と合わせて視線を水平にする
                return go.Id();
            };
            const auto stepN = [](NavSim& sim, int ticks) {
                for (int i = 0; i < ticks; ++i) {
                    sim.Step();
                }
            };
            const auto horizontalFrom = [](const BbValue* v, float x, float z) {
                return std::sqrt((v->v[0] - x) * (v->v[0] - x) + (v->v[2] - z) * (v->v[2] - z));
            };

            // ---- アセット: 4 種の往復・範囲の丸め・タグの 64 ビットマスク ----
            {
                const uint64_t tagMask = 0x8000000000000001ull;
                json j = Tree(0, { Node(0, "Sequence", { 1, 2, 3, 4 }), FindRandom(1, "Home", "Out", 7.5),
                                   FindNearest(2, true, false, true, false, true, "T", "P"), Search(3, "O", "E", true, 3.0, 5),
                                   FindTargetNode(4, 20.0, true, false, true, tagMask, "T") });
                BehaviorTreeAsset asset;
                const bool loaded = BehaviorTreeLibrary::FromJson(j, asset);
                const json again = loaded ? BehaviorTreeLibrary::ToJson(asset) : json();
                BehaviorTreeAsset second;
                const bool reloaded = loaded && BehaviorTreeLibrary::FromJson(again, second);
                ck.Check(loaded && reloaded && BehaviorTreeLibrary::ToJson(second) == again, "AI ノード 4 種は書き出して読み直しても同じ");
                const BtNodeDef* findTarget = loaded ? &asset.nodes[static_cast<size_t>(asset.FindNode(4))] : nullptr;
                const BtNodeDef* search = loaded ? &asset.nodes[static_cast<size_t>(asset.FindNode(3))] : nullptr;
                ck.Check(findTarget != nullptr && findTarget->params[btfindtargetparam::kTagMask].u == tagMask
                             && findTarget->keys[btfindtargetkey::kTarget] == "T",
                         "FindTarget の tagMask は 64 ビットのまま保存される (最上位ビットも)");
                ck.Check(search != nullptr && search->keys[btsearchkey::kOrigin] == "O" && search->keys[btsearchkey::kEndTarget] == "E"
                             && search->params[btsearchparam::kPointCount].i == 5 && search->params[btsearchparam::kUsePrediction].i == 1,
                         "SearchArea の keys と params を読める");
                json clamped = Tree(0, { Search(0, "O", nullptr, false, 3.0, 99) });
                json low = Tree(0, { Search(0, "O", nullptr, false, 3.0, 0) });
                BehaviorTreeAsset hi;
                BehaviorTreeAsset lo;
                ck.Check(BehaviorTreeLibrary::FromJson(clamped, hi) && hi.nodes[0].params[btsearchparam::kPointCount].i == kBtMaxSearchPoints
                             && BehaviorTreeLibrary::FromJson(low, lo) && lo.nodes[0].params[btsearchparam::kPointCount].i == 1,
                         "SearchArea の pointCount は 1 〜 32 へ丸める");
                json bad = Tree(0, { Node(0, "FindTarget", {}, json{ { "tagMask", "zz" } }) });
                ck.Check(!Loads(bad), "壊れた tagMask (16 進でない) は読み込みを拒否する");
                ck.Check(BtNodeTypeOf(BtNodeKind::SearchArea).extraStateBytes == static_cast<int>(sizeof(BtSearchAreaState))
                             && BtNodeTypeOf(BtNodeKind::FindTarget).category == BtNodeCategory::Ai,
                         "SearchArea だけが追加状態を持ち、4 種とも AI 分類");
            }

            // ---- FindRandomPoint ----
            {
                const auto collect = [&](uint64_t guid, std::vector<float>& xz, int& successes, int32_t& lastStatus) {
                    AiRun run;
                    makeRun(guid, 90.0f, run);
                    stepN(*run.sim, 5);
                    successes = 0;
                    for (int i = 0; i < 8; ++i) {
                        run.sim->Step();
                        lastStatus = run.sim->Comp(run.f->walker)->status;
                        const BbValue* rand = bbOf(*run.sim, run.f->walker, "Rand");
                        if (lastStatus == kSucceeded && rand != nullptr && rand->isSet != 0) {
                            ++successes;
                            xz.push_back(rand->v[0]);
                            xz.push_back(rand->v[2]);
                        }
                    }
                };
                const uint64_t selfTree = RegisterTree(lib, L"ai_random_self", Tree(0, { FindRandom(0, nullptr, "Rand", 4.0) }, aiBoard));
                std::vector<float> first;
                std::vector<float> second;
                int successesA = 0;
                int successesB = 0;
                int32_t statusA = 0;
                int32_t statusB = 0;
                collect(selfTree, first, successesA, statusA);
                collect(selfTree, second, successesB, statusB);
                bool inside = !first.empty();
                bool distinct = false;
                for (size_t i = 0; i + 1 < first.size(); i += 2) {
                    inside = inside && std::hypot(first[i] + 6.0f, first[i + 1]) <= 4.001f;
                    distinct = distinct || first[i] != first[0] || first[i + 1] != first[1];
                }
                ck.Check(successesA == 8 && inside && distinct, "FindRandomPoint: 自分を中心に半径内の点を毎回書いて Success (点は tick ごとに変わる)");
                ck.Check(first == second && successesB == 8, "FindRandomPoint: 同じ条件から同じ点列 (World の RNG だけを使う)");
                const uint64_t keyTree = RegisterTree(lib, L"ai_random_key", Tree(0, { FindRandom(0, "Center", "Rand", 3.0) }, aiBoard));
                std::vector<float> viaKey;
                collect(keyTree, viaKey, successesA, statusA);
                bool nearCenter = !viaKey.empty();
                for (size_t i = 0; i + 1 < viaKey.size(); i += 2) {
                    nearCenter = nearCenter && std::hypot(viaKey[i] + 6.0f, viaKey[i + 1] - 6.0f) <= 3.001f;
                }
                ck.Check(successesA == 8 && nearCenter, "FindRandomPoint: 中心を Vector キーで指すと、その点の半径内に出る");
                const auto failsWith = [&](const char* label, const json& node) {
                    const uint64_t guid = RegisterTree(lib, L"ai_random_fail", Tree(0, { node }, aiBoard));
                    std::vector<float> none;
                    int successes = 0;
                    int32_t status = 0;
                    collect(guid, none, successes, status);
                    ck.Check(successes == 0 && status == kFailed, label);
                };
                failsWith("FindRandomPoint: 半径 0 は Failure", FindRandom(0, nullptr, "Rand", 0.0));
                failsWith("FindRandomPoint: 書き先が Vector でないキーなら Failure", FindRandom(0, nullptr, "Target", 4.0));
                failsWith("FindRandomPoint: 中心のキーが未設定なら Failure", FindRandom(0, "Unset", "Rand", 4.0));
                failsWith("FindRandomPoint: 中心のキーが無ければ Failure", FindRandom(0, "NoSuchKey", "Rand", 4.0));
                {
                    // NavMeshAgent の無い木 (同じ場面の別エンティティ) と、ナビメッシュの外の中心
                    std::unique_ptr<Field> f;
                    makeField(selfTree, f);
                    GameObject bare = f->scene.CreateGameObjectTracked("Bare");
                    bare.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ selfTree };
                    const uint64_t farTree = RegisterTree(lib, L"ai_random_far", Tree(0, { FindRandom(0, "FarOrigin", "Rand", 3.0) }, aiBoard));
                    GameObject farOne = f->scene.CreateGameObjectTracked("FarOne");
                    farOne.SetLocalPosition(0.0f, 0.9f, 0.0f);
                    farOne.AddComponent<CharacterControllerComponent>();
                    farOne.AddComponent<NavMeshAgentComponent>();
                    farOne.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ farTree };
                    f->scene.GetWorld().ApplyStructuralChanges();
                    NavSim sim(f->scene);
                    stepN(sim, 8);
                    ck.Check(sim.Comp(bare.Id())->status == kFailed && sim.Comp(f->walker)->status == kSucceeded
                                 && sim.Comp(farOne.Id())->status == kFailed,
                             "FindRandomPoint: NavMeshAgent が無いと Failure、ナビメッシュの外の中心でも Failure (同じ場面の Agent は Success)");
                }
                {
                    // Surface の無い場面 (ナビメッシュが読めない) は Failure
                    Scene bareScene;
                    const EntityID walker = AddWalker(bareScene, selfTree, 0.0f, 0.0f);
                    bareScene.GetWorld().ApplyStructuralChanges();
                    NavSim sim(bareScene);
                    stepN(sim, 5);
                    ck.Check(sim.Comp(walker)->status == kFailed, "FindRandomPoint: Surface の無い場面では Failure");
                }
            }

            // ---- FindNearestTarget ----
            {
                const auto nearestTree = [&](const wchar_t* name, bool sight, bool hearing, bool damage, bool touch, bool currentOnly,
                                             const char* target = "Target") {
                    return RegisterTree(lib, name, Tree(0, { FindNearest(0, sight, hearing, damage, touch, currentOnly, target, "Pos") }, aiBoard));
                };
                const uint64_t allSenses = nearestTree(L"ai_nearest_all", true, true, true, true, false);
                {
                    AiRun run;
                    makeRun(allSenses, 90.0f, run);
                    const EntityID far = addStimulus(run.f->scene, -3.0f, 4.0f, 0, "Far");   // 距離 5
                    const EntityID near = addStimulus(run.f->scene, -6.0f, 3.0f, 0, "Near"); // 距離 3
                    run.f->scene.GetWorld().ApplyStructuralChanges();
                    stepN(*run.sim, 4);
                    const BbValue* target = bbOf(*run.sim, run.f->walker, "Target");
                    const BbValue* pos = bbOf(*run.sim, run.f->walker, "Pos");
                    ck.Check(run.sim->Comp(run.f->walker)->status == kSucceeded && target->isSet != 0 && target->entity == near
                                 && far != near && horizontalFrom(pos, -6.0f, 3.0f) < 0.01f,
                             "FindNearestTarget: 見えている相手のうち lastSensedPos が一番近いものを選び、位置も書く");
                }
                {
                    // 同距離 (どちらも 5 m): entity キーの小さい方。作る順を入れ替えても index の小さい方
                    bool smallerWins = true;
                    for (int order = 0; order < 2; ++order) {
                        AiRun run;
                        makeRun(allSenses, 90.0f, run);
                        const EntityID a = addStimulus(run.f->scene, order == 0 ? -3.0f : -9.0f, 4.0f, 0, "TieA");
                        const EntityID b = addStimulus(run.f->scene, order == 0 ? -9.0f : -3.0f, 4.0f, 0, "TieB");
                        run.f->scene.GetWorld().ApplyStructuralChanges();
                        stepN(*run.sim, 4);
                        const BbValue* target = bbOf(*run.sim, run.f->walker, "Target");
                        smallerWins = smallerWins && target->isSet != 0 && target->entity.index == (std::min)(a.index, b.index);
                    }
                    ck.Check(smallerWins, "FindNearestTarget: 同じ距離なら entity キーの小さい方 (作る順を入れ替えても)");
                }
                const auto noiseRun = [&](uint64_t tree, bool named, EntityID& chosenOut, EntityID& frontOut, EntityID& behindOut,
                                          bool& positionOk, bool& entityCleared, int32_t& status) {
                    AiRun run;
                    makeRun(tree, 90.0f, run);
                    frontOut = addStimulus(run.f->scene, -6.0f, 3.0f, 0, "Front");
                    behindOut = addStimulus(run.f->scene, -6.0f, -5.0f, 0, "Behind");
                    run.f->scene.GetWorld().ApplyStructuralChanges();
                    stepN(*run.sim, 3);
                    setBbEntity(*run.sim, run.f->walker, "Target", run.f->walker); // 書き換わったか見るための目印
                    const float noisePos[3] = { -6.0f, 0.9f, named ? -5.0f : -4.0f };
                    PerceptionReportNoise(run.f->scene.GetWorld(), noisePos, 1.0f, 20.0f, named ? behindOut : kNullEntity);
                    run.sim->Step(); // 報告と同じ tick の BT が結果を読む (知覚 → BT の順)
                    const BbValue* target = bbOf(*run.sim, run.f->walker, "Target");
                    const BbValue* pos = bbOf(*run.sim, run.f->walker, "Pos");
                    status = run.sim->Comp(run.f->walker)->status;
                    chosenOut = target->isSet != 0 ? target->entity : kNullEntity;
                    entityCleared = target->isSet == 0;
                    positionOk = horizontalFrom(pos, noisePos[0], noisePos[2]) < 0.01f;
                };
                EntityID chosen;
                EntityID front;
                EntityID behind;
                bool positionOk = false;
                bool cleared = false;
                int32_t status = 0;
                noiseRun(nearestTree(L"ai_nearest_hear", false, true, false, false, false), true, chosen, front, behind, positionOk, cleared, status);
                ck.Check(status == kSucceeded && chosen == behind && positionOk, "FindNearestTarget: 聴覚だけを許すと、見えている相手ではなく音の主を選ぶ (同じ tick の知覚)");
                noiseRun(nearestTree(L"ai_nearest_sight", true, false, false, false, false), true, chosen, front, behind, positionOk, cleared, status);
                ck.Check(status == kSucceeded && chosen == front, "FindNearestTarget: 視覚だけを許すと、音の主は選ばない");
                noiseRun(nearestTree(L"ai_nearest_unnamed", false, true, false, false, false), false, chosen, front, behind, positionOk, cleared, status);
                ck.Check(status == kSucceeded && cleared && positionOk,
                         "FindNearestTarget: 名乗らない音 (target = null) は Vector だけ書き、Entity キーは空にして Success");

                // 今知覚している相手だけ / 記憶も含む
                for (int currentOnly = 0; currentOnly < 2; ++currentOnly) {
                    AiRun run;
                    makeRun(nearestTree(currentOnly != 0 ? L"ai_nearest_current" : L"ai_nearest_memory", true, false, false, false, currentOnly != 0),
                            90.0f, run);
                    const EntityID quarry = addStimulus(run.f->scene, -6.0f, 3.0f, 0, "Quarry");
                    run.f->scene.GetWorld().ApplyStructuralChanges();
                    stepN(*run.sim, 4);
                    const bool seenFirst = run.sim->Comp(run.f->walker)->status == kSucceeded;
                    run.f->scene.GetWorld().GetComponent<LocalTransform>(quarry)->position = { -6.0f, 0.9f, -8.0f }; // 背後へ
                    stepN(*run.sim, 4);
                    const int32_t later = run.sim->Comp(run.f->walker)->status;
                    ck.Check(seenFirst && later == (currentOnly != 0 ? kFailed : kSucceeded),
                             currentOnly != 0 ? "FindNearestTarget: currentlySensedOnly = true は、見失った相手 (記憶だけ) を選ばない"
                                              : "FindNearestTarget: currentlySensedOnly = false は、見失った相手も記憶から選ぶ");
                }
                {
                    AiRun run;
                    makeRun(allSenses, 90.0f, run);
                    stepN(*run.sim, 4);
                    const BbValue* target = bbOf(*run.sim, run.f->walker, "Target");
                    ck.Check(run.sim->Comp(run.f->walker)->status == kFailed && target->isSet == 0, "FindNearestTarget: 何も知覚していなければ Failure で、何も書かない");
                }
                {
                    AiRun run;
                    makeRun(nearestTree(L"ai_nearest_badkey", true, true, true, true, false, "Pos"), 90.0f, run);
                    addStimulus(run.f->scene, -6.0f, 3.0f, 0, "Visible");
                    run.f->scene.GetWorld().ApplyStructuralChanges();
                    stepN(*run.sim, 4);
                    ck.Check(run.sim->Comp(run.f->walker)->status == kFailed, "FindNearestTarget: 書き先が Entity でないキーなら Failure");
                }
            }

            // ---- FindTarget ----
            {
                struct Scene13 {
                    EntityID enemyBehind, enemyTieFront, enemyFar, neutralNear, friendlyNearest;
                };
                const auto runFind = [&](const wchar_t* name, const json& node, Scene13& ids, const std::function<void(AiRun&, Scene13&)>& tweak,
                                         EntityID& chosen, int32_t& status) {
                    const uint64_t guid = RegisterTree(lib, name, Tree(0, { node }, aiBoard));
                    AiRun run;
                    makeRun(guid, 90.0f, run);
                    Scene& scene = run.f->scene;
                    // 自分も AIStimulusSource を持つ (自分は選ばれない)。陣営 1 = 味方
                    auto* self = scene.GetWorld().AddComponent<AIStimulusSourceComponent>(run.f->walker);
                    self->faction = 1;
                    ids.enemyBehind = addStimulus(scene, -6.0f, -6.0f, 0, "EnemyBehind");   // 距離 6 (背後 = 見えない)
                    ids.enemyTieFront = addStimulus(scene, -6.0f, 6.0f, 0, "EnemyTie");     // 距離 6 (同距離)
                    ids.enemyFar = addStimulus(scene, -6.0f, 8.0f, 0, "EnemyFar");          // 距離 8
                    ids.neutralNear = addStimulus(scene, -6.0f, 4.0f, 2, "Neutral");        // 距離 4
                    ids.friendlyNearest = addStimulus(scene, -6.0f, 2.0f, 1, "Friendly");   // 距離 2
                    scene.GetWorld().ApplyStructuralChanges();
                    if (tweak) {
                        tweak(run, ids);
                        scene.GetWorld().ApplyStructuralChanges();
                    }
                    stepN(*run.sim, 3);
                    const BbValue* target = bbOf(*run.sim, run.f->walker, "Target");
                    status = run.sim->Comp(run.f->walker)->status;
                    chosen = target->isSet != 0 ? target->entity : kNullEntity;
                };
                Scene13 ids;
                EntityID chosen;
                int32_t status = 0;
                runFind(L"ai_ft_enemy", FindTargetNode(0, 15.0, true, false, false, 0, "Target"), ids, nullptr, chosen, status);
                ck.Check(status == kSucceeded && chosen.index == (std::min)(ids.enemyBehind.index, ids.enemyTieFront.index),
                         "FindTarget: 敵だけ (同距離の 2 体は entity キーの小さい方、背後で見えていなくても選ぶ)");
                runFind(L"ai_ft_neutral", FindTargetNode(0, 15.0, false, true, false, 0, "Target"), ids, nullptr, chosen, status);
                ck.Check(status == kSucceeded && chosen == ids.neutralNear, "FindTarget: 中立だけ");
                runFind(L"ai_ft_friend", FindTargetNode(0, 15.0, false, false, true, 0, "Target"), ids, nullptr, chosen, status);
                ck.Check(status == kSucceeded && chosen == ids.friendlyNearest, "FindTarget: 味方だけ (自分は除く)");
                runFind(L"ai_ft_all", FindTargetNode(0, 15.0, true, true, true, 0, "Target"), ids, nullptr, chosen, status);
                ck.Check(status == kSucceeded && chosen == ids.friendlyNearest, "FindTarget: 全部許すと一番近い相手");
                runFind(L"ai_ft_none", FindTargetNode(0, 15.0, false, false, false, 0, "Target"), ids, nullptr, chosen, status);
                ck.Check(status == kFailed && chosen.IsNull(), "FindTarget: どの態度も許さなければ Failure");
                runFind(L"ai_ft_range", FindTargetNode(0, 5.0, true, false, false, 0, "Target"), ids, nullptr, chosen, status);
                ck.Check(status == kFailed && chosen.IsNull(), "FindTarget: 範囲 (5 m) に敵が居なければ Failure で、何も書かない");
                runFind(L"ai_ft_tag", FindTargetNode(0, 15.0, true, false, false, Tags::BitOf(3), "Target"), ids,
                        [](AiRun& run, Scene13& s) { Tags::SetOwnMask(run.f->scene.GetWorld(), s.enemyFar, Tags::BitOf(3)); }, chosen, status);
                ck.Check(status == kSucceeded && chosen == ids.enemyFar, "FindTarget: タグのマスクに合う相手だけから選ぶ (近い敵がタグ無しなら飛ばす)");
                runFind(L"ai_ft_tag_any", FindTargetNode(0, 15.0, true, false, false, Tags::BitOf(3) | Tags::BitOf(5), "Target"), ids,
                        [](AiRun& run, Scene13& s) {
                            Tags::SetOwnMask(run.f->scene.GetWorld(), s.enemyFar, Tags::BitOf(5));
                            Tags::SetOwnMask(run.f->scene.GetWorld(), s.enemyBehind, Tags::BitOf(4));
                        },
                        chosen, status);
                ck.Check(status == kSucceeded && chosen == ids.enemyFar, "FindTarget: タグはマスクのどれか 1 ビットを持てばよい (AND)");
                runFind(L"ai_ft_inactive", FindTargetNode(0, 15.0, true, false, false, 0, "Target"), ids,
                        [](AiRun& run, Scene13& s) {
                            World& world = run.f->scene.GetWorld();
                            for (const EntityID e : { s.enemyBehind, s.enemyTieFront }) {
                                world.AddComponent<ActiveComponent>(e)->enabled = false;
                            }
                        },
                        chosen, status);
                ck.Check(status == kSucceeded && chosen == ids.enemyFar, "FindTarget: 無効なエンティティは選ばない");
                runFind(L"ai_ft_noperception", FindTargetNode(0, 15.0, true, true, true, 0, "Target"), ids,
                        [](AiRun& run, Scene13&) { run.f->scene.GetWorld().RemoveComponent<AIPerceptionComponent>(run.f->walker); }, chosen, status);
                ck.Check(status == kFailed, "FindTarget: 自分に AIPerception が無ければ態度が決まらず Failure");
            }

            // ---- SearchArea ----
            {
                // 点ごとに Agent の目的地を記録しながら、status が until になるまで回す
                struct Tour {
                    std::vector<std::array<float, 3>> destinations;
                    int32_t finalStatus = 0;
                    uint64_t endedAt = 0;
                    bool destinationCleared = false;
                };
                const auto tour = [&](AiRun& run, int32_t until, int maxTicks, Tour& out) {
                    NavSim& sim = *run.sim;
                    for (int i = 0; i < maxTicks; ++i) {
                        sim.Step();
                        const NavMeshAgentComponent* agent = sim.Agent(run.f->walker);
                        const int32_t status = sim.Comp(run.f->walker)->status;
                        if (agent->hasDestination) {
                            const std::array<float, 3> dest = { agent->destination.x, agent->destination.y, agent->destination.z };
                            if (out.destinations.empty() || out.destinations.back() != dest) {
                                out.destinations.push_back(dest);
                            }
                        }
                        if (status == until) {
                            out.finalStatus = status;
                            out.endedAt = sim.tick - 1;
                            out.destinationCleared = !agent->hasDestination;
                            return;
                        }
                    }
                    out.finalStatus = sim.Comp(run.f->walker)->status;
                };
                {
                    AiRun run;
                    makeRun(RegisterTree(lib, L"ai_search_tour", Tree(0, { Search(0, "Last", nullptr, false, 4.0, 3) }, aiBoard)), 90.0f, run);
                    Tour t;
                    run.sim->Step(); // 最初の tick は Surface が未読み込みで Failure になる (Nav の Update の前)
                    tour(run, kFailed, 2400, t);
                    bool within = t.destinations.size() == 4;
                    for (size_t i = 1; i < t.destinations.size(); ++i) {
                        within = within && std::hypot(t.destinations[i][0] - 6.0f, t.destinations[i][2] - 6.0f) <= 4.6f;
                    }
                    MYE_LOG_INFO("  [search] destinations %zu, ended at tick %llu", t.destinations.size(), static_cast<unsigned long long>(t.endedAt));
                    ck.Check(t.finalStatus == kFailed && t.destinationCleared && within && !t.destinations.empty()
                                 && std::hypot(t.destinations[0][0] - 6.0f, t.destinations[0][2] - 6.0f) < 0.6f,
                             "SearchArea: 起点へ向かい、半径内の点を pointCount 個順に回り、回り切ったら Failure (目的地は倒れる)");
                }
                const uint64_t seekTree = RegisterTree(lib, L"ai_search_seek", Tree(0, { Search(0, "Last", "Quarry", false, 4.0, 3) }, aiBoard));
                const auto makeSeeker = [&](AiRun& run, bool quarrySet) {
                    makeRun(seekTree, 360.0f, run);
                    AIPerceptionComponent* perception = run.f->scene.GetWorld().GetComponent<AIPerceptionComponent>(run.f->walker);
                    perception->sightRadius = 8.0f;
                    perception->loseSightRadius = 9.0f;
                    const EntityID quarry = addStimulus(run.f->scene, 6.0f, 9.0f, 0, "Quarry");
                    run.f->scene.GetWorld().ApplyStructuralChanges();
                    run.sim->Step();
                    if (quarrySet) {
                        setBbEntity(*run.sim, run.f->walker, "Quarry", quarry);
                    }
                };
                {
                    AiRun run;
                    makeSeeker(run, true);
                    Tour t;
                    tour(run, kSucceeded, 900, t);
                    const LocalTransform* at = run.sim->Pos(run.f->walker);
                    ck.Check(t.finalStatus == kSucceeded && t.destinationCleared && at->position.x < 5.5f,
                             "SearchArea: 終了キーの相手が視覚で見えたら、起点へ着く前でも Success (目的地は倒れる)");
                }
                {
                    AiRun run;
                    makeSeeker(run, false);
                    Tour t;
                    tour(run, kSucceeded, 100, t);
                    ck.Check(t.finalStatus == kRunning, "SearchArea: 終了キーが未設定なら見えても終わらず、そのまま探し続ける");
                }
                {
                    // 予測位置の吸着: 起点キーは使わず、終了キーの相手の predictedPos (ナビメッシュの上空) を床へ吸着して向かう
                    const uint64_t predictTree =
                        RegisterTree(lib, L"ai_search_predict", Tree(0, { Search(0, "Center", "Quarry", true, 3.0, 2) }, aiBoard));
                    for (int withPercept = 0; withPercept < 2; ++withPercept) {
                        AiRun run;
                        makeRun(predictTree, 90.0f, run);
                        run.sim->perceive = false;
                        const EntityID quarry = addStimulus(run.f->scene, -10.0f, -10.0f, 0, "Quarry");
                        run.f->scene.GetWorld().ApplyStructuralChanges();
                        run.sim->Step();
                        setBbEntity(*run.sim, run.f->walker, "Quarry", quarry);
                        if (withPercept != 0) {
                            AIPerceptionComponent* perception = run.f->scene.GetWorld().GetComponent<AIPerceptionComponent>(run.f->walker);
                            AIPercept& percept = perception->percepts[0];
                            percept.target = quarry;
                            percept.lastSenses = perceptionsense::kSight;
                            percept.lastSensedPos = { 6.0f, 3.0f, 6.0f };
                            percept.predictedPos = { 6.0f, 3.0f, 6.0f };
                            perception->perceivedCount = 1;
                        }
                        float destination[3] = {};
                        bool written = false;
                        for (int i = 0; i < 20 && !written; ++i) {
                            run.sim->Step();
                            const NavMeshAgentComponent* agent = run.sim->Agent(run.f->walker);
                            if (agent->hasDestination) {
                                written = true;
                                destination[0] = agent->destination.x;
                                destination[1] = agent->destination.y;
                                destination[2] = agent->destination.z;
                            }
                        }
                        const float expectX = withPercept != 0 ? 6.0f : -6.0f;
                        ck.Check(written && std::hypot(destination[0] - expectX, destination[2] - 6.0f) < 0.6f && destination[1] < 1.0f,
                                 withPercept != 0 ? "SearchArea: usePrediction は predictedPos を起点にし、ナビメッシュの床へ吸着する (上空 3 m の予測位置が床に降りる)"
                                                  : "SearchArea: usePrediction でも終了キーの相手の知覚が無ければ起点キーを使う");
                    }
                }
                {
                    const auto failsFast = [&](const wchar_t* name, const char* origin, const char* label) {
                        AiRun run;
                        makeRun(RegisterTree(lib, name, Tree(0, { Search(0, origin, nullptr, false, 4.0, 3) }, aiBoard)), 90.0f, run);
                        bool everWalked = false;
                        for (int i = 0; i < 10; ++i) {
                            run.sim->Step();
                            everWalked = everWalked || run.sim->Agent(run.f->walker)->hasDestination;
                        }
                        ck.Check(!everWalked && run.sim->Comp(run.f->walker)->status == kFailed, label);
                    };
                    failsFast(L"ai_search_unset", "Unset", "SearchArea: 起点のキーが未設定なら歩かず Failure");
                    failsFast(L"ai_search_far", "FarOrigin", "SearchArea: 起点がナビメッシュの外 (吸着できない) なら歩かず Failure");
                }
                {
                    // Abort (Timeout) で Agent が止まる
                    const uint64_t guid = RegisterTree(
                        lib, L"ai_search_timeout",
                        Tree(0, { Decorated(Search(0, "Last", nullptr, false, 4.0, 3), { Timeout(90) }) }, aiBoard));
                    AiRun run;
                    makeRun(guid, 90.0f, run);
                    Tour t;
                    run.sim->Step(); // 最初の tick は Surface が未読み込みで Failure になる
                    tour(run, kFailed, 300, t);
                    const BtInstance* inst = run.sim->bt->FindInstance(run.f->walker);
                    const bool extraZero = inst != nullptr && std::all_of(inst->extra.begin(), inst->extra.end(), [](uint8_t b) { return b == 0; });
                    ck.Check(t.finalStatus == kFailed && t.destinationCleared && extraZero && t.endedAt > 80 && t.endedAt < 150,
                             "SearchArea: Timeout で Abort されると Agent の目的地を倒し、追加状態も 0 へ戻す");
                }
                {
                    const uint64_t guid = RegisterTree(lib, L"ai_search_snap", Tree(0, { Search(0, "Last", nullptr, false, 5.0, 6) }, aiBoard));
                    const auto searchState = [](const BtInstance& inst) {
                        BtSearchAreaState st;
                        std::memcpy(&st, inst.extra.data(), sizeof(st));
                        return st;
                    };
                    roundTrip("SearchArea の途中 (点を向かっている間・残り 3) で保存 → 復元 → 連続実行と毎 tick のハッシュが一致 (RNG を含む)", guid, false,
                              [&](const BtInstance& inst, uint64_t) {
                                  return inst.nodes[0].active != 0 && searchState(inst).phase == btsearchphase::kToPoint
                                         && searchState(inst).remaining == 3;
                              });
                    roundTrip("SearchArea の途中で保存 → 新しい BehaviorTreeSystem へ復元 → 連続実行と毎 tick のハッシュが一致", guid, true,
                              [&](const BtInstance& inst, uint64_t) {
                                  return inst.nodes[0].active != 0 && searchState(inst).phase == btsearchphase::kToPoint
                                         && searchState(inst).remaining == 2;
                              });
                    roundTrip("SearchArea の起点へ向かっている間で保存 → 復元 → 連続実行と毎 tick のハッシュが一致", guid, false,
                              [&](const BtInstance& inst, uint64_t tick) {
                                  return tick > 30 && inst.nodes[0].active != 0 && searchState(inst).phase == btsearchphase::kToOrigin;
                              });
                }
            }
        }
    }

    // ---- 10. 汎用イベントキューと SendEvent (M85e) ----
    {
        // キー: 0 Alert(Bool, alert) / 1 Who(Entity, alert) / 2 Count(Int, count) / 3 Pos(Vector, ping) / 4 Power(Float, ping)
        //       5 Quiet(Int, eventName なし) / 6 Peer(Entity) / 7 Dir(Vector)
        enum { kAlert = 0, kWho = 1, kCount = 2, kPos = 3, kPower = 4, kQuiet = 5, kPeer = 6, kDir = 7 };
        const json boardJson = Board({ WithEventName(BbKey("Alert", "Bool"), "alert"), WithEventName(BbKey("Who", "Entity"), "alert"),
                                       WithEventName(BbKey("Count", "Int"), "count"), WithEventName(BbKey("Pos", "Vector"), "ping"),
                                       WithEventName(BbKey("Power", "Float"), "ping"), BbKey("Quiet", "Int"), BbKey("Peer", "Entity"),
                                       BbKey("Dir", "Vector") });
        const uint64_t board = RegisterBoard(lib, L"events", boardJson);
        const uint64_t idle = RegisterTree(lib, L"ev_idle", Tree(0, { Wait(0, 1000) }, board));
        static constexpr float kNoVector[3] = {};
        const auto bbOf = [](Sim& sim, EntityID e, int key) -> const BbValue& { return sim.bt.FindInstance(e)->blackboard[static_cast<size_t>(key)]; };
        const auto toEntity = [&](Sim& sim, EntityID sender, EntityID target, const char* name, int intValue, float value = 0.0f,
                                  const float (&vec)[3] = kNoVector) {
            return sim.bt.SendEvent(sim.tick, sender, target, BtEventNameHash(name), vec, value, intValue);
        };

        // JSON: eventName の往復と防波堤
        {
            BlackboardAsset a;
            BlackboardAsset b;
            ck.Check(BlackboardLibrary::FromJson(boardJson, a) && a.keys[kAlert].eventName == "alert" && a.keys[kQuiet].eventName.empty()
                         && BlackboardLibrary::FromJson(BlackboardLibrary::ToJson(a), b) && BlackboardLibrary::ToJson(a) == BlackboardLibrary::ToJson(b),
                     "ブラックボードの eventName が往復で変わらない");
            const json sendTree = Tree(0, { SendEv(0, "ping", "Entity", 3, 1.5, "Peer", "Dir") }, board);
            BehaviorTreeAsset ta;
            BehaviorTreeAsset tb;
            ck.Check(BehaviorTreeLibrary::FromJson(sendTree, ta) && BehaviorTreeLibrary::FromJson(BehaviorTreeLibrary::ToJson(ta), tb)
                         && BehaviorTreeLibrary::ToJson(ta) == BehaviorTreeLibrary::ToJson(tb)
                         && ta.nodes[0].params[btsendparam::kEventName].s == "ping"
                         && ta.nodes[0].params[btsendparam::kTarget].i == btsendtarget::kEntity && ta.nodes[0].keys[btsendkey::kVector] == "Dir",
                     "SendEvent の JSON が往復で変わらない (イベント名は文字列パラメータ)");
            ck.Check(!Loads(Tree(0, { SendEv(0, std::string(kBbMaxNameBytes + 1, 'x').c_str(), "All") })), "長すぎるイベント名は読み込み失敗");
            ck.Check(!Loads(Tree(0, { SendEv(0, "ping", "Everyone") })), "未知の宛先は読み込み失敗");
        }

        // tick N に積んだ分は tick N+1 に届く (BT フェーズより前 = スクリプト層で積んでも、後で積んでも)
        {
            Sim sim;
            const EntityID e1 = sim.AddTreeEntity(idle, "E1");
            const EntityID e2 = sim.AddTreeEntity(idle, "E2");
            const EntityID s = sim.AddTreeEntity(idle, "S");
            sim.Step();
            const float v[3] = { 1.0f, 2.0f, 3.0f };
            toEntity(sim, s, e1, "ping", 7, 2.5f, v); // tick 2 の BT フェーズより前
            sim.Step();                               // tick 2
            const bool notYet = bbOf(sim, e1, kPos).isSet == 0 && sim.bt.EventCount(e1) == 0 && sim.bt.PendingEventCount() == 1;
            sim.Step(); // tick 3
            const BbValue& pos = bbOf(sim, e1, kPos);
            BtEvent got;
            const bool read = sim.bt.GetEvent(e1, 0, got);
            ck.Check(notYet && pos.isSet == 1 && pos.v[0] == 1.0f && pos.v[1] == 2.0f && pos.v[2] == 3.0f && bbOf(sim, e1, kPower).f == 2.5f
                         && bbOf(sim, e2, kPos).isSet == 0 && sim.bt.EventCount(e1) == 1 && sim.bt.EventCount(e2) == 0 && read
                         && got.sender == s && got.intValue == 7 && sim.bt.PendingEventCount() == 0,
                     "tick N に (BT フェーズより前で) 積んだ分は tick N+1 に宛先だけへ届き、Vector / Float が BB へ入る");
            sim.Step(); // tick 4
            ck.Check(sim.bt.EventCount(e1) == 0 && bbOf(sim, e1, kPos).isSet == 1, "配った分は次の tick で捨てられ、BB の値は残る");
            const float none[3] = {};
            sim.bt.SendEvent(sim.LastTick(), s, e1, BtEventNameHash("count"), none, 0.0f, 11); // tick 4 の BT フェーズの後
            sim.Step();                                                                         // tick 5
            ck.Check(bbOf(sim, e1, kCount).isSet == 1 && bbOf(sim, e1, kCount).i == 11, "BT フェーズの後で積んだ分も次の tick に届く");
        }

        // 配達は tick の頭: BT より前の層 (スクリプト) から読め、同じ tick に BB へも入る。一時停止中は配らない
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(idle, "E");
            const EntityID s = sim.AddTreeEntity(idle, "S");
            sim.Step();
            int seenBeforeBt = -1;
            BtEvent seen;
            bool readOk = false;
            bool bbSetBeforeBt = true;
            sim.beforeBt = [&] {
                seenBeforeBt = sim.bt.EventCount(e);
                readOk = sim.bt.GetEvent(e, 0, seen);
                bbSetBeforeBt = bbOf(sim, e, kCount).isSet != 0;
            };
            toEntity(sim, s, e, "count", 5); // tick 2
            sim.Step();                      // tick 2: まだ読めない
            const bool notYet = seenBeforeBt == 0 && !readOk;
            sim.Step();                      // tick 3: BT より前で読める。BB はまだ (反映は BT フェーズ)
            const bool readEarly = seenBeforeBt == 1 && readOk && seen.sender == s && seen.intValue == 5 && !bbSetBeforeBt;
            ck.Check(notYet && readEarly && bbOf(sim, e, kCount).i == 5,
                     "tick N に送った分を tick N+1 の BT より前の層から読め、同じ tick の BT フェーズで BB へも反映される");
            sim.beforeBt = nullptr;

            toEntity(sim, s, e, "count", 8); // tick 4 に積む
            sim.paused = true;
            sim.Step();                      // tick 4: 一時停止
            sim.Step();                      // tick 5: 一時停止 (配達しない)
            const bool heldWhilePaused = sim.bt.PendingEventCount() == 1 && bbOf(sim, e, kCount).i == 5;
            sim.paused = false;
            sim.Step();                      // tick 6: 再開、配送待ちが配られる
            ck.Check(heldWhilePaused && bbOf(sim, e, kCount).i == 8 && sim.bt.PendingEventCount() == 0,
                     "一時停止中の tick は配達せず、配送待ちはそのまま残って再開した tick に配られる");
        }

        // 型ごとの書き方: Bool = true / Entity = sender、同じイベント名のキーは全部書く、eventName の無いキーは書かない
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(idle, "E");
            const EntityID s = sim.AddTreeEntity(idle, "S");
            sim.Step();
            toEntity(sim, s, e, "alert", 0);
            sim.Step();
            sim.Step();
            ck.Check(bbOf(sim, e, kAlert).isSet == 1 && bbOf(sim, e, kAlert).i == 1 && bbOf(sim, e, kWho).isSet == 1 && bbOf(sim, e, kWho).entity == s
                         && bbOf(sim, e, kQuiet).isSet == 0,
                     "Bool = true、Entity = 送信元。同じ eventName のキーは両方書き、eventName の無いキーは触らない");
        }

        // 配送順 = 送信元キー → 送信順。同じキーへは最後が勝つ
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(idle, "E");
            const EntityID a = sim.AddTreeEntity(idle, "A");
            const EntityID b = sim.AddTreeEntity(idle, "B");
            sim.Step();
            ck.Check(a.index < b.index, "(前提) A のキーは B より小さい");
            toEntity(sim, b, e, "count", 20); // 送信順は B が先
            toEntity(sim, a, e, "count", 10);
            toEntity(sim, a, e, "count", 11);
            sim.Step();
            sim.Step();
            BtEvent e0;
            BtEvent e1;
            BtEvent e2;
            const bool read = sim.bt.GetEvent(e, 0, e0) && sim.bt.GetEvent(e, 1, e1) && sim.bt.GetEvent(e, 2, e2) && !sim.bt.GetEvent(e, 3, e0);
            ck.Check(read && e0.sender == a && e0.intValue == 10 && e1.sender == a && e1.intValue == 11 && e2.sender == b && e2.intValue == 20,
                     "配送順は送信元の entity キー、同じ送信元では送信順 (B が先に送っても A の 2 件が先)");
            ck.Check(bbOf(sim, e, kCount).i == 20, "同じ tick の複数は配送順の最後が勝つ (送信順の最後ではなく B の 20)");
        }

        // 宛先: 自分宛ては宛先だけ、全体宛ては全員 (送信元自身を含む)
        {
            Sim sim;
            const EntityID e1 = sim.AddTreeEntity(idle, "E1");
            const EntityID e2 = sim.AddTreeEntity(idle, "E2");
            sim.Step();
            toEntity(sim, e1, kNullEntity, "count", 2); // 全体宛て
            toEntity(sim, e2, e1, "count", 5);          // E1 だけ
            sim.Step();
            sim.Step();
            ck.Check(sim.bt.EventCount(e1) == 2 && sim.bt.EventCount(e2) == 1 && bbOf(sim, e2, kCount).i == 2 && bbOf(sim, e1, kCount).i == 5,
                     "全体宛ては宛先を問わず届き、個別宛ては宛先だけに届く (E1: 全体 + 個別 = 2 件、E2: 全体 1 件)");
        }

        // 上限: 256 件で溢れた分を捨て、警告は最初の 1 回だけ
        {
            Sim sim;
            const EntityID e = sim.AddTreeEntity(idle, "E");
            sim.Step();
            const uint64_t logFrom = logging::TotalWritten();
            int accepted = 0;
            for (int i = 0; i < kBtMaxEventsPerTick + 44; ++i) {
                accepted += toEntity(sim, e, kNullEntity, "count", i) ? 1 : 0;
            }
            sim.Step();
            sim.Step();
            const int delivered = sim.bt.EventCount(e);
            for (int i = 0; i < kBtMaxEventsPerTick + 1; ++i) {
                toEntity(sim, e, kNullEntity, "count", i);
            }
            ck.Check(accepted == kBtMaxEventsPerTick && delivered == kBtMaxEventsPerTick && bbOf(sim, e, kCount).i == kBtMaxEventsPerTick - 1
                         && sim.bt.PendingEventCount() == kBtMaxEventsPerTick && CountWarnings(logFrom, "event queue") == 1,
                     "1 tick の上限は 256 件で、溢れた分は捨てて警告は 1 回だけ (最初の 256 件が残る)");
        }

        // BB 反映でも LowerPriority の Abort が起きる (反映は監視より前なので、届いた tick に同じ tick で切り替わる)
        {
            Sim sim;
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            const uint64_t tree = RegisterTree(
                lib, L"ev_abort",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 100), { BbCond("Alert", "IsSet", "LowerPriority") }), Wait(2, 100) }, board));
            const EntityID e = sim.AddTreeEntity(tree, "E");
            const EntityID s = sim.AddTreeEntity(idle, "S");
            sim.Step();
            sim.Step();
            const int32_t before = sim.Comp(e)->activeNodeId;
            toEntity(sim, s, e, "alert", 0);
            sim.Step(); // 積んだ tick: まだ右
            const int32_t sameTick = sim.Comp(e)->activeNodeId;
            const bool noAbortYet = trace.empty();
            sim.Step(); // 次の tick: 届いて BB が書かれ、同じ tick の監視で右を Abort
            ck.Check(before == 2 && sameTick == 2 && noAbortYet && sim.Comp(e)->activeNodeId == 1 && trace == std::vector<int32_t>{ 2 },
                     "イベントが届いて BB が書かれると、その tick の監視で LowerPriority の Abort が起きる (積んだ tick ではまだ)");
        }

        // SendEvent ノード
        {
            const uint64_t allTree = RegisterTree(lib, L"ev_send_all",
                                                  Tree(0, { Node(0, "Sequence", { 1, 2 }), SendEv(1, "count", "All", 42), Wait(2, 1000) }, board));
            Sim sim;
            const EntityID s = sim.AddTreeEntity(allTree, "S");
            const EntityID r = sim.AddTreeEntity(idle, "R");
            sim.Step();
            const bool sentOnly = sim.bt.PendingEventCount() == 1 && bbOf(sim, r, kCount).isSet == 0;
            sim.Step();
            ck.Check(sentOnly && bbOf(sim, r, kCount).i == 42 && bbOf(sim, s, kCount).i == 42 && sim.bt.PendingEventCount() == 0
                         && sim.Comp(s)->activeNodeId == 2,
                     "SendEvent (全体宛て): 積んで即 Success、次の tick に全員の BB へ届く");
        }
        {
            const uint64_t selfTree = RegisterTree(lib, L"ev_send_self",
                                                   Tree(0, { Node(0, "Sequence", { 1, 2 }), SendEv(1, "count", "Self", 9), Wait(2, 1000) }, board));
            Sim sim;
            const EntityID s = sim.AddTreeEntity(selfTree, "S");
            const EntityID r = sim.AddTreeEntity(idle, "R");
            sim.Step();
            sim.Step();
            ck.Check(bbOf(sim, s, kCount).i == 9 && bbOf(sim, r, kCount).isSet == 0, "SendEvent (自分宛て): 自分だけに届く");
        }
        {
            const uint64_t entityTree = RegisterTree(
                lib, L"ev_send_entity",
                Tree(0, { Node(0, "Sequence", { 1, 2, 3 }), Wait(1, 3), SendEv(2, "ping", "Entity", 0, 2.5, "Peer", "Dir"), Wait(3, 1000) }, board));
            Sim sim;
            const EntityID s = sim.AddTreeEntity(entityTree, "S");
            const EntityID r = sim.AddTreeEntity(idle, "R");
            const EntityID other = sim.AddTreeEntity(idle, "Other");
            sim.Step();
            sim.Mutable(s)->blackboard[kPeer] = BbValue{ 1, 0, 0.0f, { 0.0f, 0.0f, 0.0f }, r };
            sim.Mutable(s)->blackboard[kDir] = BbValue{ 1, 0, 0.0f, { 4.0f, 5.0f, 6.0f }, kNullEntity };
            for (int i = 0; i < 12; ++i) {
                sim.Step();
            }
            const BbValue& pos = bbOf(sim, r, kPos);
            ck.Check(pos.isSet == 1 && pos.v[0] == 4.0f && pos.v[1] == 5.0f && pos.v[2] == 6.0f && bbOf(sim, r, kPower).f == 2.5f
                         && bbOf(sim, other, kPos).isSet == 0 && bbOf(sim, s, kPos).isSet == 0,
                     "SendEvent (Entity 宛て): Entity キーの相手だけに、Vector キーの値と Float 定数が届く");
        }
        {
            // Failure: 名前が空 / Entity キーが未設定 / Vector キーを指定して未設定
            const auto failsWith = [&](const json& send, const wchar_t* name) {
                Sim sim;
                const EntityID e = sim.AddTreeEntity(RegisterTree(lib, name, Tree(0, { send }, board)), "E");
                sim.Step();
                return sim.Status(e) == kFailed && sim.bt.PendingEventCount() == 0;
            };
            ck.Check(failsWith(SendEv(0, "", "All"), L"ev_fail_name") && failsWith(SendEv(0, "ping", "Entity", 0, 0.0, "Peer"), L"ev_fail_peer")
                         && failsWith(SendEv(0, "ping", "All", 0, 0.0, nullptr, "Dir"), L"ev_fail_dir"),
                     "SendEvent: 名前が空・宛先の Entity が未設定・指定した Vector キーが未設定は Failure で何も積まない");
        }

        // 配送待ちが空ならハッシュは変わらない / 配送待ちがあるとき表が空でもハッシュに入る
        {
            Sim quiet;
            Sim noisy;
            quiet.AddTreeEntity(idle, "E");
            const EntityID e = noisy.AddTreeEntity(idle, "E");
            quiet.Step();
            noisy.Step();
            const uint64_t baseline = quiet.bt.StateHash();
            ck.Check(baseline == noisy.bt.StateHash(), "(前提) 同じ木・同じ tick の 2 つの表は同じハッシュ");
            noisy.bt.SendEvent(noisy.LastTick(), e, kNullEntity, BtEventNameHash("unmapped"), kNoVector, 0.0f, 0);
            const bool pendingChanges = noisy.bt.StateHash() != baseline;
            quiet.Step();
            noisy.Step();
            ck.Check(pendingChanges && quiet.bt.StateHash() == noisy.bt.StateHash(),
                     "配送待ちはハッシュに入り、配り終えて空に戻ればイベントの無い表とハッシュが一致する");
            Sim empty;
            ck.Check(!empty.bt.HasHashableState(), "(前提) 木もイベントも無ければ畳むものが無い");
            empty.bt.SendEvent(empty.tick, kNullEntity, kNullEntity, BtEventNameHash("x"), kNoVector, 0.0f, 0);
            const bool whilePending = empty.bt.HasHashableState();
            empty.Step();
            empty.Step();
            ck.Check(whilePending && !empty.bt.HasHashableState(), "木が無くても配送待ちの間は畳み、配り終えれば何も畳まない");
        }

        // 配送待ちの途中で保存 → 復元 → 連続実行と毎 tick のハッシュが一致 (配った分は保存しない)
        {
            const uint64_t reactive = RegisterTree(
                lib, L"ev_reactive",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 400), { BbCond("Alert", "IsSet", "LowerPriority") }), Wait(2, 400) }, board));
            const uint64_t talker = RegisterTree(
                lib, L"ev_talker", Tree(0, { Node(0, "Sequence", { 1, 2, 3 }), Wait(1, 8), SendEv(2, "alert", "All", 3), Wait(3, 1000) }, board));
            Scene scene;
            BehaviorTreeSystem first;
            GameObject receiverObject = scene.CreateGameObjectTracked("Receiver");
            receiverObject.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ reactive };
            GameObject talkerObject = scene.CreateGameObjectTracked("Talker");
            talkerObject.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ talker };
            const EntityID receiver = receiverObject.Id();
            scene.GetWorld().ApplyStructuralChanges();

            SimRefs refs;
            refs.scene = &scene;
            refs.behaviorTree = &first;
            uint64_t tickRef = 0;
            refs.tickIndex = &tickRef;
            uint64_t tick = 1;
            const auto step = [&](BehaviorTreeSystem& bt) {
                bt.DeliverPending(tick);
                bt.Update(scene.GetWorld(), tick, nullptr);
                scene.GetWorld().ApplyStructuralChanges();
                ++tick;
            };
            for (int i = 0; i < 60 && first.PendingEventCount() == 0; ++i) {
                step(first);
            }
            tickRef = tick;
            std::vector<std::byte> blob;
            const bool captured = CaptureSimSnapshot(refs, blob) && first.PendingEventCount() == 1;
            ck.Check(captured, "(前提) イベントが配送待ちの tick 末で撮影できる");

            const uint64_t startTick = tick;
            std::vector<uint64_t> continuous;
            std::vector<int32_t> activeAfter;
            for (int i = 0; i < 12; ++i) {
                step(first);
                continuous.push_back(HashWorld(scene.GetWorld(), refs.HashSources()));
                activeAfter.push_back(scene.GetWorld().GetComponent<BehaviorTreeComponent>(receiver)->activeNodeId);
            }
            ck.Check(activeAfter.front() == 1 && first.PendingEventCount() == 0,
                     "(前提) 連続実行では次の tick にイベントが届いて Abort が起き、配送待ちが空になる");

            BehaviorTreeSystem second;
            SimRefs refsSecond = refs;
            refsSecond.behaviorTree = &second;
            ck.Check(RestoreSimSnapshot(refsSecond, blob.data(), blob.size()) && second.PendingEventCount() == 1 && second.EventCount(receiver) == 0,
                     "配送待ちが復元され、配った分は空");
            tick = startTick;
            bool same = true;
            bool sameActive = true;
            for (int i = 0; i < 12; ++i) {
                step(second);
                same = same && HashWorld(scene.GetWorld(), refsSecond.HashSources()) == continuous[static_cast<size_t>(i)];
                sameActive = sameActive && scene.GetWorld().GetComponent<BehaviorTreeComponent>(receiver)->activeNodeId == activeAfter[static_cast<size_t>(i)];
            }
            ck.Check(same && sameActive && second.StateHash() == first.StateHash(),
                     "配送待ちで保存 → 復元 → 12 tick の毎 tick のハッシュと実行中ノードが連続実行と一致 (イベントの BB 反映と Abort を含む)");
        }

        // 壊れた配送待ちの節は拒否する
        {
            const auto pendingBlob = [](uint64_t count, uint32_t seqOffset, uint32_t eventCount = 0) {
                std::vector<std::byte> out;
                ByteWriter w(out);
                w.Count(0); // 木の表は空
                w.Count(count);
                for (uint32_t i = 0; i < eventCount; ++i) {
                    w.U64(1);
                    w.U32(1);
                    w.U32(0);
                    w.U32(0);
                    w.U32(0);
                    for (int f = 0; f < 4; ++f) {
                        w.F32(0.0f);
                    }
                    w.I32(0);
                    w.U32(i + seqOffset);
                    w.U64(1);
                }
                return out;
            };
            const auto accepts = [](const std::vector<std::byte>& data) {
                ByteReader r(data.data(), data.size());
                BtSnapshot parsed;
                return BehaviorTreeSystem::ReadSnapshot(r, parsed);
            };
            ck.Check(accepts(pendingBlob(2, 0, 2)), "(前提) 作った配送待ちは読める");
            ck.Check(!accepts(pendingBlob(2, 1, 2)), "seq が 0 から連番でない配送待ちは拒否する");
            ck.Check(!accepts(pendingBlob(2, 0, 1)), "件数より中身が短い配送待ちは拒否する");
            ck.Check(!accepts(pendingBlob(static_cast<uint64_t>(kBtMaxEventsPerTick) + 1, 0, kBtMaxEventsPerTick + 1)),
                     "上限を超える件数の配送待ちは拒否する");
        }
    }

    // ---- 13. PlayAnimation と SubTree (M85f) ----
    {
        // アセット: 往復・防波堤
        {
            BehaviorTreeAsset a;
            BehaviorTreeAsset b;
            const json j = Tree(0, { Node(0, "Sequence", { 1, 2 }), PlayAnim(1, "Slash", 6, true), SubTreeNode(2, 0xABCDull) });
            ck.Check(BehaviorTreeLibrary::FromJson(j, a) && BehaviorTreeLibrary::FromJson(BehaviorTreeLibrary::ToJson(a), b)
                         && BehaviorTreeLibrary::ToJson(a) == BehaviorTreeLibrary::ToJson(b)
                         && a.nodes[1].params[btplayparam::kState].s == "Slash" && a.nodes[1].params[btplayparam::kDurationTicks].i == 6
                         && a.nodes[1].params[btplayparam::kWaitForEnd].i == 1 && a.nodes[2].params[btsubtreeparam::kTree].u == 0xABCDull,
                     "PlayAnimation / SubTree の JSON が往復で変わらない");
            ck.Check(!Loads(Tree(0, { Node(0, "SubTree", { 1 }, json{ { "tree", GuidHex(1) } }), Wait(1, 3) })),
                     "ファイルの SubTree に子を付けると読み込み失敗 (子は実行用の木にだけ付く)");
            ck.Check(!Loads(Tree(0, { Node(0, "PlayAnimation", {}, json{ { "state", std::string(kBbMaxNameBytes + 1, 'x') } }) })),
                     "長すぎるステート名は読み込み失敗");
        }

        // ---- PlayAnimation ----
        // ステート 0 Idle (60 tick) / 1 Slash (20 tick, 等速) / 2 Fast (20 tick, 2 倍速 = 10 tick) / 3 Bare (クリップなし)
        AnimationLibrary clipLib;
        const auto makeClip = [&](const wchar_t* path, int32_t length) {
            AnimationClipAsset clip;
            clip.lengthTicks = length;
            return clipLib.Register(path, clip);
        };
        const uint64_t idleClip = makeClip(L"selftest\\ai\\idle.anim.json", 60);
        const uint64_t slashClip = makeClip(L"selftest\\ai\\slash.anim.json", 20);
        ControllerLibrary ctrlLib;
        ControllerAsset controller;
        controller.states.push_back({ "Idle", "", idleClip, 1, 1 });
        controller.states.push_back({ "Slash", "", slashClip, 1, 1 });
        controller.states.push_back({ "Fast", "", slashClip, 2, 1 });
        controller.states.push_back({ "Bare", "", 0, 1, 1 });
        const uint64_t ctrlHash = ctrlLib.Register(L"selftest\\ai\\anim.controller.json", controller);
        AnimatorControllerSystem animatorSystem;
        const auto wire = [&](Sim& sim) {
            sim.controllers = &ctrlLib;
            sim.clips = &clipLib;
            sim.animator = &animatorSystem;
        };
        const auto addActor = [&](Sim& sim, uint64_t tree, bool withAnimator) {
            GameObject go = sim.scene.CreateGameObjectTracked("Actor");
            go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ tree };
            if (withAnimator) {
                go.AddComponent<AnimatorControllerComponent>()->controller = AssetID{ ctrlHash };
            }
            sim.GetWorld().ApplyStructuralChanges();
            return go.Id();
        };
        const auto animOf = [](Sim& sim, EntityID e) { return sim.GetWorld().GetComponent<AnimatorControllerComponent>(e); };

        {
            Sim sim;
            wire(sim);
            const EntityID e = addActor(sim, RegisterTree(lib, L"play_now", Tree(0, { PlayAnim(0, "Slash", 0, false) })), true);
            sim.Step();
            ck.Check(sim.Status(e) == kSucceeded && animOf(sim, e)->currentState == 1 && animOf(sim, e)->transitionTo == -1,
                     "PlayAnimation (durationTicks 0, 待たない): 即切り替えて即 Success");
        }
        {
            Sim sim;
            wire(sim);
            // 後ろに Wait を付けて、Play を毎 tick やり直さないようにする
            const EntityID e = addActor(
                sim, RegisterTree(lib, L"play_blend", Tree(0, { Node(0, "Sequence", { 1, 2 }), PlayAnim(1, "Slash", 6, false), Wait(2, 1000) })), true);
            sim.Step();
            const AnimatorControllerComponent* animator = animOf(sim, e);
            ck.Check(sim.Comp(e)->activeNodeId == 2 && animator->currentState == 0 && animator->transitionTo == 1 && animator->transitionDuration == 6,
                     "PlayAnimation (durationTicks 6, 待たない): 遷移を始めて即 Success (次の兄弟へ進む)");
            for (int i = 0; i < 6; ++i) {
                sim.Step();
            }
            ck.Check(animator->currentState == 1 && animator->transitionTo == -1, "遷移は durationTicks 後に完了する");
        }
        {
            // waitForEnd: BT が Success を返す tick = クリップが 1 周して先頭へ戻った tick (loop のクリップでも 1 周)
            const auto lap = [&](const wchar_t* name, const char* state, int expectedTicks) {
                Sim sim;
                wire(sim);
                const EntityID e = addActor(sim, RegisterTree(lib, name, Tree(0, { PlayAnim(0, state, 0, true) })), true);
                std::vector<int32_t> timeBeforeBt;
                sim.beforeBt = [&] { timeBeforeBt.push_back(animOf(sim, e)->stateTimeTicks); };
                const std::vector<int32_t> statuses = Run(sim, e, expectedTicks + 2);
                const size_t finish = static_cast<size_t>(expectedTicks); // 0 始まりの添字 = expectedTicks + 1 tick 目
                bool runsUntil = true;
                for (size_t i = 0; i < finish; ++i) {
                    runsUntil = runsUntil && statuses[i] == kRunning;
                }
                return runsUntil && statuses[finish] == kSucceeded && statuses[finish + 1] == kRunning && timeBeforeBt[finish] == 0
                       && timeBeforeBt[finish - 1] != 0;
            };
            ck.Check(lap(L"play_lap", "Slash", 20),
                     "PlayAnimation (waitForEnd): 長さ 20 のクリップは入った tick から 20 tick 後に Success、その tick にクリップが先頭へ戻る");
            ck.Check(lap(L"play_lap_fast", "Fast", 10), "PlayAnimation (waitForEnd): 2 倍速のステートは 10 tick で 1 周");
        }
        {
            Sim sim;
            wire(sim);
            const EntityID e = addActor(sim, RegisterTree(lib, L"play_bare", Tree(0, { PlayAnim(0, "Bare", 0, true) })), true);
            sim.Step();
            ck.Check(sim.Status(e) == kSucceeded && animOf(sim, e)->currentState == 3,
                     "クリップの無いステートは待つものが無いので waitForEnd でも即 Success");
        }
        {
            // Failure: 名前が無い・空・Animator なし・controller を引けない構成。Animator の状態は何も変えない
            const auto failsAndKeeps = [&](const char* state, bool withAnimator, bool wired, const wchar_t* name) {
                Sim sim;
                if (wired) {
                    wire(sim);
                }
                const EntityID e = addActor(sim, RegisterTree(lib, name, Tree(0, { PlayAnim(0, state, 0, true) })), withAnimator);
                sim.Step();
                const AnimatorControllerComponent* animator = animOf(sim, e);
                return sim.Status(e) == kFailed
                       && (animator == nullptr || (animator->currentState == 0 && animator->transitionTo == -1 && animator->stateTimeTicks <= 1));
            };
            ck.Check(failsAndKeeps("Nope", true, true, L"play_fail_name") && failsAndKeeps("", true, true, L"play_fail_empty")
                         && failsAndKeeps("Slash", false, true, L"play_fail_noanim") && failsAndKeeps("Slash", true, false, L"play_fail_nolib"),
                     "PlayAnimation: ステート名が無い・空・Animator なし・controller の引き先なしは Failure で Animator を変えない");
        }

        // ---- SubTree ----
        // BB のキー: 0 N (Int) / 1 Alert (Bool) / 2 Run (Bool, 初期 true) / 3 Never (Bool)
        const uint64_t board = RegisterBoard(
            lib, L"sub_bb", Board({ BbKey("N", "Int"), BbKey("Alert", "Bool"), BbKey("Run", "Bool", true), BbKey("Never", "Bool") }));
        const uint64_t otherBoard = RegisterBoard(lib, L"sub_bb_other", Board({ BbKey("N", "Int") }));
        enum { kN = 0, kAlert = 1, kRun = 2 };
        const auto setN = [](int id, int value) { return SetBb(id, "N", "Constant", json{ { "intValue", value } }); };
        const auto bbOf = [](Sim& sim, EntityID e, int key) -> BbValue& { return sim.Mutable(e)->blackboard[static_cast<size_t>(key)]; };
        const auto waitIdOf = [](Sim& sim, EntityID e, int ticks) {
            for (const BtNodeDef& node : sim.bt.FindInstance(e)->tree->nodes) {
                if (node.kind == BtNodeKind::Wait && node.params[0].i == ticks) {
                    return node.id;
                }
            }
            return -1;
        };
        const auto guidOf = [](const wchar_t* name) { return BehaviorTreeLibrary::HashForPath(TreePath(name)); };

        {
            // BB の共有と、実行用の木が呼び出し側のノード表へ平らに取り込まれること
            const uint64_t sub = RegisterTree(lib, L"sub_set", Tree(0, { setN(0, 7) }, board));
            const uint64_t parent = RegisterTree(lib, L"sub_parent", Tree(0, { Node(0, "Sequence", { 1, 2 }), SubTreeNode(1, sub), Wait(2, 1000) }, board));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            sim.Step();
            const BtInstance* inst = sim.bt.FindInstance(e);
            ck.Check(sim.Status(e) == kRunning && inst->blackboard.size() == 4 && inst->blackboard[kN].isSet == 1 && inst->blackboard[kN].i == 7
                         && inst->tree->nodes.size() == 4 && lib.trees.Get(parent)->nodes.size() == 3,
                     "SubTree: 部分木は親の BB をそのまま使い (キーの数は親のまま)、書いた値が親から見える");
        }
        {
            // 結果の伝播 / SubTree ノードの Decorator
            const uint64_t failing = RegisterTree(lib, L"sub_fail", Tree(0, { Decorated(Wait(0, 1), { BbCond("Never", "IsSet") }) }, board));
            const uint64_t sub = guidOf(L"sub_set");
            const uint64_t selector = RegisterTree(lib, L"sub_sel", Tree(0, { Node(0, "Selector", { 1, 2 }), SubTreeNode(1, failing), setN(2, 3) }, board));
            const uint64_t guarded = RegisterTree(lib, L"sub_guard", Tree(0, { Decorated(SubTreeNode(0, sub), { BbCond("Never", "IsSet") }) }, board));
            Sim sim;
            const EntityID s = sim.AddTreeEntity(selector, "S");
            const EntityID g = sim.AddTreeEntity(guarded, "G");
            sim.Step();
            ck.Check(sim.Status(s) == kSucceeded && bbOf(sim, s, kN).i == 3, "SubTree: 部分木の Failure が親へ返り、次の兄弟へ進む");
            ck.Check(sim.Status(g) == kFailed && bbOf(sim, g, kN).isSet == 0, "SubTree ノード自身の Decorator が偽なら部分木に入らず Failure");
        }
        {
            // 同じ木を 2 か所で使っても状態は別 (3 tick + 3 tick)
            const uint64_t three = RegisterTree(lib, L"sub_three", Tree(0, { Wait(0, 3) }));
            const uint64_t twice = RegisterTree(lib, L"sub_twice", Tree(0, { Node(0, "Sequence", { 1, 2 }), SubTreeNode(1, three), SubTreeNode(2, three) }));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(twice);
            const std::vector<int32_t> got = Run(sim, e, 8);
            ck.Check(Is(got, { kRunning, kRunning, kRunning, kRunning, kRunning, kRunning, kSucceeded, kRunning }),
                     "SubTree: 同じ木を 2 か所で使うと別々の状態で順に動く (3 + 3 tick)");
        }
        {
            // 取り込めない SubTree は実行時 Failure。警告は取り込みのとき 1 回ずつ (体の数・tick の数に依らない)
            const uint64_t mismatch = RegisterTree(lib, L"sub_other", Tree(0, { setN(0, 5) }, otherBoard));
            const uint64_t noBoard = RegisterTree(lib, L"sub_noboard", Tree(0, { Wait(0, 0) }));
            const uint64_t parent = RegisterTree(
                lib, L"sub_unusable",
                Tree(0, { Node(0, "Selector", { 1, 2, 3, 4, 5 }), SubTreeNode(1, mismatch), SubTreeNode(2, noBoard), SubTreeNode(3, 0),
                          SubTreeNode(4, 0xDEADBEEFull), setN(5, 3) },
                     board));
            const uint64_t before = logging::TotalWritten();
            Sim sim;
            const EntityID a = sim.AddTreeEntity(parent, "A");
            const EntityID b = sim.AddTreeEntity(parent, "B");
            for (int i = 0; i < 3; ++i) {
                sim.Step();
            }
            ck.Check(bbOf(sim, a, kN).i == 3 && bbOf(sim, b, kN).i == 3 && CountWarnings(before, "SubTree cannot be used") == 4
                         && CountWarnings(before, "blackboard differs") == 2,
                     "SubTree: BB が違う・BB なしの子・未指定・未登録は Failure で、理由ごとに 1 回ずつだけ警告");
        }
        {
            // 入れ子の段数: 8 段までは動き、9 段目の SubTree は Failure + 警告
            const auto chain = [&](int length, const wchar_t* prefix) {
                const auto name = [&](int i) { return std::wstring(prefix) + L"_" + std::to_wstring(i); };
                for (int i = 0; i < length; ++i) {
                    RegisterTree(lib, name(i).c_str(), Tree(0, { SubTreeNode(0, guidOf(name(i + 1).c_str())) }));
                }
                RegisterTree(lib, name(length).c_str(), Tree(0, { Wait(0, 0) }));
                return guidOf(name(0).c_str());
            };
            const uint64_t ok8 = chain(kBtMaxSubTreeDepth, L"chain_ok");
            const uint64_t over9 = chain(kBtMaxSubTreeDepth + 1, L"chain_over");
            const uint64_t before = logging::TotalWritten();
            Sim sim;
            const EntityID a = sim.AddTreeEntity(ok8, "Ok");
            const EntityID b = sim.AddTreeEntity(over9, "Over");
            sim.Step();
            ck.Check(sim.Status(a) == kSucceeded && sim.bt.FindInstance(a)->tree->nodes.size() == static_cast<size_t>(kBtMaxSubTreeDepth) + 1,
                     "SubTree: 入れ子 8 段は最後まで取り込まれて Success");
            ck.Check(sim.Status(b) == kFailed && CountWarnings(before, "nested deeper") == 1, "SubTree: 9 段目は Failure + 警告 1 回");
        }
        {
            // 自分自身を含む循環は入れ子の上限で止まる (元の木 + 取り込み 8 回 = 9 個 x 2 ノード)
            const uint64_t self = guidOf(L"sub_rec");
            RegisterTree(lib, L"sub_rec", Tree(0, { Node(0, "Sequence", { 1 }), SubTreeNode(1, self) }));
            const uint64_t before = logging::TotalWritten();
            Sim sim;
            const EntityID e = sim.AddTreeEntity(self);
            for (int i = 0; i < 3; ++i) {
                sim.Step();
            }
            ck.Check(sim.Status(e) == kFailed && sim.bt.FindInstance(e)->tree->nodes.size() == 2u * (kBtMaxSubTreeDepth + 1)
                         && CountWarnings(before, "nested deeper") == 1,
                     "SubTree: 自分自身を取り込む循環は入れ子の上限で止まり、Failure + 警告 1 回");
        }
        {
            // 取り込み後がノード数の上限を超える木は、どの SubTree も取り込まない (Failure + 警告)。収まるなら取り込む。
            // 木の中身: Selector / SubTree / Wait + どこにもつながらないノード (読み込みは通る)
            const uint64_t leaf = RegisterTree(lib, L"sub_big_leaf", Tree(0, { Wait(0, 0) }));
            const auto big = [&](int total, const wchar_t* name) {
                std::vector<json> nodes{ Node(0, "Selector", { 1, 2 }), SubTreeNode(1, leaf), Wait(2, 1000) };
                for (int i = 3; i < total; ++i) {
                    nodes.push_back(Wait(i, 1));
                }
                return RegisterTree(lib, name, Tree(0, nodes));
            };
            const uint64_t tooBig = big(kBtMaxNodes, L"sub_big_over");
            const uint64_t fits = big(kBtMaxNodes - 1, L"sub_big_fit");
            const uint64_t before = logging::TotalWritten();
            Sim sim;
            const EntityID over = sim.AddTreeEntity(tooBig, "Over");
            const EntityID fit = sim.AddTreeEntity(fits, "Fit");
            sim.Step();
            ck.Check(tooBig != 0 && fits != 0 && sim.Status(over) == kRunning && sim.bt.FindInstance(over)->tree == lib.trees.GetShared(tooBig)
                         && CountWarnings(before, "too large") == 1,
                     "SubTree: 取り込むとノード数の上限を超える木は全部を取り込まず (SubTree は Failure)、警告 1 回");
            ck.Check(sim.Status(fit) == kSucceeded && sim.bt.FindInstance(fit)->tree->nodes.size() == static_cast<size_t>(kBtMaxNodes),
                     "SubTree: 取り込んでちょうど上限に収まる木は取り込まれる");
        }

        // 部分木の中の Abort は親の木の監視に入る (spec 4.1.1 の優先順)
        {
            // (a) 部分木の中の LowerPriority
            const uint64_t sub = RegisterTree(lib, L"sub_lp",
                                              Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 1001), { BbCond("Alert", "IsSet", "LowerPriority") }), Wait(2, 1002) }, board));
            const uint64_t parent = RegisterTree(lib, L"sub_lp_parent", Tree(0, { Node(0, "Sequence", { 1 }), SubTreeNode(1, sub) }, board));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            sim.Step();
            const bool lowRunning = sim.Comp(e)->activeNodeId == waitIdOf(sim, e, 1002);
            bbOf(sim, e, kAlert) = BbValue{ 1, 1, 0.0f, { 0.0f, 0.0f, 0.0f }, kNullEntity };
            sim.Step();
            ck.Check(lowRunning && sim.Comp(e)->activeNodeId == waitIdOf(sim, e, 1001) && Is(trace, { waitIdOf(sim, e, 1002) }),
                     "部分木の中の LowerPriority が、部分木の中の右の兄弟を Abort して左から入り直す");
            sim.bt.SetAbortTrace(nullptr);
        }
        {
            // (b) SubTree ノードに付けた Self の Decorator: 部分木の中身は深い方から Abort され、SubTree ノードが最後
            const uint64_t sub = RegisterTree(lib, L"sub_self", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 1003) }, board));
            const uint64_t parent = RegisterTree(lib, L"sub_self_parent", Tree(7, { Decorated(SubTreeNode(7, sub), { BbCond("Run", "IsSet", "Self") }) }, board));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            sim.Step();
            const bool running = sim.Status(e) == kRunning && sim.Comp(e)->activeNodeId == waitIdOf(sim, e, 1003);
            bbOf(sim, e, kRun).isSet = 0;
            sim.Step();
            ck.Check(running && trace.size() == 3 && trace[0] == waitIdOf(sim, e, 1003) && trace[2] == 7 && sim.Status(e) == kFailed,
                     "SubTree ノードの Self Abort: 部分木の中身を深い方から Abort し、SubTree ノードが最後 (Failure)");
            sim.bt.SetAbortTrace(nullptr);
        }
        {
            // (c) 親の Selector の LowerPriority が、SubTree の中で動いているものを Abort する (深い方から → SubTree ノード)
            const uint64_t sub = RegisterTree(lib, L"sub_run", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 1005) }, board));
            const uint64_t parent = RegisterTree(
                lib, L"sub_lp2_parent",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 1004), { BbCond("Alert", "IsSet", "LowerPriority") }), SubTreeNode(2, sub) }, board));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            sim.Step();
            const bool inSub = sim.Comp(e)->activeNodeId == waitIdOf(sim, e, 1005);
            bbOf(sim, e, kAlert) = BbValue{ 1, 1, 0.0f, { 0.0f, 0.0f, 0.0f }, kNullEntity };
            sim.Step();
            ck.Check(inSub && trace.size() == 3 && trace[0] == waitIdOf(sim, e, 1005) && trace[2] == 2 && sim.Comp(e)->activeNodeId == waitIdOf(sim, e, 1004),
                     "親の LowerPriority が SubTree の中で動いているものを深い方から Abort して左の兄弟へ入り直す");
            sim.bt.SetAbortTrace(nullptr);
        }
        {
            // 取り込み元が読み直されたら実行中の木は作り直し (BB は保つ)。実行中だった部分木は Abort される
            const uint64_t sub = RegisterTree(lib, L"sub_reload", Tree(0, { Wait(0, 1000) }, board));
            const uint64_t parent = RegisterTree(lib, L"sub_reload_parent", Tree(0, { Node(0, "Sequence", { 1 }), SubTreeNode(1, sub) }, board));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            sim.Step();
            bbOf(sim, e, kN) = BbValue{ 1, 42, 0.0f, { 0.0f, 0.0f, 0.0f }, kNullEntity };
            const bool waiting = sim.Status(e) == kRunning;
            const int32_t subWaitId = waitIdOf(sim, e, 1000);
            std::vector<int32_t> trace;
            sim.bt.SetAbortTrace(&trace);
            RegisterTree(lib, L"sub_reload", Tree(0, { setN(0, 9) }, board));
            sim.Step();
            sim.bt.SetAbortTrace(nullptr);
            ck.Check(waiting && sim.Status(e) == kSucceeded && bbOf(sim, e, kN).i == 9, "取り込み元の木が読み直されたら、親の木は新しい部分木で最初からやり直す");
            ck.Check(trace.size() == 3 && trace.front() == subWaitId && trace.back() == 0 && sim.Comp(e)->lastAbortTick == static_cast<int32_t>(sim.LastTick()),
                     "取り込み元の木が読み直されたら、取り込んでいる親の木で動くエンティティは古い形のまま深い方から Abort され (lastAbortTick が立つ)、根からやり直す");
        }
        {
            // 取り込み元が後から登録されたら、次の tick に取り込まれる
            const uint64_t late = guidOf(L"sub_late");
            const uint64_t parent = RegisterTree(lib, L"sub_late_parent", Tree(0, { SubTreeNode(0, late) }));
            const uint64_t before = logging::TotalWritten();
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            sim.Step();
            const bool failedFirst = sim.Status(e) == kFailed && CountWarnings(before, "not registered") == 1;
            RegisterTree(lib, L"sub_late", Tree(0, { Wait(0, 0) }));
            sim.Step();
            sim.Step();
            ck.Check(failedFirst && sim.Status(e) == kSucceeded, "未登録だった取り込み元を登録すると、親の木が取り込み直して動く");
        }

        // 取り込みの途中 (2 段入れ子 + 部分木の中の Abort) で保存 → 復元 → 連続実行と毎 tick のハッシュが一致
        {
            const uint64_t inner = RegisterTree(lib, L"snap_inner",
                                                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 40), { BbCond("Alert", "IsSet", "LowerPriority") }), Wait(2, 50) }, board));
            const uint64_t middle = RegisterTree(lib, L"snap_mid", Tree(0, { Node(0, "Sequence", { 1, 2, 3 }), Wait(1, 5), SubTreeNode(2, inner), setN(3, 4) }, board));
            const uint64_t top = RegisterTree(lib, L"snap_top", Tree(0, { Node(0, "Sequence", { 1, 2 }), SubTreeNode(1, middle), Wait(2, 10) }, board));
            Scene scene;
            BehaviorTreeSystem first;
            GameObject go = scene.CreateGameObjectTracked("Agent");
            go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ top };
            const EntityID agent = go.Id();
            scene.GetWorld().ApplyStructuralChanges();
            SimRefs refs;
            refs.scene = &scene;
            refs.behaviorTree = &first;
            uint64_t tickRef = 0;
            refs.tickIndex = &tickRef;
            uint64_t tick = 1;
            const auto step = [&](BehaviorTreeSystem& bt) {
                bt.DeliverPending(tick);
                bt.Update(scene.GetWorld(), tick, nullptr);
                BtInstance* inst = const_cast<BtInstance*>(bt.FindInstance(agent));
                if (inst != nullptr && !inst->blackboard.empty()) {
                    inst->blackboard[kAlert].i = (tick / 9) % 2 == 0 ? 0 : 1; // Alert を周期的に反転する
                    inst->blackboard[kAlert].isSet = 1;
                }
                scene.GetWorld().ApplyStructuralChanges();
                ++tick;
            };
            for (int i = 0; i < 8; ++i) {
                step(first); // Wait(5) を終えて内側の Selector の中にいる
            }
            tickRef = tick;
            std::vector<std::byte> blob;
            const bool captured = CaptureSimSnapshot(refs, blob) && first.FindInstance(agent)->tree->nodes.size() == 10;
            ck.Check(captured, "(前提) 2 段入れ子の SubTree の途中で撮影できる (展開後 10 ノード)");

            const uint64_t startTick = tick;
            std::vector<uint64_t> continuous;
            std::vector<int32_t> activeAfter;
            for (int i = 0; i < 150; ++i) {
                step(first);
                continuous.push_back(HashWorld(scene.GetWorld(), refs.HashSources()));
                activeAfter.push_back(scene.GetWorld().GetComponent<BehaviorTreeComponent>(agent)->activeNodeId);
            }
            BehaviorTreeSystem second;
            SimRefs refsSecond = refs;
            refsSecond.behaviorTree = &second;
            bool same = captured && RestoreSimSnapshot(refsSecond, blob.data(), blob.size());
            tick = startTick;
            bool sameActive = true;
            for (int i = 0; i < 150 && same; ++i) {
                step(second);
                same = HashWorld(scene.GetWorld(), refsSecond.HashSources()) == continuous[static_cast<size_t>(i)];
                sameActive = sameActive && scene.GetWorld().GetComponent<BehaviorTreeComponent>(agent)->activeNodeId == activeAfter[static_cast<size_t>(i)];
            }
            ck.Check(same && sameActive && continuous.front() != continuous.back() && second.StateHash() == first.StateHash(),
                     "SubTree の途中で保存 → 復元 → 150 tick の毎 tick のハッシュと実行中ノードが連続実行と一致 (部分木の Abort を含む)");
        }
    }

    // ---- 15. ライブ表示の読み口 (M85j) ----
    {
        const uint64_t board = RegisterBoard(lib, L"live_bb", Board({ BbKey("Flag", "Bool", true) }));
        const auto setFlag = [](Sim& sim, EntityID e, bool on) { sim.Mutable(e)->blackboard[0].i = on ? 1 : 0; };
        {
            // Self: Decorator の付いたノードと、止められた実行中の葉を残す。BT 節には入らず、復元後は空
            const uint64_t guid = RegisterTree(
                lib, L"live_self",
                Tree(0, { Node(0, "Selector", { 1, 3 }), Decorated(Node(1, "Sequence", { 2 }), { BbCond("Flag", "IsSet", "Self") }), Wait(2, 100),
                          Wait(3, 100) },
                     board));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(guid);
            sim.Step();
            const bool none = sim.bt.FindInstance(e)->lastAbort.sourceId < 0;
            setFlag(sim, e, false);
            sim.Step();
            const BtAbortRecord record = sim.bt.FindInstance(e)->lastAbort;
            ck.Check(none && record.sourceId == 1 && record.targetId == 2 && record.tick == sim.LastTick() && sim.Comp(e)->activeNodeId == 3,
                     "Abort の記録: Self は Decorator の付いたノード (1) と止められた葉 (2) を、止めた tick と一緒に残す");

            std::vector<int32_t> active;
            BehaviorTreeSystem::ActiveNodeIndices(*sim.bt.FindInstance(e), active);
            ck.Check(Is(active, { 0, 3 }), "実行中ノードの列: 入っていて終わっていないノードだけが添字順に並ぶ (Abort 後は根と 3)");

            const uint64_t hashWith = sim.bt.StateHash();
            std::vector<std::byte> bytes;
            ByteWriter writer(bytes);
            sim.bt.SaveSnapshot(writer);
            BtSnapshot parsed;
            ByteReader reader(bytes.data(), bytes.size());
            const bool read = BehaviorTreeSystem::ReadSnapshot(reader, parsed);
            sim.bt.ApplySnapshot(sim.GetWorld(), std::move(parsed));
            const BtInstance* restored = sim.bt.FindInstance(e);
            ck.Check(read && restored != nullptr && restored->lastAbort.sourceId < 0 && sim.bt.StateHash() == hashWith,
                     "Abort の記録は BT 節にもハッシュにも入らない (復元すると空で、ハッシュは記録の有無で変わらない)");
        }
        {
            // LowerPriority: 優先側の兄弟 (1) が、止められた右の葉 (2) を指す
            const uint64_t guid = RegisterTree(
                lib, L"live_lower",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 100), { BbCond("Flag", "IsSet", "LowerPriority") }), Wait(2, 100) },
                     RegisterBoard(lib, L"live_bb_off", Board({ BbKey("Flag", "Bool", false) }))));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(guid);
            sim.Step();
            sim.Step();
            setFlag(sim, e, true);
            sim.Step();
            const BtAbortRecord record = sim.bt.FindInstance(e)->lastAbort;
            ck.Check(record.sourceId == 1 && record.targetId == 2 && record.tick == sim.LastTick(),
                     "Abort の記録: LowerPriority は条件が真になった兄弟 (1) から、止められた右の葉 (2) へ");
        }
        {
            // Timeout: 打ち切った Decorator のノード自身 (葉なので source = target)
            const uint64_t guid = RegisterTree(lib, L"live_timeout", Tree(0, { Decorated(Wait(0, 100), { Timeout(3) }) }));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(guid);
            Run(sim, e, 6);
            const BtAbortRecord record = sim.bt.FindInstance(e)->lastAbort;
            ck.Check(record.sourceId == 0 && record.targetId == 0 && record.tick > 0, "Abort の記録: Timeout は打ち切ったノード自身を指す");
        }
        {
            // SubTree の展開: 実行木の id は振り直されるが、元の木の (GUID, 元の id) へ戻せる
            const uint64_t sub = RegisterTree(lib, L"live_sub", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 100) }));
            const uint64_t parent = RegisterTree(lib, L"live_parent", Tree(0, { Node(0, "Sequence", { 1, 2 }), SubTreeNode(1, sub), Wait(2, 5) }));
            const uint64_t plain = RegisterTree(lib, L"live_plain", Tree(0, { Wait(0, 100) }));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            const EntityID p = sim.AddTreeEntity(plain);
            sim.Step();
            const BehaviorTreeAsset& tree = *sim.bt.FindInstance(e)->tree;
            int fromSub = 0;
            int fromParent = 0;
            bool idsKept = true;
            for (const BtNodeDef& node : tree.nodes) {
                if (tree.OriginTreeOf(node) == sub) {
                    ++fromSub;
                    idsKept = idsKept && node.id != tree.OriginIdOf(node) && (tree.OriginIdOf(node) == 0 || tree.OriginIdOf(node) == 1);
                } else if (tree.OriginTreeOf(node) == parent) {
                    ++fromParent;
                    idsKept = idsKept && node.id == tree.OriginIdOf(node);
                }
            }
            std::vector<int32_t> active;
            BehaviorTreeSystem::ActiveNodeIndices(*sim.bt.FindInstance(e), active);
            const BehaviorTreeAsset& plainTree = *sim.bt.FindInstance(p)->tree;
            ck.Check(fromSub == 2 && fromParent == 3 && idsKept && active.size() == 4
                         && plainTree.OriginTreeOf(plainTree.nodes[0]) == plain && plainTree.OriginIdOf(plainTree.nodes[0]) == 0,
                     "SubTree の展開: 部分木のノードは (部分木の GUID, 元の id) へ、親のノードは (親の GUID, 同じ id) へ戻せる (展開しない木は自分自身)");
        }
        {
            // 親の LowerPriority が部分木の中で動いているものを止めた記録を、親の木・部分木の木のどちらの id へも戻せる
            const uint64_t flagOff = RegisterBoard(lib, L"live_bb_off2", Board({ BbKey("Flag", "Bool", false) }));
            const uint64_t sub = RegisterTree(lib, L"live_sub2", Tree(0, { Node(0, "Sequence", { 1 }), Wait(1, 100) }, flagOff));
            const uint64_t parent = RegisterTree(
                lib, L"live_parent2",
                Tree(0, { Node(0, "Selector", { 1, 2 }), Decorated(Wait(1, 100), { BbCond("Flag", "IsSet", "LowerPriority") }), SubTreeNode(2, sub) },
                     flagOff));
            Sim sim;
            const EntityID e = sim.AddTreeEntity(parent);
            sim.Step();
            sim.Step();
            setFlag(sim, e, true);
            sim.Step();
            const BtInstance* inst = sim.bt.FindInstance(e);
            const BehaviorTreeAsset& tree = *inst->tree;
            const int source = tree.FindNode(inst->lastAbort.sourceId);
            const int target = tree.FindNode(inst->lastAbort.targetId);
            ck.Check(source >= 0 && target >= 0 && tree.DisplayedIdOf(source, parent) == 1 && tree.DisplayedIdOf(target, parent) == 2
                         && tree.DisplayedIdOf(target, sub) == 1 && tree.DisplayedIdOf(source, sub) == -1,
                     "ライブ表示の対応: 部分木の中の葉は、親の木では SubTree ノード (2)、部分木の木では元の id (1) へ戻り、親の Decorator ノードは部分木の木には無い");
        }
    }

    // ---- 8. 計測: 100 体 x 30 ノード ----
    {
        Sim sim;
        const uint64_t guid = RegisterTree(lib, L"perf", PerfTree());
        BehaviorTreeAsset shape;
        ck.Check(guid != 0 && BehaviorTreeLibrary::FromJson(PerfTree(), shape) && shape.nodes.size() == 30, "(前提) 計測用の木は 30 ノード");
        for (int i = 0; i < 100; ++i) {
            sim.AddTreeEntity(guid, "Perf");
        }
        sim.GetWorld().ApplyStructuralChanges();
        constexpr int kTicks = 200;
        double updateUs = 0.0;
        double hashUs = 0.0;
        uint64_t hashSink = 0; // StateHash の呼び出しが最適化で消えないよう結果を使う
        for (int i = 0; i < kTicks; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            sim.bt.Update(sim.GetWorld(), sim.tick++, nullptr);
            const auto t1 = std::chrono::steady_clock::now();
            hashSink ^= sim.bt.StateHash();
            const auto t2 = std::chrono::steady_clock::now();
            updateUs += std::chrono::duration<double, std::micro>(t1 - t0).count();
            hashUs += std::chrono::duration<double, std::micro>(t2 - t1).count();
        }
        MYE_LOG_INFO("  [perf] 100 trees x 30 nodes: Update avg %.1f us / tick, StateHash avg %.1f us / tick (sink %llx)",
                     updateUs / kTicks, hashUs / kTicks, static_cast<unsigned long long>(hashSink));
        ck.Check(updateUs / kTicks < 5000.0, "100 体 x 30 ノードの 1 tick が桁違いに遅くない (Debug でも 5 ms 未満)");
    }

    if (ck.failCount == 0) {
        MYE_LOG_INFO("==== BehaviorTree self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== BehaviorTree self test: %d FAILED ====", ck.failCount);
    }
    return ck.failCount == 0;
}

} // namespace mye
