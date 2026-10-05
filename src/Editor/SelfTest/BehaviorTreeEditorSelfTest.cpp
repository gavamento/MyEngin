//====================================================================================
//                          BehaviorTreeEditorSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          BT 窓のモデル層の回帰テスト実装
//====================================================================================
#include "Editor/SelfTest/BehaviorTreeEditorSelfTest.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "Editor/Windows/AI/BehaviorTreeEditModel.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

using nlohmann::json;

// 空の木を登録して、編集用に読む (CreateBehaviorTreeAsset と同じ形: 根に Selector 1 つ)
std::shared_ptr<const BehaviorTreeAsset> RegisterEmpty(BehaviorTreeLibrary& trees, const fs::path& path, uint64_t blackboard = 0)
{
    BehaviorTreeAsset asset;
    asset.blackboard = blackboard;
    trees.Register(path.wstring(), std::move(asset));
    return trees.GetShared(BehaviorTreeLibrary::HashForPath(path.wstring()));
}

BtParamValue IntValue(int32_t v)
{
    BtParamValue value;
    value.i = v;
    return value;
}

BtParamValue FloatValue(float v)
{
    BtParamValue value;
    value.f = v;
    return value;
}

BtParamValue StringValue(const std::string& v)
{
    BtParamValue value;
    value.s = v;
    return value;
}

std::vector<int32_t> ChildrenOf(const BehaviorTreeEditModel& model, int32_t id)
{
    const BtNodeDef* node = model.FindNode(id);
    return node != nullptr ? node->childIds : std::vector<int32_t>{};
}

// 木と編集中の BB を 1 本の文字列にする (Undo の往復が元のバイト列に戻るかの比較用)
std::string DumpState(const BehaviorTreeEditModel& model)
{
    std::string text = BehaviorTreeLibrary::ToJson(model.Asset()).dump();
    if (model.Board() != nullptr) {
        text += BlackboardLibrary::ToJson(*model.Board()).dump();
    }
    return text;
}

// Undo できる限り戻した段数
int UndoAll(BehaviorTreeEditModel& model)
{
    int steps = 0;
    while (model.Undo()) {
        ++steps;
    }
    return steps;
}

int RedoAll(BehaviorTreeEditModel& model)
{
    int steps = 0;
    while (model.Redo()) {
        ++steps;
    }
    return steps;
}

bool HasIssue(const std::vector<BtIssue>& issues, BtIssueKind kind, int32_t nodeId)
{
    return std::any_of(issues.begin(), issues.end(), [&](const BtIssue& i) { return i.kind == kind && i.nodeId == nodeId; });
}

bool ReadJsonFile(const fs::path& path, json& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out = json::parse(file, nullptr, false);
    return !out.is_discarded();
}

} // namespace

bool RunBehaviorTreeEditorSelfTest()
{
    MYE_LOG_INFO("==== BehaviorTreeEditModel (M85h) self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_bt_editor_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    BehaviorTreeLibrary trees;
    BlackboardLibrary boards;

    // ---- 1. 全種類の種類表から作れ、キー欄の候補が空にならない ----
    {
        bool everyKeyHasType = true;
        for (int k = 0; k < static_cast<int>(BtNodeKind::Count); ++k) {
            const BtNodeKind kind = static_cast<BtNodeKind>(k);
            const BtNodeTypeInfo& info = BtNodeTypeOf(kind);
            for (int key = 0; key < info.keyCount; ++key) {
                bool any = false;
                for (const BbType type : { BbType::Bool, BbType::Int, BbType::Float, BbType::Vector, BbType::Entity }) {
                    any = any || BehaviorTreeEditModel::KeyAccepts(kind, key, type);
                }
                if (!any) {
                    MYE_LOG_ERROR("  key %s[%d] accepts no BB type", info.name, key);
                    everyKeyHasType = false;
                }
            }
        }
        check(everyKeyHasType, "全ノード種類・全キー欄に、選べる BB のキーの型がある (新しい種類を足して絞り込みを忘れたら赤くなる)");
        check(BehaviorTreeEditModel::KeyAccepts(BtNodeKind::Patrol, 0, BbType::Entity) && !BehaviorTreeEditModel::KeyAccepts(BtNodeKind::Patrol, 0, BbType::Vector)
                  && BehaviorTreeEditModel::KeyAccepts(BtNodeKind::MoveTo, 0, BbType::Vector) && !BehaviorTreeEditModel::KeyAccepts(BtNodeKind::MoveTo, 0, BbType::Bool)
                  && BehaviorTreeEditModel::KeyAccepts(BtNodeKind::SetBlackboard, 0, BbType::Bool),
                  "キー欄の絞り込み: Patrol = Entity、MoveTo = Vector / Entity、SetBlackboard = 全部");
    }

    // ---- 2. 追加・接続・子の順序 (x の左から) ----
    const fs::path mainPath = root / L"main.bt.json";
    BehaviorTreeEditModel model;
    model.BindLibraries(&trees, &boards);
    {
        check(model.Load(RegisterEmpty(trees, mainPath)) && model.IsLoaded() && model.Asset().nodes.empty() && !model.Dirty(),
              "空の木 (ノード 0 個) を読める。dirty は偽");
        const int32_t selector = model.AddNode(BtNodeKind::Selector, 200.0f, 0.0f);
        check(selector == 0 && model.RootId() == selector && model.Dirty(), "最初に足したノードが根になり、dirty が立つ");
        const int32_t waitA = model.AddNode(BtNodeKind::Wait, 300.0f, 120.0f);
        const int32_t waitB = model.AddNode(BtNodeKind::Wait, 100.0f, 120.0f);
        const int32_t sequence = model.AddNode(BtNodeKind::Sequence, 500.0f, 120.0f);
        const int32_t moveTo = model.AddNode(BtNodeKind::MoveTo, 480.0f, 240.0f);
        check(waitA == 1 && waitB == 2 && sequence == 3 && moveTo == 4 && model.RootId() == selector, "ノード id は連番で、2 つ目以降は根を変えない");
        check(model.FindNode(waitA)->params.size() == 2 && model.FindNode(waitA)->params[0].i == 60 && model.FindNode(moveTo)->keys.size() == 1
                  && model.FindNode(moveTo)->params[btmoveparam::kObserveTarget].i == 1,
              "追加したノードのパラメータは種類表の既定値、キー欄は空");

        check(model.Connect(selector, waitA) && model.Connect(selector, sequence) && model.Connect(selector, waitB),
              "親の下へ子を接続できる");
        check(ChildrenOf(model, selector) == std::vector<int32_t>{ waitB, waitA, sequence },
              "子の順序は x の左から (接続した順 A, Sequence, B ではなく B, A, Sequence)");
        check(model.ChildOrderOf(waitB) == 0 && model.ChildOrderOf(waitA) == 1 && model.ChildOrderOf(sequence) == 2 && model.ParentOf(waitA) == selector,
              "ChildOrderOf / ParentOf が子の順序と親を返す");
        check(model.MoveNode(waitB, 400.0f, 120.0f) && ChildrenOf(model, selector) == std::vector<int32_t>{ waitA, waitB, sequence },
              "子を右へ動かして兄弟を追い越すと、順序が x の順へ並べ直る");
        check(model.Connect(sequence, moveTo) && model.ParentOf(moveTo) == sequence, "孫を接続できる");

        // 拒否
        check(!model.Connect(waitA, waitB), "葉 (Wait) の下には接続できない");
        check(!model.Connect(moveTo, selector) && !model.Connect(sequence, selector), "根を子にはできない");
        {
            const int32_t inner = model.AddNode(BtNodeKind::Sequence, 560.0f, 240.0f);
            check(model.Connect(sequence, inner) && !model.Connect(inner, sequence), "循環 (子孫を親にする) は拒む");
            model.Remove(inner, BtRemoveMode::WithDescendants);
        }
        check(!model.Connect(selector, selector) && !model.Connect(99, selector) && !model.Connect(selector, 99), "自分自身・存在しないノードは拒む");
        const int32_t parallel = model.AddNode(BtNodeKind::SimpleParallel, 700.0f, 120.0f);
        const int32_t w1 = model.AddNode(BtNodeKind::Wait, 650.0f, 240.0f);
        const int32_t w2 = model.AddNode(BtNodeKind::Wait, 700.0f, 240.0f);
        const int32_t w3 = model.AddNode(BtNodeKind::Wait, 750.0f, 240.0f);
        check(model.Connect(parallel, w1) && model.Connect(parallel, w2) && !model.Connect(parallel, w3), "SimpleParallel の子は 2 つまで");
        const int32_t subTree = model.AddNode(BtNodeKind::SubTree, 900.0f, 120.0f);
        check(!model.Connect(subTree, w3), "SubTree はファイルでは子を持てない");

        // 付け替え
        check(model.Connect(sequence, w3) && model.ParentOf(w3) == sequence && ChildrenOf(model, sequence) == std::vector<int32_t>{ moveTo, w3 },
              "子を別の親へ付け替えると、元の親の子から外れる");
        check(model.Connect(parallel, w3) == false && model.ParentOf(w3) == sequence, "上限の親への付け替えは拒み、元のつながりは壊さない");
        check(model.Disconnect(w3) && model.ParentOf(w3) == -1 && !model.Disconnect(w3), "切断すると親なしの根になり、親なしの切断は false");
        // 付け替えで親が変わらないとき (同じ親へ再接続) は並べ直すだけ
        check(model.Connect(sequence, moveTo) && ChildrenOf(model, sequence) == std::vector<int32_t>{ moveTo }, "同じ親への再接続は並べ直すだけで子を増やさない");

        // 深さの上限 (kBtMaxDepth)
        {
            BehaviorTreeEditModel deep;
            deep.BindLibraries(&trees, &boards);
            deep.Load(RegisterEmpty(trees, root / L"deep.bt.json"));
            int32_t parent = deep.AddNode(BtNodeKind::Sequence, 0.0f, 0.0f);
            int depth = 1;
            while (true) {
                const int32_t child = deep.AddNode(BtNodeKind::Sequence, 0.0f, static_cast<float>(depth) * 10.0f);
                if (!deep.Connect(parent, child)) {
                    break;
                }
                parent = child;
                ++depth;
            }
            check(depth == kBtMaxDepth && deep.CheckSavable().problem == BtSaveProblem::None,
                  "深さの上限 (kBtMaxDepth) の手前で接続を拒み、そこまでの木は保存できる");
        }
    }

    // ---- 3. 削除 ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"remove.bt.json"));
        const int32_t rootNode = m.AddNode(BtNodeKind::Selector, 100.0f, 0.0f);
        const int32_t a = m.AddNode(BtNodeKind::Sequence, 0.0f, 100.0f);
        const int32_t a1 = m.AddNode(BtNodeKind::Wait, -50.0f, 200.0f);
        const int32_t a2 = m.AddNode(BtNodeKind::Wait, 50.0f, 200.0f);
        const int32_t b = m.AddNode(BtNodeKind::Wait, 200.0f, 100.0f);
        m.Connect(rootNode, a);
        m.Connect(rootNode, b);
        m.Connect(a, a1);
        m.Connect(a, a2);
        check(m.Remove(a, BtRemoveMode::KeepChildren) && m.FindNode(a) == nullptr && ChildrenOf(m, rootNode) == std::vector<int32_t>{ b }
                  && m.ParentOf(a1) == -1 && m.ParentOf(a2) == -1 && m.FindNode(a1)->pos[0] == -50.0f && m.FindNode(a2)->pos[1] == 200.0f,
              "子を残して削除: ノードだけ消え、子は親なしの根として位置のまま残る");
        check(m.Connect(rootNode, a1), "残った子は再び接続できる");
        check(m.Remove(a1, BtRemoveMode::WithDescendants) && m.FindNode(a1) == nullptr && ChildrenOf(m, rootNode) == std::vector<int32_t>{ b }, "子ごと削除で、親の子からも外れる");
        m.AddNode(BtNodeKind::Sequence, 0.0f, 300.0f);
        const int32_t sub = m.AddNode(BtNodeKind::Sequence, 0.0f, 400.0f);
        const int32_t subChild = m.AddNode(BtNodeKind::Wait, 0.0f, 500.0f);
        m.Connect(sub, subChild);
        const size_t before = m.Asset().nodes.size();
        check(m.Remove(sub, BtRemoveMode::WithDescendants) && m.Asset().nodes.size() == before - 2 && m.FindNode(subChild) == nullptr, "子ごと削除は孫も消す");
        check(m.Remove(rootNode, BtRemoveMode::KeepChildren) && m.RootId() == -1 && m.ParentOf(b) == -1, "根を消すと根なしになる (子は残る)");
        check(m.SetRoot(b) && m.RootId() == b && !m.SetRoot(b) && !m.SetRoot(99), "親のないノードを根にできる。同じ根・存在しないノードは false");
        check(!m.Remove(99, BtRemoveMode::WithDescendants), "存在しないノードの削除は false");
    }

    // ---- 4. パラメータ・キー・Decorator ----
    {
        const uint64_t boardGuid = [&] {
            BlackboardAsset board;
            BbKeyDef alarm;
            alarm.name = "Alarm";
            alarm.type = BbType::Bool;
            BbKeyDef goal;
            goal.name = "Goal";
            goal.type = BbType::Vector;
            board.keys = { alarm, goal };
            return boards.Register((root / L"unit.bb.json").wstring(), std::move(board));
        }();
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"params.bt.json"));
        const int32_t waitNode = m.AddNode(BtNodeKind::Wait, 0.0f, 0.0f);
        const int32_t moveNode = m.AddNode(BtNodeKind::MoveTo, 0.0f, 100.0f);
        const int32_t sendNode = m.AddNode(BtNodeKind::SendEvent, 0.0f, 200.0f);
        m.SetParam(waitNode, 0, IntValue(60)); // 既定と同じ値は変化なし
        check(!m.SetParam(waitNode, 0, IntValue(60)), "既定と同じ値は変更なし (false)");
        check(m.SetParam(waitNode, 0, IntValue(kBtMaxTicksParam + 1000)) && m.FindNode(waitNode)->params[0].i == kBtMaxTicksParam, "Int は範囲の上限へ丸める");
        check(m.SetParam(waitNode, 0, IntValue(-5)) && m.FindNode(waitNode)->params[0].i == 0, "Int は範囲の下限へ丸める");
        check(m.SetParam(moveNode, btmoveparam::kAcceptanceRadius, FloatValue(2.5f)) && m.FindNode(moveNode)->params[btmoveparam::kAcceptanceRadius].f == 2.5f
                  && !m.SetParam(moveNode, btmoveparam::kAcceptanceRadius, FloatValue(std::nanf(""))),
              "Float を書ける。非有限は拒む");
        check(m.SetParam(moveNode, btmoveparam::kFailOnStuck, IntValue(7)) && m.FindNode(moveNode)->params[btmoveparam::kFailOnStuck].i == 1, "Bool は 0 / 1 へ丸める");
        BtParamValue guid;
        guid.u = 0x1234abcdULL;
        check(m.SetParam(moveNode, btmoveparam::kNavFilter, guid) && m.FindNode(moveNode)->params[btmoveparam::kNavFilter].u == 0x1234abcdULL, "Guid (アセット参照) を書ける");
        check(m.SetParam(sendNode, btsendparam::kTarget, IntValue(9)) && m.FindNode(sendNode)->params[btsendparam::kTarget].i == 2, "Enum は添字の範囲へ丸める");
        check(m.SetParam(sendNode, btsendparam::kEventName, StringValue("alert")) && !m.SetParam(sendNode, btsendparam::kEventName, StringValue(std::string(64, 'x')))
                  && m.FindNode(sendNode)->params[btsendparam::kEventName].s == "alert",
              "文字列を書ける。63 バイトを超える文字列は拒み、元の値のまま");
        check(!m.SetParam(waitNode, 5, IntValue(1)) && !m.SetParam(99, 0, IntValue(1)), "範囲外のパラメータ・存在しないノードは false");

        check(m.SetKey(moveNode, btnodekey::kTarget, "Goal") && m.FindNode(moveNode)->keys[0] == "Goal" && !m.SetKey(moveNode, btnodekey::kTarget, "Goal"),
              "キー欄を書ける。同じ名前は変更なし");
        check(!m.SetKey(moveNode, 3, "x") && !m.SetKey(moveNode, 0, std::string(64, 'k')), "存在しないキー欄・長すぎる名前は拒む");
        check(m.SetKey(moveNode, 0, "") && m.FindNode(moveNode)->keys[0].empty(), "空の名前 = 未指定へ戻せる");

        // Decorator: BB が無いと BlackboardCondition は足せない
        check(m.AddDecorator(waitNode, BtDecoratorKind::BlackboardCondition) == -1, "BB を使っていない木には BlackboardCondition を足せない (キー名が空になるため)");
        check(m.AddDecorator(waitNode, BtDecoratorKind::Cooldown) == 0 && m.AddDecorator(waitNode, BtDecoratorKind::Invert) == 1
                  && m.FindNode(waitNode)->decorators.size() == 2 && m.FindNode(waitNode)->decorators[0].params[0].i == 60,
              "BB の要らない Decorator は足せて、既定値で始まる");
        check(BehaviorTreeEditModel::NodeHeight(*m.FindNode(waitNode)) == kBtNodeBodyHeight + 2.0f * kBtDecoratorBandHeight, "箱の高さは Decorator の帯の数だけ増える");
        check(m.SetBlackboard(boardGuid) && m.Board() != nullptr && m.Board()->keys.size() == 2 && !m.SetBlackboard(boardGuid), "BB を選べる。同じ BB は変更なし");
        const int bbCond = m.AddDecorator(waitNode, BtDecoratorKind::BlackboardCondition);
        check(bbCond == 2 && m.FindNode(waitNode)->decorators[2].key == "Alarm", "BlackboardCondition は BB の先頭のキーで始まる");
        check(m.SetDecoratorKey(waitNode, bbCond, "Goal") && !m.SetDecoratorKey(waitNode, bbCond, "") && !m.SetDecoratorKey(waitNode, 0, "Goal"),
              "Decorator のキーを替えられる。空・キー欄の無い Decorator は拒む");
        check(m.SetDecoratorParam(waitNode, 0, 0, IntValue(120)) && m.FindNode(waitNode)->decorators[0].params[0].i == 120 && !m.SetDecoratorParam(waitNode, 1, 0, IntValue(1)),
              "Decorator のパラメータを書ける。パラメータの無い Decorator は拒む");
        check(m.MoveDecorator(waitNode, 2, 0) && m.FindNode(waitNode)->decorators[0].kind == BtDecoratorKind::BlackboardCondition
                  && m.FindNode(waitNode)->decorators[1].kind == BtDecoratorKind::Cooldown && !m.MoveDecorator(waitNode, 0, 0) && !m.MoveDecorator(waitNode, 0, 3),
              "Decorator を上へ並べ替えられる (外側から順)");
        check(m.RemoveDecorator(waitNode, 1) && m.FindNode(waitNode)->decorators.size() == 2 && !m.RemoveDecorator(waitNode, 5), "Decorator を消せる");
        while (m.AddDecorator(waitNode, BtDecoratorKind::Invert) >= 0) {
        }
        check(m.FindNode(waitNode)->decorators.size() == static_cast<size_t>(kBtMaxDecoratorsPerNode), "Decorator は 1 ノード kBtMaxDecoratorsPerNode 個まで");
    }

    // ---- 5. 保存 -> 読み直しで位置・順序・パラメータが戻る ----
    {
        check(model.Dirty(), "(前提) ここまでの操作で dirty が立っている");
        const BtSaveCheck pre = model.CheckSavable();
        // 2. の木は SimpleParallel (子 2 つ) と孤立ノードを含むが、木として成り立つ
        check(pre.problem == BtSaveProblem::None, "2. で作った木は保存できる");
        model.SetParam(model.Asset().nodes[1].id, 0, IntValue(77)); // Wait の ticks
        const std::vector<BtNodeDef> expected = model.Asset().nodes;
        const int32_t expectedRoot = model.RootId();
        BtSaveCheck blocked;
        const std::shared_ptr<const BehaviorTreeAsset> beforeSave = trees.GetShared(BehaviorTreeLibrary::HashForPath(mainPath.wstring()));
        check(model.Save(&blocked) == BtSaveResult::Ok && !model.Dirty() && fs::exists(mainPath, ec), "保存するとファイルができ、dirty が倒れる");
        const std::shared_ptr<const BehaviorTreeAsset> afterSave = trees.GetShared(BehaviorTreeLibrary::HashForPath(mainPath.wstring()));
        check(beforeSave != afterSave && model.IsRegistered(), "保存はライブラリへ直接登録する (走っている木は同一性の違いで Abort -> やり直しを検出する)");

        json fromDisk;
        BehaviorTreeAsset reread;
        const bool parsed = ReadJsonFile(mainPath, fromDisk) && BehaviorTreeLibrary::FromJson(fromDisk, reread);
        bool same = parsed && reread.rootId == expectedRoot && reread.nodes.size() == expected.size();
        for (size_t i = 0; same && i < expected.size(); ++i) {
            same = reread.nodes[i].id == expected[i].id && reread.nodes[i].kind == expected[i].kind && reread.nodes[i].childIds == expected[i].childIds
                   && reread.nodes[i].pos[0] == expected[i].pos[0] && reread.nodes[i].pos[1] == expected[i].pos[1]
                   && reread.nodes[i].params.size() == expected[i].params.size();
        }
        check(same && reread.nodes[1].params[0].i == 77, "ファイルを読み直すと、ノード・位置・子の順序・パラメータが保存前と同じ");

        BehaviorTreeEditModel again;
        again.BindLibraries(&trees, &boards);
        again.Load(trees.GetShared(BehaviorTreeLibrary::HashForPath(mainPath.wstring())));
        check(again.IsLoaded() && !again.Dirty() && BehaviorTreeLibrary::ToJson(again.Asset()) == BehaviorTreeLibrary::ToJson(model.Asset()),
              "保存した木を別のモデルで読み直しても同じ内容 (位置を持つ木は整列し直さない)");

        // 別の経路で読み直されたら、登録の同一性が変わる
        BehaviorTreeAsset other = *afterSave;
        trees.Register(mainPath.wstring(), std::move(other));
        check(!model.IsRegistered(), "別の経路で登録し直されると IsRegistered が偽になる (外部の読み直しを窓が検出する)");
    }

    // ---- 6. 保存できない木は書かない ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        const fs::path badPath = root / L"bad.bt.json";
        m.Load(RegisterEmpty(trees, badPath));
        const int32_t parallel = m.AddNode(BtNodeKind::SimpleParallel, 0.0f, 0.0f);
        const int32_t one = m.AddNode(BtNodeKind::Wait, 0.0f, 100.0f);
        m.Connect(parallel, one);
        BtSaveCheck blocked;
        check(m.CheckSavable().problem == BtSaveProblem::ChildCount && m.CheckSavable().nodeId == parallel, "子が 1 つの SimpleParallel は ChildCount で保存できない (ノード id も返す)");
        check(m.Save(&blocked) == BtSaveResult::Blocked && blocked.problem == BtSaveProblem::ChildCount && !fs::exists(badPath, ec) && m.Dirty(),
              "保存できない木はファイルを作らず、dirty のまま");
        const int32_t two = m.AddNode(BtNodeKind::Wait, 100.0f, 100.0f);
        m.Connect(parallel, two);
        check(m.CheckSavable().problem == BtSaveProblem::None && m.Save() == BtSaveResult::Ok && fs::exists(badPath, ec), "直せば保存できる");
        const std::vector<char> beforeBytes = [&] {
            std::ifstream f(badPath, std::ios::binary);
            return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }();
        m.Connect(parallel, m.AddNode(BtNodeKind::Wait, 200.0f, 100.0f)); // 3 つ目は拒まれる
        m.Remove(two, BtRemoveMode::KeepChildren);                        // 子が 1 つになる
        check(m.Save() == BtSaveResult::Blocked, "あとから壊した木の保存も拒む");
        const std::vector<char> afterBytes = [&] {
            std::ifstream f(badPath, std::ios::binary);
            return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }();
        check(beforeBytes == afterBytes, "拒んだ保存は既存のファイルを 1 バイトも変えない");
        BehaviorTreeEditModel unloaded;
        check(unloaded.Save() == BtSaveResult::NotLoaded && unloaded.AddNode(BtNodeKind::Wait, 0.0f, 0.0f) == -1, "読み込んでいないモデルは保存も追加も受けない");
    }

    // ---- 7. 位置を持たない木 (手書き) は読み込みで整列し、位置を持つ木は触らない ----
    {
        const fs::path handPath = root / L"hand.bt.json";
        json hand;
        hand["engine"] = "MyEngine";
        hand["behaviortree"] = 1;
        hand["root"] = 0;
        const auto node = [](int id, const char* type, std::vector<int> kids) {
            json n;
            n["id"] = id;
            n["type"] = type;
            n["children"] = kids;
            return n;
        };
        hand["nodes"] = json::array({ node(0, "Selector", { 1, 2, 3 }), node(1, "Sequence", { 4, 5 }), node(2, "Wait", {}), node(3, "Wait", {}), node(4, "Wait", {}), node(5, "Wait", {}) });
        BehaviorTreeAsset handAsset;
        check(BehaviorTreeLibrary::FromJson(hand, handAsset), "(前提) pos の無い手書きの木を読める");
        trees.Register(handPath.wstring(), std::move(handAsset));
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(trees.GetShared(BehaviorTreeLibrary::HashForPath(handPath.wstring())));
        const BtNodeDef& r = *m.FindNode(0);
        const BtNodeDef& left = *m.FindNode(1);
        const BtNodeDef& mid = *m.FindNode(2);
        const BtNodeDef& right = *m.FindNode(3);
        const BtNodeDef& leafA = *m.FindNode(4);
        const BtNodeDef& leafB = *m.FindNode(5);
        check(!m.Dirty() && left.pos[0] < mid.pos[0] && mid.pos[0] < right.pos[0] && leafA.pos[0] < leafB.pos[0], "位置の無い木は読み込みで整列する (dirty は偽): 兄弟は子の順に左から右");
        check(r.pos[1] < left.pos[1] && left.pos[1] < leafA.pos[1] && mid.pos[1] == left.pos[1] && leafA.pos[1] == leafB.pos[1], "整列は上から下 (段ごとに y が揃う)");
        check(left.pos[0] < r.pos[0] && r.pos[0] < right.pos[0], "整列は親を子の左端と右端の間に置く");
        // 重ならない: 全ノードの箱が互いに交差しない
        bool overlap = false;
        for (size_t i = 0; i < m.Asset().nodes.size(); ++i) {
            for (size_t j = i + 1; j < m.Asset().nodes.size(); ++j) {
                const BtNodeDef& a = m.Asset().nodes[i];
                const BtNodeDef& b = m.Asset().nodes[j];
                const bool apartX = a.pos[0] + kBtNodeWidth <= b.pos[0] || b.pos[0] + kBtNodeWidth <= a.pos[0];
                const bool apartY = a.pos[1] + BehaviorTreeEditModel::NodeHeight(a) <= b.pos[1] || b.pos[1] + BehaviorTreeEditModel::NodeHeight(b) <= a.pos[1];
                overlap = overlap || !(apartX || apartY);
            }
        }
        check(!overlap, "整列した箱は互いに重ならない");
        BehaviorTreeEditModel keep;
        keep.BindLibraries(&trees, &boards);
        keep.Load(trees.GetShared(BehaviorTreeLibrary::HashForPath(mainPath.wstring())));
        check(keep.FindNode(1)->pos[0] == 300.0f && keep.FindNode(2)->pos[0] == 400.0f, "位置を持つ木は整列し直さない");
    }

    // ---- 8. MoveSubtree: 子孫も同じだけ動き、兄弟の順序が x で並べ直る ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"move.bt.json"));
        const int32_t top = m.AddNode(BtNodeKind::Selector, 200.0f, 0.0f);
        const int32_t a = m.AddNode(BtNodeKind::Sequence, 100.0f, 100.0f);
        const int32_t a1 = m.AddNode(BtNodeKind::Wait, 100.0f, 200.0f);
        const int32_t b = m.AddNode(BtNodeKind::Wait, 300.0f, 100.0f);
        m.Connect(top, a);
        m.Connect(top, b);
        m.Connect(a, a1);
        check(ChildrenOf(m, top) == std::vector<int32_t>{ a, b }, "(前提) 順序は a, b");
        check(m.MoveSubtree(a, 400.0f, 10.0f) && m.FindNode(a1)->pos[0] == 500.0f && m.FindNode(a1)->pos[1] == 210.0f && m.FindNode(top)->pos[0] == 200.0f,
              "MoveSubtree は子孫を同じだけ動かし、親・兄弟は動かさない");
        check(ChildrenOf(m, top) == std::vector<int32_t>{ b, a }, "サブツリーが兄弟を追い越すと順序が入れ替わる");
        check(!m.MoveSubtree(a, 0.0f, 0.0f) && !m.MoveNode(a, m.FindNode(a)->pos[0], m.FindNode(a)->pos[1]), "動きが無ければ false");
    }

    // ---- 9. Undo / Redo: 全部戻すと元のバイト列、やり直すと後の状態 ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"undo.bt.json"));
        const std::string original = DumpState(m);
        check(!m.CanUndo() && !m.CanRedo() && !m.Undo() && !m.Redo(), "読み込み直後は Undo も Redo もできない");
        const int32_t top = m.AddNode(BtNodeKind::Selector, 200.0f, 0.0f);
        const int32_t waitA = m.AddNode(BtNodeKind::Wait, 100.0f, 120.0f);
        const int32_t waitB = m.AddNode(BtNodeKind::Wait, 300.0f, 120.0f);
        m.Connect(top, waitA);
        m.Connect(top, waitB);
        m.MoveNode(waitA, 400.0f, 120.0f);
        m.SetParam(waitA, 0, IntValue(99));
        m.AddDecorator(waitB, BtDecoratorKind::Invert);
        m.Remove(waitB, BtRemoveMode::WithDescendants);
        const std::string afterAll = DumpState(m);
        check(m.CanUndo() && !m.CanRedo() && m.Dirty(), "操作のあとは Undo できて、Redo はできない");
        const int undone = UndoAll(m);
        check(undone == 9 && DumpState(m) == original && !m.Dirty(),
              "追加 3 + 接続 2 + 移動 + パラメータ + Decorator + 削除の 9 操作 = 9 段。全部戻すと元のバイト列と一致し、dirty も消える");
        check(m.CanRedo() && !m.CanUndo(), "全部戻すと Redo だけできる");
        check(RedoAll(m) == 9 && DumpState(m) == afterAll && m.Dirty(), "全部やり直すと、操作後の状態にバイト列まで一致する");
        UndoAll(m);
        m.AddNode(BtNodeKind::Sequence, 0.0f, 0.0f);
        check(!m.CanRedo(), "新しい操作をすると Redo の履歴は捨てる");
        check(!m.Connect(99, 98) && !m.SetParam(99, 0, IntValue(1)) && UndoAll(m) == 1, "失敗した操作は履歴に積まない (戻せるのは追加の 1 段だけ)");
    }

    // ---- 9b. ジェスチャ: 複数回の変更を 1 段にする ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"gesture.bt.json"));
        const int32_t node = m.AddNode(BtNodeKind::Wait, 0.0f, 0.0f);
        const std::string before = DumpState(m);
        check(m.BeginGesture() && !m.BeginGesture() && m.InGesture(), "ジェスチャは開く。開いている間にもう一度開いても何もしない");
        for (int i = 1; i <= 20; ++i) {
            m.MoveSubtree(node, 5.0f, 1.0f);
        }
        check(!m.CanUndo() && !m.Undo(), "ジェスチャの途中は Undo / Redo を受けない");
        m.EndGesture();
        check(!m.InGesture() && m.FindNode(node)->pos[0] == 100.0f, "20 回の移動が反映されている");
        check(m.Undo() && DumpState(m) == before, "ジェスチャ 1 回 (20 回の移動) は 1 段で、戻すと開く前の状態");
        check(m.Undo() && m.Asset().nodes.empty(), "その前の段は追加 (ジェスチャが余計な段を積んでいない)");
        check(!m.CanUndo(), "段はこれで全部");
        // 何も変えなかったジェスチャは段を積まない
        m.Redo();
        m.Redo();
        m.BeginGesture();
        m.EndGesture();
        check(m.Undo() && m.FindNode(node)->pos[0] == 0.0f, "変更の無いジェスチャは段を積まない (直前の段が戻る)");
    }

    // ---- 9c. 保存時の内容へ戻ったら dirty が消える。履歴は保存で消えない ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"savedirty.bt.json"));
        m.AddNode(BtNodeKind::Wait, 12.3f, 45.6f);
        check(m.Save() == BtSaveResult::Ok && !m.Dirty(), "(前提) 保存すると dirty が消える");
        {
            // 窓の保存の直後に ReloadHub が同じファイルを読み直しても、登録を置き換えない (走っている木が 2 回やり直さない)
            const std::shared_ptr<const BehaviorTreeAsset> registered = trees.GetShared(BehaviorTreeLibrary::HashForPath((root / L"savedirty.bt.json").wstring()));
            bool unchanged = false;
            check(trees.LoadFromFile(NormalizePathKey((root / L"savedirty.bt.json").wstring()), &unchanged) != 0 && unchanged
                      && trees.GetShared(BehaviorTreeLibrary::HashForPath((root / L"savedirty.bt.json").wstring())) == registered,
                  "窓が保存したファイルの読み直し (小数の位置を含む) は登録と同じ内容と判定され、置き換えない");
        }
        const int32_t extra = m.AddNode(BtNodeKind::Wait, 50.0f, 0.0f);
        check(m.Dirty(), "保存のあとの編集で dirty");
        check(m.Undo() && !m.Dirty() && m.FindNode(extra) == nullptr, "Undo で保存時の内容へ戻ると dirty が消える");
        check(m.Redo() && m.Dirty(), "Redo で保存後の内容へ進むと dirty");
        m.Undo();
        check(m.Undo() && m.Dirty(), "保存より前へ戻ると dirty (保存しても履歴は消えない)");
        // ドラッグして元の位置へ戻した場合も内容は同じ (dirty にならない)
        m.Redo();
        const int32_t only = m.Asset().nodes[0].id;
        m.BeginGesture();
        m.MoveNode(only, 10.0f, 10.0f);
        m.MoveNode(only, 12.3f, 45.6f);
        m.EndGesture();
        check(!m.Dirty(), "ドラッグして元の位置へ戻すと、内容が保存時と同じなので dirty ではない");
    }

    // ---- 9d. Undo は 128 段まで ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"limit.bt.json"));
        for (size_t i = 0; i < kBtMaxUndoSteps + 20; ++i) {
            m.AddNode(BtNodeKind::Wait, static_cast<float>(i), 0.0f);
        }
        check(UndoAll(m) == static_cast<int>(kBtMaxUndoSteps) && m.Asset().nodes.size() == 20, "128 段を超えた古い履歴は捨てる (戻せるのは 128 段)");
    }

    // ---- 9e. 保存できない途中の状態も戻せる (JSON 経由ではなくコピーで持つ理由) ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"midstate.bt.json"));
        const int32_t parallel = m.AddNode(BtNodeKind::SimpleParallel, 0.0f, 0.0f);
        const int32_t a = m.AddNode(BtNodeKind::Wait, 0.0f, 100.0f);
        m.Connect(parallel, a); // 子 1 つの SimpleParallel (保存できない)
        check(m.CheckSavable().problem == BtSaveProblem::ChildCount, "(前提) 子 1 つの SimpleParallel は保存できない状態");
        const std::string broken = DumpState(m);
        m.AddNode(BtNodeKind::Wait, 100.0f, 100.0f);
        check(m.Undo() && DumpState(m) == broken && m.CheckSavable().problem == BtSaveProblem::ChildCount, "保存できない途中の状態へも Undo で戻れる");
        check(m.Redo() && m.Asset().nodes.size() == 3, "Redo で進める");
    }

    // ---- 10. Blackboard の編集と追従、Undo ----
    {
        BlackboardAsset boardAsset;
        BbKeyDef goal;
        goal.name = "Goal";
        goal.type = BbType::Vector;
        BbKeyDef alarm;
        alarm.name = "Alarm";
        alarm.type = BbType::Bool;
        boardAsset.keys = { goal, alarm };
        const fs::path boardPath = root / L"edit.bb.json";
        {
            std::ofstream f(boardPath, std::ios::binary);
            f << BlackboardLibrary::ToJson(boardAsset).dump(2);
        }
        const uint64_t boardGuid = boards.LoadFromFile(boardPath.wstring());
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"board.bt.json"));
        check(m.Board() == nullptr && m.AddBoardKey(BbType::Bool) == -1, "BB を使っていない木では BB の操作を受けない");
        const int32_t top = m.AddNode(BtNodeKind::Selector, 100.0f, 0.0f);
        const int32_t move = m.AddNode(BtNodeKind::MoveTo, 100.0f, 100.0f);
        m.Connect(top, move);
        check(m.SetBlackboard(boardGuid) && m.Board() != nullptr && m.Board()->keys.size() == 2 && !m.BoardDirty(), "BB を割り当てると作業用コピーを読む。BB の dirty は偽");
        m.SetKey(move, btnodekey::kTarget, "Goal");
        const int deco = m.AddDecorator(move, BtDecoratorKind::BlackboardCondition);
        m.SetDecoratorKey(move, deco, "Goal");
        const std::string beforeRename = DumpState(m);

        check(m.RenameBoardKey(0, "Destination") && m.Board()->keys[0].name == "Destination" && m.FindNode(move)->keys[0] == "Destination"
                  && m.FindNode(move)->decorators[0].key == "Destination" && m.BoardDirty() && m.Dirty(),
              "キーの改名: BT のノードのキー欄と Decorator のキーが同じ操作で追従し、木と BB の両方が dirty");
        check(!m.RenameBoardKey(0, "Alarm") && !m.RenameBoardKey(0, "") && !m.RenameBoardKey(0, std::string(64, 'k')) && !m.RenameBoardKey(5, "x"),
              "重複・空・長すぎる名前・存在しないキーへの改名は拒む");
        check(m.Undo() && DumpState(m) == beforeRename && !m.BoardDirty(), "改名の Undo 1 回で、BB と BT のキー参照が元のバイト列へ戻る");
        m.Redo();

        // 型・初期値・eventName
        BbValue init;
        init.isSet = 1;
        init.v[0] = 1.0f;
        init.v[1] = 2.0f;
        init.v[2] = 3.0f;
        check(m.SetBoardKeyInitial(0, init) && m.Board()->keys[0].initial.isSet == 1 && !m.SetBoardKeyInitial(0, init), "Vector の初期値を書ける。同じ値は変更なし");
        BbValue bad = init;
        bad.v[1] = std::nanf("");
        check(!m.SetBoardKeyInitial(0, bad), "非有限の初期値は拒む");
        check(m.SetBoardKeyType(0, BbType::Entity) && m.Board()->keys[0].initial.isSet == 0, "型を変えると初期値は「なし」へ戻る");
        BbValue entityInit;
        entityInit.isSet = 1;
        check(!m.SetBoardKeyInitial(0, entityInit), "Entity のキーはアセットから指せないので初期値を拒む");
        check(m.SetBoardKeyEventName(1, "alarm") && m.Board()->keys[1].eventName == "alarm" && !m.SetBoardKeyEventName(1, "alarm")
                  && !m.SetBoardKeyEventName(1, std::string(64, 'e')),
              "eventName を書ける。同じ値・長すぎる名前は拒む");
        const int added = m.AddBoardKey(BbType::Float);
        const int addedAgain = m.AddBoardKey(BbType::Int);
        check(added == 2 && addedAgain == 3 && m.Board()->keys[2].name == "NewKey" && m.Board()->keys[3].name == "NewKey1", "キーの追加は重ならない名前で付く");

        // 削除: ノードのキー欄は未指定へ、Decorator は名前を残す
        const std::string beforeRemove = DumpState(m);
        check(m.RemoveBoardKey(0) && m.Board()->FindKey("Destination") < 0 && m.FindNode(move)->keys[0].empty() && m.FindNode(move)->decorators[0].key == "Destination",
              "キーの削除: ノードのキー欄は未指定になり、Decorator のキーは名前を残す");
        check(HasIssue(m.Inspect(), BtIssueKind::KeyUnset, move) && HasIssue(m.Inspect(), BtIssueKind::KeyMissing, move), "削除のあと、未設定と「BB に無い」が検査に出る");
        check(m.Undo() && DumpState(m) == beforeRemove, "削除の Undo で、BB のキーとノードのキー欄が元に戻る");

        // 保存
        check(m.BoardDirty() && m.SaveBoard() == BtSaveResult::Ok && !m.BoardDirty(), "BB を保存すると BoardDirty が消える");
        json fromDisk;
        BlackboardAsset reread;
        check(ReadJsonFile(boardPath, fromDisk) && BlackboardLibrary::FromJson(fromDisk, reread) && reread.keys.size() == 4 && reread.keys[0].name == "Destination"
                  && reread.keys[1].eventName == "alarm",
              "保存した BB ファイルを読み直すと、キー・型・eventName が編集どおり");
        check(boards.GetShared(boardGuid)->keys.size() == 4, "保存は BB ライブラリへ直接登録する");
        m.RenameBoardKey(1, "Siren");
        check(m.BoardDirty() && m.Undo() && !m.BoardDirty(), "保存のあとの編集を Undo で保存時の内容へ戻すと BoardDirty が消える");

        // BB の外部の置き換え: 未保存の編集が無ければ読み直し、あれば守る
        m.SyncBoardWithRegistry();
        BlackboardAsset external = *boards.GetShared(boardGuid);
        external.keys.push_back(BbKeyDef{ "External", BbType::Int, BbValue{}, std::string() });
        boards.Register(boardPath.wstring(), external);
        m.SyncBoardWithRegistry();
        check(m.Board()->FindKey("External") >= 0 && !m.BoardDirty(), "BB が外で置き換わり、未保存の編集が無ければ読み直す");
        m.RenameBoardKey(1, "Siren");
        BlackboardAsset external2 = *boards.GetShared(boardGuid);
        external2.keys.push_back(BbKeyDef{ "External2", BbType::Int, BbValue{}, std::string() });
        boards.Register(boardPath.wstring(), external2);
        m.SyncBoardWithRegistry();
        check(m.Board()->FindKey("External2") < 0 && m.Board()->FindKey("Siren") >= 0, "未保存の BB の編集があるときは、外の置き換えで上書きしない");
        m.Undo();

        // 新規 BB: 作って割り当てる (割り当ては Undo の 1 段)
        const fs::path newPath = root / L"fresh.bb.json";
        const uint64_t newGuid = m.CreateBoardFile(newPath.wstring());
        check(newGuid != 0 && fs::exists(newPath, ec) && boards.Contains(newGuid), "新規 BB のファイルを作って登録できる");
        check(m.SetBlackboard(newGuid) && m.Board() != nullptr && m.Board()->keys.empty() && m.AddBoardKey(BbType::Bool) == 0, "新規 BB を木に割り当てると、キーを足せる");
        check(m.Undo() && m.Board() != nullptr && m.Board()->keys.empty() && m.Undo() && m.Board()->FindKey("Alarm") >= 0 && m.Asset().blackboard == boardGuid,
              "割り当ての Undo で元の BB (作業用コピーごと) へ戻る");
    }

    // ---- 11. 検査エラー ----
    {
        BlackboardAsset boardAsset;
        BbKeyDef vec;
        vec.name = "Goal";
        vec.type = BbType::Vector;
        BbKeyDef flag;
        flag.name = "Flag";
        flag.type = BbType::Bool;
        BbKeyDef who;
        who.name = "Who";
        who.type = BbType::Entity;
        boardAsset.keys = { vec, flag, who };
        const uint64_t boardGuid = boards.Register((root / L"inspect.bb.json").wstring(), boardAsset);
        BlackboardAsset otherBoard;
        const uint64_t otherGuid = boards.Register((root / L"other.bb.json").wstring(), otherBoard);

        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"inspect.bt.json"));
        m.SetBlackboard(boardGuid);
        const int32_t top = m.AddNode(BtNodeKind::Selector, 300.0f, 0.0f);
        const int32_t seq = m.AddNode(BtNodeKind::Sequence, 100.0f, 100.0f);
        const int32_t parallel = m.AddNode(BtNodeKind::SimpleParallel, 500.0f, 100.0f);
        const int32_t goodMove = m.AddNode(BtNodeKind::MoveTo, 100.0f, 200.0f);
        m.Connect(top, seq);
        m.Connect(top, parallel);
        m.Connect(seq, goodMove);
        m.SetKey(goodMove, btnodekey::kTarget, "Goal");
        const int32_t waitMain = m.AddNode(BtNodeKind::Wait, 450.0f, 200.0f);
        const int32_t waitBack = m.AddNode(BtNodeKind::Wait, 600.0f, 200.0f);
        m.Connect(parallel, waitMain);
        m.Connect(parallel, waitBack);
        check(m.Inspect().empty(), "問題の無い木 (キーの型が合う MoveTo、Task を左に置いた SimpleParallel) は検査に何も出ない");

        // 子の数
        const int32_t lonelyParallel = m.AddNode(BtNodeKind::SimpleParallel, 800.0f, 100.0f);
        m.Connect(top, lonelyParallel);
        m.Connect(lonelyParallel, m.AddNode(BtNodeKind::Wait, 800.0f, 200.0f));
        check(HasIssue(m.Inspect(), BtIssueKind::ChildCount, lonelyParallel), "子の数の違反 (子 1 つの SimpleParallel)");
        m.Remove(lonelyParallel, BtRemoveMode::WithDescendants);

        // SimpleParallel の左が Task でない
        const int32_t badLeft = m.AddNode(BtNodeKind::Sequence, 0.0f, 300.0f);
        m.Connect(parallel, badLeft); // 3 つ目は拒まれる。先に右を外す
        m.Disconnect(waitMain);
        check(m.Connect(parallel, badLeft) && m.FindNode(parallel)->childIds.front() == badLeft, "(前提) x が一番左の Sequence が SimpleParallel の左 (メイン) になる");
        m.MoveNode(badLeft, 450.0f, 300.0f);
        check(HasIssue(m.Inspect(), BtIssueKind::ParallelMainNotTask, parallel), "SimpleParallel の左が Task でない (Composite)");
        m.Remove(badLeft, BtRemoveMode::WithDescendants);
        m.Connect(parallel, waitMain);
        check(m.Inspect().empty(), "(前提) 直すと検査は空");

        // LowerPriority / Both の位置
        const int deco = m.AddDecorator(goodMove, BtDecoratorKind::BlackboardCondition);
        m.SetDecoratorKey(goodMove, deco, "Flag");
        m.SetDecoratorParam(goodMove, deco, btbbparam::kAbort, IntValue(btabort::kLowerPriority));
        check(HasIssue(m.Inspect(), BtIssueKind::LowerPriorityParent, goodMove), "親が Sequence の LowerPriority は検査エラー");
        m.SetDecoratorParam(goodMove, deco, btbbparam::kAbort, IntValue(btabort::kBoth));
        check(HasIssue(m.Inspect(), BtIssueKind::LowerPriorityParent, goodMove), "親が Sequence の Both も検査エラー");
        m.SetDecoratorParam(goodMove, deco, btbbparam::kAbort, IntValue(btabort::kSelf));
        check(m.Inspect().empty(), "Self なら親が Sequence でもよい");
        const int selectorDeco = m.AddDecorator(seq, BtDecoratorKind::BlackboardCondition);
        m.SetDecoratorKey(seq, selectorDeco, "Flag");
        m.SetDecoratorParam(seq, selectorDeco, btbbparam::kAbort, IntValue(btabort::kLowerPriority));
        check(!HasIssue(m.Inspect(), BtIssueKind::LowerPriorityParent, seq), "親が Selector の LowerPriority は問題なし");
        m.RemoveDecorator(seq, selectorDeco);
        m.RemoveDecorator(goodMove, deco);

        // キー: 未設定・無い・型違い
        const int32_t unsetMove = m.AddNode(BtNodeKind::MoveTo, 0.0f, 400.0f);
        check(HasIssue(m.Inspect(), BtIssueKind::KeyUnset, unsetMove), "必須のキー欄が空 (MoveTo の target) は未設定");
        m.SetKey(unsetMove, btnodekey::kTarget, "Nope");
        check(HasIssue(m.Inspect(), BtIssueKind::KeyMissing, unsetMove), "BB に無い名前を指す");
        m.SetKey(unsetMove, btnodekey::kTarget, "Flag");
        check(HasIssue(m.Inspect(), BtIssueKind::KeyTypeMismatch, unsetMove), "型が合わないキー (MoveTo の target に Bool) は型違い (手書きの木に残る型違いも拾う)");
        m.SetKey(unsetMove, btnodekey::kTarget, "Who");
        check(!HasIssue(m.Inspect(), BtIssueKind::KeyTypeMismatch, unsetMove) && !HasIssue(m.Inspect(), BtIssueKind::KeyUnset, unsetMove), "Entity は MoveTo の target に使える");
        const int32_t findRandom = m.AddNode(BtNodeKind::FindRandomPoint, 0.0f, 500.0f);
        m.SetKey(findRandom, btrandomkey::kResult, "Goal");
        check(!HasIssue(m.Inspect(), BtIssueKind::KeyUnset, findRandom), "任意のキー欄 (FindRandomPoint の center) が空でも問題なし");
        const int32_t decoWait = m.AddNode(BtNodeKind::Wait, 0.0f, 600.0f);
        const int d2 = m.AddDecorator(decoWait, BtDecoratorKind::BlackboardCondition);
        m.SetDecoratorKey(decoWait, d2, "Flag");
        m.RenameBoardKey(1, "Renamed"); // 追従する
        check(!HasIssue(m.Inspect(), BtIssueKind::KeyMissing, decoWait), "BB のキーの改名に Decorator のキーが追従して検査に出ない");
        m.RemoveBoardKey(1);
        check(HasIssue(m.Inspect(), BtIssueKind::KeyMissing, decoWait), "BB のキーを消すと、Decorator のキー参照が「BB に無い」として出る");

        // BB を外すと全部「無い」
        m.SetBlackboard(0);
        check(HasIssue(m.Inspect(), BtIssueKind::KeyMissing, goodMove), "BB を使わない木のキー参照は「BB に無い」");
        m.SetBlackboard(boardGuid);

        // SubTree
        const int32_t sub = m.AddNode(BtNodeKind::SubTree, 900.0f, 200.0f);
        check(HasIssue(m.Inspect(), BtIssueKind::SubTreeUnresolved, sub) && m.Inspect().back().severity == BtIssueSeverity::Warning,
              "取り込む木が未指定の SubTree は警告");
        BehaviorTreeAsset subAsset;
        BtNodeDef subRoot;
        subRoot.id = 0;
        subRoot.kind = BtNodeKind::Wait;
        subRoot.params = { IntValue(10), FloatValue(0.0f) };
        subAsset.nodes.push_back(subRoot);
        subAsset.rootId = 0;
        subAsset.blackboard = otherGuid;
        const uint64_t subGuid = trees.Register((root / L"sub.bt.json").wstring(), subAsset);
        BtParamValue guidValue;
        guidValue.u = subGuid;
        m.SetParam(sub, btsubtreeparam::kTree, guidValue);
        check(HasIssue(m.Inspect(), BtIssueKind::SubTreeBoardMismatch, sub), "取り込む木の BB が違う SubTree は検査エラー (BtExpandSubTrees が Failure にする条件)");
        subAsset.blackboard = boardGuid;
        trees.Register((root / L"sub.bt.json").wstring(), subAsset);
        check(!HasIssue(m.Inspect(), BtIssueKind::SubTreeBoardMismatch, sub) && !HasIssue(m.Inspect(), BtIssueKind::SubTreeUnresolved, sub), "BB が同じなら問題なし");
        subAsset.rootId = -1;
        trees.Register((root / L"sub.bt.json").wstring(), subAsset);
        check(HasIssue(m.Inspect(), BtIssueKind::SubTreeUnresolved, sub), "根の無い木を取り込む SubTree は警告");

        // 検査は保存を止めない
        check(!m.Inspect().empty() && m.CheckSavable().problem == BtSaveProblem::None, "検査エラーがあっても保存はできる (止めるのは CheckSavable だけ)");
        BehaviorTreeEditModel unloaded;
        check(unloaded.Inspect().empty(), "読み込んでいないモデルの検査は空");
    }

    // ---- 12. CppTask のフィールド (M85k): 保存 -> 読み直しで残り、Undo で戻る ----
    {
        BehaviorTreeEditModel m;
        m.BindLibraries(&trees, &boards);
        m.Load(RegisterEmpty(trees, root / L"cpptask.bt.json"));
        const int32_t task = m.AddNode(BtNodeKind::CppTask, 0.0f, 0.0f);
        const int32_t wait = m.AddNode(BtNodeKind::Wait, 0.0f, 100.0f);
        BtParamValue name;
        name.s = "ProbeTask";
        check(task >= 0 && m.SetParam(task, btcpptaskparam::kTask, name) && m.FindNode(task)->params[btcpptaskparam::kTask].s == "ProbeTask",
              "CppTask のタスク名は String のパラメータとして編集できる");
        check(m.SetTaskField(task, "targetTicks", nlohmann::json(7)) && m.SetTaskField(task, "label", nlohmann::json("go"))
                  && m.FindNode(task)->taskFields["targetTicks"] == 7,
              "SetTaskField: フィールドの値を名前で書ける");
        check(!m.SetTaskField(task, "targetTicks", nlohmann::json(7)) && !m.SetTaskField(task, "x", nlohmann::json::object())
                  && !m.SetTaskField(task, "", nlohmann::json(1)) && !m.SetTaskField(wait, "ticks", nlohmann::json(1)),
              "同じ値・オブジェクト・空の名前・CppTask でないノードは false");
        check(m.Undo() && m.FindNode(task)->taskFields.contains("targetTicks") && !m.FindNode(task)->taskFields.contains("label")
                  && m.Undo() && m.FindNode(task)->taskFields.is_null(),
              "Undo でフィールドの変更が 1 つずつ戻る");
        check(m.Redo() && m.Redo() && m.FindNode(task)->taskFields["label"] == "go", "Redo でやり直せる");
        check(m.Save() == BtSaveResult::Ok, "CppTask を含む木を保存できる");
        BehaviorTreeEditModel again;
        again.BindLibraries(&trees, &boards);
        const auto reloaded = trees.GetShared(BehaviorTreeLibrary::HashForPath((root / L"cpptask.bt.json").wstring()));
        check(again.Load(reloaded) && again.FindNode(task) != nullptr && again.FindNode(task)->params[btcpptaskparam::kTask].s == "ProbeTask"
                  && again.FindNode(task)->taskFields["targetTicks"] == 7 && again.FindNode(task)->taskFields["label"] == "go",
              "保存した木を読み直すとタスク名とフィールドが残っている");
    }

    fs::remove_all(root, ec);

    if (failCount == 0) {
        MYE_LOG_INFO("BehaviorTreeEditModel self test: PASS");
    } else {
        MYE_LOG_ERROR("BehaviorTreeEditModel self test: %d FAIL", failCount);
    }
    return failCount == 0;
}

} // namespace mye
