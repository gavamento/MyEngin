//====================================================================================
//                          BehaviorTreeEditModel.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          BT 窓が編集する木のモデル (ImGui に依存しない操作の集まり)
//====================================================================================
#pragma once
#include <cstdint>
#include <deque>
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

constexpr size_t kBtMaxUndoSteps = 128; // Undo / Redo の段数の上限

// 検査エラーの種類 (Inspect)。保存は止めない (止めるのは CheckSavable だけ)。実行時にそのノードが Failure になる / 意味を持たないもの
enum class BtIssueKind : uint8_t {
    ChildCount,           // 子の数が種類の範囲から外れている
    ParallelMainNotTask,  // SimpleParallel の左 (メイン) の子が Task でない。otherId = その子
    LowerPriorityParent,  // 親が Selector でないノードの LowerPriority / Both。実行時は Self 扱い
    SubTreeBoardMismatch, // 取り込む木の BB がこの木の BB と違う (実行時 Failure)
    SubTreeUnresolved,    // 取り込む木が未指定・未登録・根なし (実行時 Failure)
    KeyUnset,             // 必要なキー欄が空
    KeyMissing,           // BB に無い名前を指している (BB 未設定を含む)
    KeyTypeMismatch,      // キーの型がこの欄に合わない
    CSharpTask,           // C# のタスクを含む (決定論の保証外)。C# タスクのノード種が入るまで出ない
};

enum class BtIssueSeverity : uint8_t { Warning, Error };

struct BtIssue {
    BtIssueKind kind = BtIssueKind::ChildCount;
    BtIssueSeverity severity = BtIssueSeverity::Error;
    int32_t nodeId = -1;
    int decoratorIndex = -1; // Decorator のキーの問題のときその添字。-1 = ノード本体
    int keyIndex = -1;       // キー欄の問題のときその添字
    int32_t otherId = -1;    // 関連するもう一つのノード (ParallelMainNotTask の左の子)
};

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
    // 木が保存したときの内容から変わっているか。Undo / Redo で保存時の内容へ戻れば偽になる
    bool Dirty() const;
    // BB (Board()) が登録されている内容から変わっているか
    bool BoardDirty() const;

    // ---- 参照 ----
    int FindIndex(int32_t id) const { return asset_.FindNode(id); }
    const BtNodeDef* FindNode(int32_t id) const;
    int32_t ParentOf(int32_t id) const;  // 親の id。親なし (根・孤立) と存在しない id は -1
    int ChildOrderOf(int32_t id) const;  // 親の子の中での 0 始まりの順。親なしは -1
    int32_t RootId() const { return asset_.rootId; }
    // 編集中の BB (asset_.blackboard の登録を写した作業用コピー。BB の操作と Undo はこれに掛かる)。未設定・未登録なら null
    const BlackboardAsset* Board() const { return boardLoaded_ ? &board_ : nullptr; }
    // 変更のたびに増える通し番号 (窓が検査結果などの再計算の要否を決める)
    uint64_t Revision() const { return revision_; }

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
    // CppTask のフィールド (.bt.json の "fields")。値は BtTaskReadField が作る JSON。同じ値・CppTask でないノード・保存できない形は false
    bool SetTaskField(int32_t id, const std::string& name, const nlohmann::json& value);
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

    // ---- Blackboard の編集 (Board() の作業用コピーに掛かる。BT の保存とは別に SaveBoard で書く) ----
    // BB の登録が変わっていて作業用コピーに未保存の編集が無ければ、登録の内容へ読み直す (履歴には積まない)。
    // まだ読めていなかった BB が登録されたときも読む。窓が毎フレーム呼ぶ
    void SyncBoardWithRegistry();
    // 型 type の新しいキーを足して添字を返す (名前は自動。-1 = 上限か BB なし)
    int AddBoardKey(BbType type);
    // キーを消す。BT のノードのキー欄がそのキーを指していれば未指定にする (Decorator のキーは空にできないので残り、検査が「無い」と出す)
    bool RemoveBoardKey(int index);
    // キーの改名。BT のノードと Decorator が指している名前も同じ操作で付け替える。空・長すぎ・重複は拒む
    bool RenameBoardKey(int index, const std::string& name);
    // 型を変える。初期値は「なし」へ戻る
    bool SetBoardKeyType(int index, BbType type);
    // 初期値。isSet = 0 で「なし」。Entity はアセットから指せないので拒む。非有限の Float / Vector も拒む
    bool SetBoardKeyInitial(int index, const BbValue& value);
    bool SetBoardKeyEventName(int index, const std::string& eventName);
    // 空の BB ファイルを path に作って登録し、GUID を返す (0 = 失敗)。使うには SetBlackboard を呼ぶ
    uint64_t CreateBoardFile(const std::wstring& path);
    // BB を保存する (ファイルへ書く -> 登録)。走っている木のブラックボードは次の tick に初期値へ戻る
    BtSaveResult SaveBoard();

    // ---- Undo / Redo ----
    // 1 つの操作 (AddNode / Connect / Remove / SetParam ...) が 1 段。BT と BB の両方を前後の状態のコピーで持つ
    // (JSON にしないのは、子が足りない SimpleParallel のような保存できない途中の状態も戻せるように)。
    // 保存しても履歴は消さない。Load / Clear で空になる
    bool CanUndo() const { return !gesture_ && !undo_.empty(); }
    bool CanRedo() const { return !gesture_ && !redo_.empty(); }
    bool Undo();
    bool Redo();
    // 複数回の変更を 1 段にまとめる (ノードのドラッグ、DragFloat など)。開いている間の最初の変更で 1 段だけ積む。
    // 戻り値 = 今開いたか (既に開いていれば false で何もしない)
    bool BeginGesture();
    void EndGesture();
    bool InGesture() const { return gesture_; }

    // ---- 検査 ----
    // 木と BB を調べた結果 (ノード順。保存は止めない)。窓が赤枠と一覧に使う
    std::vector<BtIssue> Inspect() const;
    static bool IsTaskKind(BtNodeKind kind); // SimpleParallel の左に置ける (Composite でも SubTree でもない)

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
    // 変更後に呼ぶ。Undo の 1 段を積む (ジェスチャ中は最初の 1 回だけ)
    void Touch();
    void LoadBoardFromRegistry();      // asset_.blackboard の登録を作業用コピーへ。履歴には触らない
    void BindBoardRegistered(std::shared_ptr<const BlackboardAsset> registered);
    void RenameKeyReferences(const std::string& from, const std::string& to);

    struct EditSnapshot {
        BehaviorTreeAsset asset;
        BlackboardAsset board;
        bool boardLoaded = false;
    };
    EditSnapshot Capture() const;
    void Restore(EditSnapshot&& snapshot);
    void PushUndo(const EditSnapshot& snapshot);

    BehaviorTreeAsset asset_;
    BlackboardAsset board_;                                  // 編集中の BB (boardLoaded_ のとき有効)
    std::shared_ptr<const BlackboardAsset> boardRegistered_; // board_ の元になった登録 (同一性の比較用)
    std::string savedBtJson_;                                // 保存 (読み込み) 時点の木の JSON。Dirty の比較元
    std::string savedBoardJson_;                             // boardRegistered_ の JSON。BoardDirty の比較元
    std::deque<EditSnapshot> undo_;
    std::deque<EditSnapshot> redo_;
    EditSnapshot committed_;                                 // 直前の操作が終わった時点の状態 (次の操作の「前」として積む)
    bool gesture_ = false;
    bool gesturePushed_ = false;
    bool restoring_ = false;
    uint64_t revision_ = 0;
    mutable bool btDirtyValid_ = false;
    mutable bool btDirty_ = false;
    mutable bool boardDirtyValid_ = false;
    mutable bool boardDirty_ = false;
    bool boardLoaded_ = false;
    std::shared_ptr<const BehaviorTreeAsset> registered_; // load / save したときの登録 (同一性の比較用)
    BehaviorTreeLibrary* trees_ = nullptr;
    BlackboardLibrary* boards_ = nullptr;
    bool loaded_ = false;
};

} // namespace mye
