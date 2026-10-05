//====================================================================================
//                          BehaviorTreeEditModel.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          BT 窓が編集する木のモデル (ImGui に依存しない操作の集まり)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Engine/Engine/AI/BehaviorTreeLibrary.h"
#include "Engine/Engine/AI/BlackboardLibrary.h"

namespace mye {

// 箱の寸法とレイアウトの間隔 (グラフ座標。キャンバスはこれにズームを掛けて描く)。位置 (pos) は箱全体の左上
constexpr float kBtNodeWidth = 190.0f;
constexpr float kBtNodeBodyHeight = 50.0f;
constexpr float kBtDecoratorBandHeight = 20.0f; // Decorator 1 つぶんの帯 (箱の上に積む)
constexpr float kBtLayoutGapX = 36.0f;          // 兄弟の箱の間
constexpr float kBtLayoutGapY = 56.0f;          // 親の下端から子の上端まで

// ノードの削除の仕方
enum class BtRemoveMode : uint8_t {
    WithDescendants, // 子孫ごと消す
    KeepChildren,    // そのノードだけ消す。子は親なしの根として残る (位置はそのまま)
};

// 保存できない理由
enum class BtSaveProblem : uint8_t {
    None,
    ChildCount, // ノード nodeId の子の数が種類の範囲 (SimpleParallel は 2 つ) から外れている
    Structure,  // それ以外で木として成り立たない (BtLinkAsset が拒む)
};

struct BtSaveCheck {
    BtSaveProblem problem = BtSaveProblem::None;
    int32_t nodeId = -1;
};

enum class BtSaveResult : uint8_t {
    Ok,
    NotLoaded,
    Blocked,     // CheckSavable が None でない
    WriteFailed, // ファイルへ書けなかった (既存のファイルは触らない)
};

// BT 窓が編集中の木 1 本。ノード・位置・接続・パラメータの操作と保存を持ち、ImGui に依存しない
// (BehaviorTreeEditorSelfTest が機械検査する)。操作は全部、成功したら dirty を立てて true を返す。
// 親子の関係は childIds が正本 (BehaviorTreeAsset の導出値 children / parent / slot は保存のときにだけ作り直す)。
// 子の順序は x の左から (Connect と移動のたびに並べ直す)
class BehaviorTreeEditModel {
public:
    // 使うライブラリ。保存の登録先 (trees) と BB の引き先 (boards)。どちらも null 可 (null の間は保存の登録・BB の候補が無い)
    void BindLibraries(BehaviorTreeLibrary* trees, BlackboardLibrary* boards)
    {
        trees_ = trees;
        boards_ = boards;
    }

    // ---- 読み込み ----
    // 登録済みの木を編集用に写す。位置を持たない木 (全ノードが同じ位置) は整列する。dirty は偽
    bool Load(std::shared_ptr<const BehaviorTreeAsset> registered);
    bool IsLoaded() const { return loaded_; }
    void Clear();
    // 登録が load したときのままか。別の経路で読み直されていたら false
    bool IsRegistered() const;

    const BehaviorTreeAsset& Asset() const { return asset_; }
    bool Dirty() const { return dirty_; }

    // ---- 参照 ----
    int FindIndex(int32_t id) const { return asset_.FindNode(id); }
    const BtNodeDef* FindNode(int32_t id) const;
    int32_t ParentOf(int32_t id) const;  // 親の id。親なし (根・孤立) と存在しない id は -1
    int ChildOrderOf(int32_t id) const;  // 親の子の中での 0 始まりの順。親なしは -1
    int32_t RootId() const { return asset_.rootId; }
    const BlackboardAsset* Board() const; // asset_.blackboard の BB。未設定・未登録なら null

    // 種類 kind のノードが持てる子の数の上限 (SubTree は実行時に展開されるのでファイルでは 0)
    static int MaxChildren(BtNodeKind kind);
    // 箱の高さ (Decorator の帯を含む)
    static float NodeHeight(const BtNodeDef& node);
    // 種類 kind の keyIndex 番目のキー欄に選べる BB のキーの型 (パラメータ欄の絞り込み)
    static bool KeyAccepts(BtNodeKind kind, int keyIndex, BbType type);

    // ---- ノード・接続の操作 ----
    // 位置 (x, y) に新しいノードを足す。ノード数の上限なら -1。木に根が無ければ根にする
    int32_t AddNode(BtNodeKind kind, float x, float y);
    // child を parent の子にする (child が別の親を持っていれば付け替える)。
    // 拒む: 存在しない / 同じノード / child が根 / parent の子が上限 / 循環 / 深さが kBtMaxDepth を超える
    bool Connect(int32_t parentId, int32_t childId);
    bool Disconnect(int32_t childId);
    bool Remove(int32_t id, BtRemoveMode mode);
    // 親を持たないノードを根にする (元の根は孤立したノードになる)
    bool SetRoot(int32_t id);
    // 位置を動かす。MoveSubtree は子孫も同じだけ動かす。どちらも親の子の順を x で並べ直す
    bool MoveNode(int32_t id, float x, float y);
    bool MoveSubtree(int32_t id, float dx, float dy);
    // 全ノードを整列し直す
    void AutoLayout();

    // ---- パラメータ・キー・Decorator ----
    // 範囲外の値は丸める。値が変わらなければ false (dirty は立てない)。型に合わない値 (長すぎる文字列・非有限の Float) は false
    bool SetParam(int32_t id, int paramIndex, const BtParamValue& value);
    // BB のキー名。空 = 未指定。63 バイトを超えれば false
    bool SetKey(int32_t id, int keyIndex, const std::string& name);
    // Decorator を末尾に足して添字を返す。-1 = 上限 (kBtMaxDecoratorsPerNode) か、BlackboardCondition なのに BB にキーが無い
    int AddDecorator(int32_t id, BtDecoratorKind kind);
    bool RemoveDecorator(int32_t id, int index);
    bool MoveDecorator(int32_t id, int from, int to);
    bool SetDecoratorParam(int32_t id, int decoratorIndex, int paramIndex, const BtParamValue& value);
    bool SetDecoratorKey(int32_t id, int decoratorIndex, const std::string& name);
    // 使う BB (GUID。0 = なし)。ノードのキー名は消さない (BB に無いキーは窓が「見つからない」と出す)
    bool SetBlackboard(uint64_t guid);

    // ---- 保存 ----
    BtSaveCheck CheckSavable() const;
    // 検査 -> ファイルへ書く (書き切れたときだけ置き換え) -> ライブラリへ登録 (走っている木は spec 4.1.9 のやり直し)
    BtSaveResult Save(BtSaveCheck* blocked = nullptr);

private:
    int ParentIndexOf(int index) const;
    int32_t NextId() const;
    void RemoveFromParent(int32_t childId);
    void SortChildrenByX(int32_t parentId);
    void CollectSubtree(int32_t id, std::vector<int32_t>& out) const;
    int DepthOf(int32_t id) const;     // 根 (親なし) までの段数
    int HeightOf(int32_t id) const;    // 一番深い子孫までの段数 (葉 = 0)
    void LayoutAll();                  // dirty は触らない
    void Touch() { dirty_ = true; }

    BehaviorTreeAsset asset_;
    std::shared_ptr<const BehaviorTreeAsset> registered_; // load / save したときの登録 (同一性の比較用)
    BehaviorTreeLibrary* trees_ = nullptr;
    BlackboardLibrary* boards_ = nullptr;
    bool loaded_ = false;
    bool dirty_ = false;
};

} // namespace mye
