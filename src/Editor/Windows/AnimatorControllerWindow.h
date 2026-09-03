#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Engine/Engine/AnimatorController.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

struct Selection;

// Animator Controller ウィンドウ (M22)。選択エンティティの AnimatorControllerComponent が指す
// .controller.json を **ノードグラフ**で編集/可視化する:
//   - ステートをドラッグ可能なノード、遷移を矢印で描画 (ImDrawList 自作、node-graph ライブラリ無し)
//   - Play 中は現在ステートをハイライト + 遷移の進行を表示
//   - パラメータをライブ編集して遷移をテスト
//   - ステート/遷移/条件の編集、Add State / Add Transition / Save
class AnimatorControllerWindow {
public:
    bool open = true;
    void OnImGui(EngineContext& ctx, Selection& selection);

    // 触ったコントローラのうち 1 本でもディスクと食い違うか (M66d、spec §4.1 の S6)。
    // 判定の理屈は AnimationWindow::HasUnsavedChanges と同じ (この窓も dirty を持たない)
    bool HasUnsavedChanges() const;

private:
    void MarkTouched(ControllerLibrary* controllers, uint64_t ctrlHash);

    ControllerLibrary* controllers_ = nullptr; // 寿命は EngineContext と同じ
    std::vector<uint64_t> touched_;

    struct NodePos {
        float x = 0.0f;
        float y = 0.0f;
    };
    // コントローラごとのノード配置 (エディタ都合。アセットには保存しない)
    std::vector<NodePos>& NodePositions(uint64_t ctrlHash, size_t stateCount);
    std::unordered_map<uint64_t, std::vector<NodePos>> layout_;
    int selectedState_ = -1;
    int selectedTransition_ = -1;
};

} // namespace mye
