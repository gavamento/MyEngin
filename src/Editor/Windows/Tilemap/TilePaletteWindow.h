//====================================================================================
//                          TilePaletteWindow.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット窓 (Scene View の 2D モードでタイルを塗る)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Editor/Windows/Tilemap/TilemapEditModel.h"
#include "Engine/Engine/Loop/EngineLoop.h"

namespace mye {

struct Selection;

// Scene View が毎フレーム窓へ渡すマウスの状態。セルは Tilemap 原点基準 (ワールド z は Tilemap の z の平面との交点)
struct TileSceneInput {
    bool hasCell = false; // カーソルがセルに変換できた
    int32_t cellX = 0;
    int32_t cellY = 0;
    bool hovered = false;  // Scene View の上にある (ギズモ・ツールバーの上は除く)
    bool pressed = false;  // 左ボタンを押した瞬間 (hovered のときだけ立つ)
    bool down = false;     // 左ボタンを押している
    bool released = false; // 左ボタンを離した瞬間
};


// Scene View が重ねるセルの枠
struct TileSceneOverlay {
    bool hasCursor = false; // 黄: カーソルのセル
    int32_t cursorX = 0;
    int32_t cursorY = 0;
    bool hasDragRect = false; // 黄: 矩形 / 選択ツールのドラッグ範囲
    tileedit::CellRect dragRect;
    bool hasSelection = false; // 水色: 確定した選択範囲
    tileedit::CellRect selection;
};

// タイルパレット窓 (M94l)。シーン内の Tilemap エンティティを 1 つ対象にして、その .tilemap.json を編集する:
//   - ツール (選択 / ブラシ / 消去 / 塗り潰し / 矩形 / ピック)、flip / 回転、パレットのタイル一覧、レイヤー選択
//   - 塗りは Scene View (2D モード) のドラッグ。編集中は TilemapLibrary へ即時に反映し (ファイルは書かない)、Ctrl+S で書く
//   - 独自の Undo (128 段)。Play 中は塗れない (Play 中の書き換えはスクリプトの TilemapSetCell だけ)
class TilePaletteWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx, const Selection& selection);

    // Inspector の [Open Tile Palette]: 窓を開いて、そのエンティティを対象にする
    void OpenForEntity(EngineContext& ctx, uint64_t fileId);

    bool HasUnsavedChanges() const { return model_.Dirty(); }

    // ---- Scene View との連携 ----
    // EditorApp が毎フレーム Scene View より先に渡す
    void SetInPlayMode(bool inPlay) { inPlay_ = inPlay; }
    void SetSceneMode2d(bool mode2d) { sceneMode2d_ = mode2d; }
    // Scene View のクリックを塗りとして受けるか (窓が開いていて、対象があり、Play 中でない)
    bool SceneEditActive() const;
    uint64_t TargetFileId() const { return targetFid_; }
    const TilemapAsset* TargetAsset() const { return model_.IsLoaded() ? &model_.Asset() : nullptr; }
    void OnSceneInput(const TileSceneInput& in);
    // Delete キーを窓が使う (選択範囲のタイルを消す) か。EditorApp のグローバルな Delete (エンティティの削除) より先に問い合わせる。
    // 窓がフォーカスされているか Scene View 上にカーソルがあり、Select ツールで選択範囲があるあいだ true
    bool ClaimsDeleteKey() const;
    TileSceneOverlay SceneOverlay() const;

private:
    enum class StatusKind { None, Saved, SaveFailed, TooLarge, NoTile };
    struct TargetEntry {
        uint64_t fileId = 0;
        uint64_t assetHash = 0;
        std::string label;
    };

    void CollectTargets(EngineContext& ctx);
    void RequestTarget(EngineContext& ctx, uint64_t fileId);
    bool LoadTarget(EngineContext& ctx, uint64_t fileId);
    void DiscardEdits();
    void SyncLive();
    void DoSave(EngineContext& ctx);
    uint32_t BrushValue() const;
    void SetTool(tileedit::Tool tool);

    void DrawUnsavedModal(EngineContext& ctx);
    void HandleKeys(EngineContext& ctx);
    void HandleToolKeys();
    void DrawTargetRow(EngineContext& ctx);
    void DrawToolRow();
    void DrawBrushRow();
    void DrawPaletteList(EngineContext& ctx);
    void DrawStatus();

    // Scene View の入力 1 ツール分
    void BeginStroke(const TileSceneInput& in);
    void ContinueStroke(const TileSceneInput& in);
    void EndStroke(const TileSceneInput& in);
    void ApplyAt(int32_t x, int32_t y);
    void Report(tileedit::EditResult result);

    TilemapEditModel model_;
    std::vector<TargetEntry> targets_;
    uint64_t targetFid_ = 0;
    uint64_t syncedRevision_ = 0;
    uint64_t syncedEpoch_ = 0; // 窓が最後に見た TilemapLibrary の版
    bool inPlay_ = false;
    bool sceneMode2d_ = false;

    // ツールとブラシ
    tileedit::Tool tool_ = tileedit::Tool::Brush;
    int layer_ = 0;
    uint32_t brushTile_ = 0; // 0 = 未選択
    bool flipX_ = false;
    bool flipY_ = false;
    uint32_t rotation_ = 0; // 0..3 (90 度の個数)
    bool sceneEdit_ = true;

    // ドラッグ
    bool stroking_ = false;
    int32_t anchorX_ = 0;
    int32_t anchorY_ = 0;
    int32_t lastX_ = 0;
    int32_t lastY_ = 0;
    bool hasCursor_ = false;
    int32_t cursorX_ = 0;
    int32_t cursorY_ = 0;

    StatusKind status_ = StatusKind::None;
    int deleteClaimFrame_ = -10; // Delete を使える状態だった最後の ImGui フレーム
    uint64_t pendingFid_ = 0;
    bool modalRequest_ = false;
};

} // namespace mye
