//====================================================================================
//                          BehaviorTreeSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリーの核の回帰テスト実装
//====================================================================================
#include "Engine/Engine/AI/BehaviorTreeSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "Engine/Engine/Demo/DemoContent.h"
#include "Engine/Engine/Loop/EngineLoop.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
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

    World& GetWorld() { return scene.GetWorld(); }

    EntityID AddTreeEntity(uint64_t tree, const char* name = "Agent")
    {
        GameObject go = scene.CreateGameObjectTracked(name);
        go.AddComponent<BehaviorTreeComponent>()->tree = AssetID{ tree };
        return go.Id();
    }

    void Step()
    {
        bt.Update(GetWorld(), tick);
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
            bt.Update(scene.GetWorld(), tick);
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
        const auto craft = [](uint32_t firstIndex, uint32_t secondIndex, uint8_t rootStatus, uint8_t active) {
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
            }
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
                bt.Update(scene.GetWorld(), tick);
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
            sim.bt.Update(sim.GetWorld(), sim.tick++);
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
