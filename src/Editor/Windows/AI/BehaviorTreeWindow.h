//====================================================================================
//                          BehaviorTreeWindow.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          ビヘイビアツリーのグラフエディタ窓 (M85。キャンバス・パレット・パラメータ欄)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Editor/Windows/AI/BehaviorTreeEditModel.h"
#include "imgui.h"

namespace mye {

struct EngineContext;
class BtTaskRegistry;
struct Selection;

// BT 窓。編集するのは BehaviorTreeEditModel (ImGui に依存しない) で、この窓はそれを描き、マウスとキーを操作へ変える。
// 保存は BehaviorTreeLibrary へ直接登録する (ReloadHub の再読込を待たない)。
// ライブ表示: 選んだエンティティの実行状態を毎フレーム BehaviorTreeSystem から取り直して重ねる (Play 中もタイムラインの巻き戻し中も同じ)
class BehaviorTreeWindow {
public:
    bool open = false; // ツール窓なので既定は非表示 (Window メニューか .bt.json のダブルクリックで開く)

    void OnImGui(EngineContext& ctx, const Selection& selection);

    // Asset Browser で .bt.json がダブルクリックされたとき。未登録なら読み込んで登録する。
    // 今の木に未保存の変更があれば、確認 (保存して開く / 捨てて開く / キャンセル) を挟む
    void OpenAsset(const std::wstring& path);

    // Source Control の書き込み操作のゲート (GateBlocker::BehaviorTreeDirty) が見る。木と BB のどちらも含む
    bool HasUnsavedChanges() const { return model_.Dirty() || model_.BoardDirty(); }

private:
    enum class DragMode : uint8_t { None, Pan, Node, Connect };
    enum class StatusKind : uint8_t { None, Saved, BlockedChildren, BlockedStructure, SaveFailed, LoadFailed, BoardSaved, BoardSaveFailed };
    // Undo の 1 段にまとめている操作の持ち主。Canvas = ノードのドラッグ、Widget = パラメータ欄のドラッグ・文字入力
    enum class GestureOwner : uint8_t { None, Canvas, Widget };

    // キャンバスの座標系 (グラフ座標 <-> 画面)。描画・当たり判定・ライブ表示が同じ変換を使う
    struct CanvasView {
        ImVec2 min = ImVec2(0.0f, 0.0f);    // キャンバスの画面上の左上
        ImVec2 size = ImVec2(0.0f, 0.0f);
        ImVec2 pan = ImVec2(40.0f, 40.0f);  // グラフの原点のキャンバス内位置 (px)
        float zoom = 1.0f;

        ImVec2 ToScreen(float gx, float gy) const { return ImVec2(min.x + pan.x + gx * zoom, min.y + pan.y + gy * zoom); }
        ImVec2 ToGraph(const ImVec2& p) const { return ImVec2((p.x - min.x - pan.x) / zoom, (p.y - min.y - pan.y) / zoom); }
        // ノードの箱 (Decorator の帯を含む) の画面上の左上と右下
        void RectOf(const BtNodeDef& node, ImVec2& outMin, ImVec2& outMax) const
        {
            outMin = ToScreen(node.pos[0], node.pos[1]);
            outMax = ImVec2(outMin.x + kBtNodeWidth * zoom, outMin.y + BehaviorTreeEditModel::NodeHeight(node) * zoom);
        }
    };

    // ライブ表示の読み値 (1 フレームぶん。実行状態の持ち主は BehaviorTreeSystem で、ここは持たない)。
    // id は表示中の木 (model_) の id。SubTree の展開で振り直された実行木の id は、元の木の (GUID, 元の id) を通して戻してある
    struct LiveView {
        bool hasEntity = false;          // 選んだエンティティが BehaviorTreeComponent を持つ
        bool running = false;            // 実行状態が引けた (Play 中・巻き戻し中)
        bool otherTree = false;          // 表示中の木はそのエンティティの木でも取り込んでいる部分木でもない
        uint64_t entityTree = 0;         // BehaviorTreeComponent.tree
        std::string entityName;
        std::unordered_set<int32_t> runningIds; // 実行中のノード (緑の太枠)
        std::unordered_set<int32_t> insideIds;  // 取り込んだ部分木の中で実行中の SubTree ノード
        int32_t abortFrom = -1;          // 直前の Abort: Decorator の付いたノード
        int32_t abortTo = -1;            // 止められたタスク (abortFrom と同じなら矢印でなく枠)
        float abortAlpha = 0.0f;         // 1 -> 0 (30 tick で薄れる)。0 = 出さない
        std::vector<std::string> boardValues; // BB パネルのキーの添字 -> 現在値の文字列。空 = 出さない
    };

    // マウスの下にあるもの
    struct Hit {
        int32_t node = -1;
        bool outputPin = false; // 箱の下の接続の点
    };

    // 右クリックで開くメニュー
    struct MenuRequest {
        bool node = false;
        bool canvas = false;
    };

    // 描画の色と寸法 (ズームと文字の大きさから 1 フレームに 1 回作る)
    struct CanvasStyle {
        float fontSize = 0.0f;
        bool drawText = false;
        float rounding = 0.0f;
        float pinRadius = 0.0f;
        ImU32 text = 0;
        ImU32 dim = 0;
        ImU32 line = 0;
        ImU32 accent = 0;
        ImU32 root = 0;
        ImU32 orphan = 0;
        ImU32 error = 0;
        ImU32 warning = 0;
    };

    void RequestOpen(uint64_t guid); // 未保存の変更があれば確認を挟んでから OpenGuid
    void OpenGuid(uint64_t guid);
    void ReloadFromRegistry();
    void DoSave();
    void DoSaveBoard();
    void DoUndo();
    void DoRedo();
    void DrawToolbar();
    void DrawLeftPanel();
    void DrawPalette(float height);
    void DrawProperties();
    void DrawTreeSettings();
    void DrawBoardPanel();
    bool DrawBoardKey(int index); // 消すよう求められたら true
    void CreateBoard();
    void DrawNodeProperties(int32_t id);
    // CppTask のタスク名の選択欄と、そのタスクのフィールド (登録表の記述子から自動生成)
    void DrawTaskPicker(int32_t id, const BtNodeDef& node);
    void DrawTaskFields(int32_t id);
    bool DrawParam(const BtParamDesc& desc, BtParamValue& value);
    // BB のキーの選択欄。accepts を満たす型のキーだけ並べる。選んだ名前を chosen へ (空 = 未指定)。選んだら true
    bool DrawKeyCombo(const char* label, const std::string& current, bool allowNone, const std::function<bool(BbType)>& accepts,
                      std::string& chosen);
    // 編集が確定したときだけ値を返す文字欄 (入力の途中の文字列を操作として積まない)。slot = 欄の識別
    bool DrawCommitText(const char* label, const std::string& current, int slot, std::string& committed);
    // 欄の変更を Undo の 1 段にする。ドラッグ・文字入力の間は 1 つの操作へまとめる (終わりは FinishWidgetGesture)
    void CommitEdit(bool changed, const std::function<void()>& apply);
    void FinishWidgetGesture(); // 触っている欄が無くなったら、まとめていた操作を閉じる

    void RefreshLive(EngineContext& ctx, const Selection& selection);
    const BtTaskRegistry* tasks_ = nullptr; // CppTask の登録表 (OnImGui のたびに EngineContext から取り直す)
    void DrawLiveBar();
    void DrawLiveAbort(ImDrawList* dl, const CanvasStyle& style);
    void DrawCanvas();
    void DrawIssuePanel();
    MenuRequest HandleCanvasInput(bool hovered);
    void HandleKeys();
    void DrawGrid(ImDrawList* dl);
    void DrawEdges(ImDrawList* dl, const CanvasStyle& style);
    void DrawConnectPreview(ImDrawList* dl, const CanvasStyle& style);
    void DrawNodes(ImDrawList* dl, const CanvasStyle& style, const Hit& hover);
    void DrawCanvasMenus(const MenuRequest& request);
    CanvasStyle MakeStyle() const;
    Hit HitTest(const ImVec2& p) const;
    void RefreshIssues();
    void FocusNode(int32_t id);
    void DrawUnsavedModal();
    void FitView();
    ImVec2 CanvasCenterGraph() const;
    void AddNodeAt(BtNodeKind kind, float gx, float gy);
    void DeleteSelected(BtRemoveMode mode);

    BehaviorTreeEditModel model_;
    int32_t selected_ = -1;       // 選んでいるノードの id。-1 = なし
    CanvasView view_;
    bool needFit_ = false;        // 次に描くとき全体が入るよう表示を合わせる

    DragMode drag_ = DragMode::None;
    int32_t dragNode_ = -1;
    int32_t contextNode_ = -1;
    ImVec2 contextGraphPos_ = ImVec2(0.0f, 0.0f);
    GestureOwner gestureOwner_ = GestureOwner::None;

    LiveView live_;

    // 検査 (model_.Inspect の結果。木が変わったときだけ作り直す)
    std::vector<BtIssue> issues_;
    std::unordered_map<int32_t, BtIssueSeverity> issueSeverityOfNode_; // 赤枠・橙枠の対象 (重い方)
    uint64_t issuesRevision_ = 0;
    bool issuesValid_ = false;

    // 文字欄の編集中の内容 (確定するまで model_ へ渡さない)
    int textSlot_ = -1;
    char textBuf_[kBbMaxNameBytes + 1] = {};

    StatusKind status_ = StatusKind::None;
    int32_t statusNode_ = -1;
    std::string statusText_;      // Saved / LoadFailed の名前

    uint64_t pendingGuid_ = 0;    // 未保存の確認の後に開く木
    bool openModal_ = false;
};

} // namespace mye
