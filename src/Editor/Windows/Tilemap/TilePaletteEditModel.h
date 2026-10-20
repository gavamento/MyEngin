//====================================================================================
//                          TilePaletteEditModel.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット (.tilepalette.json) の編集モデルと Undo
//====================================================================================
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "Engine/Engine/Tilemap/TilemapAssets.h"

namespace mye {

// 編集中のタイルパレット。アセットのコピーを持ち、編集は tiles 配列ごとのスナップショットで Undo に積む
// (パレットは数十種の小さな表なので差分にしない)。保存 (ファイルを書く / TilePaletteLibrary を登録し直す) は
// 呼び手の仕事で、保存時点は MarkSaved で知らせる。Undo は TilemapEditModel と同じ流儀の独自 deque
class TilePaletteEditModel {
public:
    static constexpr size_t kMaxUndo = 128;

    bool Load(const TilePalette& palette);
    void Clear();
    bool IsLoaded() const { return loaded_; }
    const TilePalette& Palette() const { return palette_; }
    // 内容が変わるたびに増える (編集 / Undo / Redo)。窓が TilePaletteLibrary への反映の要否を見る
    uint64_t Revision() const { return revision_; }

    bool Dirty() const;
    void MarkSaved();
    // タイルの定義が other と同じか (外部の再読み込みと編集中の内容の比較用)
    bool SameTiles(const TilePalette& other) const;

    // ---- 編集 (どれも Undo の 1 段。連続した変更は BeginGesture / EndGesture で 1 段にまとまる) ----

    // 末尾 (id = 最大 + 1) に 1 種足す。id が尽きていたら false。outId に割り当てた id
    bool AddTile(const std::string& name, AssetID sprite, uint32_t* outId = nullptr);
    // 複数をまとめて 1 段で足す。足せた数を返す (id が尽きたらそこまで)
    size_t AddTiles(const std::vector<std::pair<std::string, AssetID>>& entries);
    // 消す。他のタイルの destroyedTile が指していたら 0 (空) にする。無い id は false。
    // マップのセルに残る id は消さない (パレットに無い id のセルは描かれないだけ)
    bool RemoveTile(uint32_t id);
    // 同じ id のタイルを def で置き換える。値は TilePaletteLibrary::Sanitize の範囲に丸める。無い id / 変化なしは false
    bool SetTile(const TileDef& def);

    // ---- Undo / Redo ----
    bool CanUndo() const { return !gesture_ && !undo_.empty(); }
    bool CanRedo() const { return !gesture_ && !redo_.empty(); }
    bool Undo();
    bool Redo();
    void BeginGesture();
    void EndGesture();
    bool InGesture() const { return gesture_; }
    size_t UndoDepth() const { return undo_.size(); }

private:
    struct Step {
        std::vector<TileDef> before;
        std::vector<TileDef> after;
    };

    // palette_.tiles を書き換えた後に呼ぶ。before は書き換え前の配列
    void Commit(std::vector<TileDef>&& before);

    bool loaded_ = false;
    TilePalette palette_;
    std::vector<TileDef> saved_;
    std::deque<Step> undo_;
    std::deque<Step> redo_;
    bool gesture_ = false;
    bool gesturePushed_ = false;
    uint64_t revision_ = 0;
};

} // namespace mye
