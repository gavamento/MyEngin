//====================================================================================
//                          TilemapEditModel.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット窓の編集モデル (塗りの純関数と Undo)
//====================================================================================
#pragma once
#include <cstdint>
#include <deque>
#include <vector>

#include "Engine/Engine/Tilemap/TilemapAssets.h"

namespace mye {
namespace tileedit {

enum class Tool : int { Select = 0, Brush, Erase, Fill, Rect, Pick };
inline constexpr int kToolCount = 6;

// セル矩形 (両端を含む)。x0 <= x1 かつ y0 <= y1 に正規化して使う
struct CellRect {
    int32_t x0 = 0;
    int32_t y0 = 0;
    int32_t x1 = 0;
    int32_t y1 = 0;
    bool operator==(const CellRect&) const = default;
};

// 2 隅から正規化した矩形
CellRect MakeCellRect(int32_t ax, int32_t ay, int32_t bx, int32_t by);

enum class EditResult {
    Changed,  // 1 セル以上書き換えた
    NoChange, // 書く値が今と同じ
    TooLarge, // 対象のセル数が上限を超えた (何も書かない)
    Invalid,  // レイヤー / 座標が範囲外、または未ロード
};

} // namespace tileedit

// 編集中の .tilemap.json (全レイヤーのセル)。アセットのコピーを持ち、塗りはセル差分で Undo に積む。
// 保存 (ファイルを書く / TilemapLibrary を登録し直す) は呼び手の仕事で、保存時点は MarkSaved で知らせる。
// Undo は BehaviorTreeEditModel / SpriteEditModel と同じ流儀の独自 deque (シーンの UndoStack には載せない)
class TilemapEditModel {
public:
    static constexpr size_t kMaxUndo = 128;
    // 塗り潰しが 1 回で書ける最大のセル数。空白は無限に広がるので、超えたら何も書かない
    static constexpr size_t kMaxFloodCells = 65536;
    // 矩形が 1 回で書ける最大のセル数
    static constexpr int64_t kMaxRectCells = 1 << 18;

    bool Load(const TilemapAsset& asset);
    void Clear();
    bool IsLoaded() const { return loaded_; }
    const TilemapAsset& Asset() const { return asset_; }
    // 内容が変わるたびに増える (編集 / Undo / Redo)。窓が TilemapLibrary への反映の要否を見る
    uint64_t Revision() const { return revision_; }

    // 保存した (読み込んだ) 内容と違うか
    bool Dirty() const;
    // 今の内容を保存済みとして記録する
    void MarkSaved();
    // 全レイヤーのセルが other と同じか (外部の再読み込みと編集中の内容の比較用)
    bool SameCells(const TilemapAsset& other) const;

    uint32_t GetCell(int32_t layer, int32_t x, int32_t y) const;

    // ---- 編集 (純関数: 引数と現在のセルだけで結果が決まる) ----
    tileedit::EditResult PaintCell(int32_t layer, int32_t x, int32_t y, uint32_t value);
    tileedit::EditResult EraseCell(int32_t layer, int32_t x, int32_t y);
    // 4 連結で (x, y) と同じ値のセルを value に置き換える。書いた数は outCount へ
    tileedit::EditResult FloodFill(int32_t layer, int32_t x, int32_t y, uint32_t value, size_t* outCount = nullptr);
    // 矩形 (2 隅は順不同) を value で埋める
    tileedit::EditResult FillRect(int32_t layer, int32_t ax, int32_t ay, int32_t bx, int32_t by, uint32_t value,
                                  size_t* outCount = nullptr);
    // ピック: セルの値。空は 0
    uint32_t PickCell(int32_t layer, int32_t x, int32_t y) const { return GetCell(layer, x, y); }

    // ---- 選択 (Select ツール) ----
    void SelectRect(int32_t ax, int32_t ay, int32_t bx, int32_t by);
    void ClearSelection() { hasSelection_ = false; }
    bool HasSelection() const { return hasSelection_; }
    const tileedit::CellRect& Selection() const { return selection_; }
    // 選択範囲のセルを空にする
    tileedit::EditResult EraseSelection(int32_t layer);

    // ---- Undo / Redo ----
    bool CanUndo() const { return !gesture_ && !undo_.empty(); }
    bool CanRedo() const { return !gesture_ && !redo_.empty(); }
    bool Undo();
    bool Redo();
    // ドラッグなどの連続した塗りを 1 段にまとめる。開いている間の最初の変更で 1 段だけ積む
    void BeginGesture();
    void EndGesture();
    bool InGesture() const { return gesture_; }
    size_t UndoDepth() const { return undo_.size(); }

private:
    struct CellEdit {
        int32_t layer = 0;
        int32_t x = 0;
        int32_t y = 0;
        uint32_t before = 0;
        uint32_t after = 0;
    };
    using Step = std::vector<CellEdit>;

    bool ValidCell(int32_t layer, int32_t x, int32_t y) const;
    // edits を反映して Undo に積む。空なら NoChange
    tileedit::EditResult Commit(Step&& edits);
    void ApplyStep(const Step& step, bool forward);

    bool loaded_ = false;
    TilemapAsset asset_;
    std::vector<TilemapLayer> saved_;
    std::deque<Step> undo_;
    std::deque<Step> redo_;
    bool gesture_ = false;
    bool gesturePushed_ = false;
    uint64_t revision_ = 0;
    mutable uint64_t dirtyRevision_ = ~0ull; // Dirty のキャッシュ
    mutable bool dirtyCache_ = false;
    bool hasSelection_ = false;
    tileedit::CellRect selection_;
};

} // namespace mye
