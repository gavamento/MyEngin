//====================================================================================
//                          BehaviorTreeWindow.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          ビヘイビアツリーのグラフエディタ窓 (M85。キャンバス・パレット・パラメータ欄)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "Editor/Windows/AI/BehaviorTreeEditModel.h"
#include "imgui.h"

namespace mye {

// BT 窓。編集するのは BehaviorTreeEditModel (ImGui に依存しない) で、この窓はそれを描き、マウスとキーを操作へ変える。
// 保存は BehaviorTreeLibrary へ直接登録する (ReloadHub の再読込を待たない)。Undo / Redo・BB の編集・検査エラー・
// ライブ表示は後続のサブで足す
class BehaviorTreeWindow {
public:
    bool open = false; // ツール窓なので既定は非表示 (Window メニューか .bt.json のダブルクリックで開く)

    void OnImGui();

    // Asset Browser で .bt.json がダブルクリックされたとき。未登録なら読み込んで登録する。
    // 今の木に未保存の変更があれば、確認 (保存して開く / 捨てて開く / キャンセル) を挟む
    void OpenAsset(const std::wstring& path);

    // Source Control の書き込み操作のゲート (GateBlocker::BehaviorTreeDirty) が見る
    bool HasUnsavedChanges() const { return model_.Dirty(); }

private:
    enum class DragMode : uint8_t { None, Pan, Node, Connect };
    enum class StatusKind : uint8_t { None, Saved, BlockedChildren, BlockedStructure, SaveFailed, LoadFailed };

    void RequestOpen(uint64_t guid); // 未保存の変更があれば確認を挟んでから OpenGuid
    void OpenGuid(uint64_t guid);
    void ReloadFromRegistry();
    void DoSave();
    void DrawToolbar();
    void DrawLeftPanel();
    void DrawPalette(float height);
    void DrawProperties();
    void DrawTreeSettings();
    void DrawNodeProperties(int32_t id);
    bool DrawParam(const BtParamDesc& desc, BtParamValue& value);
    // BB のキーの選択欄。accepts を満たす型のキーだけ並べる。選んだ名前を chosen へ (空 = 未指定)。選んだら true
    bool DrawKeyCombo(const char* label, const std::string& current, bool allowNone, const std::function<bool(BbType)>& accepts,
                      std::string& chosen);
    void DrawCanvas();
    void DrawUnsavedModal();
    void FitView(const ImVec2& canvasSize);
    ImVec2 CanvasCenterGraph() const;
    void AddNodeAt(BtNodeKind kind, float gx, float gy);
    void DeleteSelected(BtRemoveMode mode);

    BehaviorTreeEditModel model_;
    int32_t selected_ = -1;       // 選んでいるノードの id。-1 = なし
    ImVec2 pan_ = ImVec2(40.0f, 40.0f); // グラフの原点のキャンバス内位置 (px)
    float zoom_ = 1.0f;
    bool needFit_ = false;        // 次に描くとき全体が入るよう表示を合わせる
    ImVec2 canvasMin_ = ImVec2(0.0f, 0.0f);
    ImVec2 canvasSize_ = ImVec2(0.0f, 0.0f);

    DragMode drag_ = DragMode::None;
    int32_t dragNode_ = -1;
    int32_t contextNode_ = -1;
    ImVec2 contextGraphPos_ = ImVec2(0.0f, 0.0f);

    StatusKind status_ = StatusKind::None;
    int32_t statusNode_ = -1;
    std::string statusText_;      // Saved / LoadFailed の名前

    uint64_t pendingGuid_ = 0;    // 未保存の確認の後に開く木
    bool openModal_ = false;
};

} // namespace mye
