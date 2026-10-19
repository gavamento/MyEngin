//====================================================================================
//                          SpriteEditorWindow.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトエディタ窓 (テクスチャのスライス編集)
//====================================================================================
#pragma once
#include <string>

#include "Editor/Windows/Sprite/SpriteEditModel.h"
#include "Engine/Engine/Loop/EngineLoop.h"

struct ImVec2;

namespace mye {

// スプライトエディタ窓 (M94e)。テクスチャ 1 枚を開き、.meta の tex.sprites (スライス) を編集して保存する:
//   - グリッド (セルサイズ / セル数) と自動 (アルファの連結成分) のスライス、Delete Existing / Safe
//   - 矩形のドラッグ追加・移動・8 ハンドルのリサイズ・Delete、名前 / 矩形 / pivot / border の数値編集
//   - 独自の Undo (128 段)。保存は .meta を書いて SpriteLibrary とテクスチャを登録し直す
// 表示のフィルターは常にリニア (ImGui のサンプラは差し替えない)。拡大すると滲む
class SpriteEditorWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx);

    // テクスチャを開く (窓も開く)。未保存の編集があれば確認モーダルを出してから切り替える
    void OpenTexture(const std::wstring& path);

    bool HasUnsavedChanges() const { return model_.Dirty(); }

private:
    enum class Drag { None, Create, Move, Resize };
    enum class StatusKind { None, Sliced, Nothing, Saved, SaveFailed, LoadFailed, NoPixels };

    bool LoadTexture(const std::wstring& path);
    void DrawToolbar(EngineContext& ctx);
    void DrawSlicePanel();
    void DrawCanvas(EngineContext& ctx);
    void DrawDetail();
    void DrawUnsavedModal(EngineContext& ctx);
    void HandleKeys(EngineContext& ctx);
    void DoSlice();
    void DoSave(EngineContext& ctx);
    void FitView(const ImVec2& canvasSize);
    // Slice パネルの pivot 設定 (新規スプライトの初期 pivot)
    void SlicePivot(float& pivotX, float& pivotY) const;

    SpriteEditModel model_;
    std::string stem_; // 新規スプライトの名前の接頭辞 (テクスチャのファイル名)
    int selected_ = -1;

    // 表示
    float zoom_ = 4.0f;
    float panX_ = 0.0f; // 画像左上の、キャンバス左上からの画面オフセット
    float panY_ = 0.0f;
    bool fitPending_ = false;

    // ドラッグ
    Drag drag_ = Drag::None;
    int dragHandle_ = 0;
    spriteedit::IRect dragStartRect_;
    int dragStartX_ = 0;
    int dragStartY_ = 0;
    spriteedit::IRect createRect_;

    // Slice パネルの設定
    int sliceType_ = 0; // 0 セルサイズ / 1 セル数 / 2 自動
    int cellW_ = 16;
    int cellH_ = 16;
    int cols_ = 4;
    int rows_ = 4;
    int offsetX_ = 0;
    int offsetY_ = 0;
    int padX_ = 0;
    int padY_ = 0;
    int minSize_ = 4;
    bool keepEmpty_ = true;
    int method_ = 0; // SliceMethod
    int slicePivotPreset_ = 0;
    float sliceCustomPivot_[2] = { 0.5f, 0.5f };

    // 状態表示と未保存の確認
    StatusKind status_ = StatusKind::None;
    int statusCount_ = 0;
    std::wstring pendingPath_;
    bool modalRequest_ = false;
};

} // namespace mye
