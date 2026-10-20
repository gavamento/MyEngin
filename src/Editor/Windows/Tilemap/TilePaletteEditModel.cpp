//====================================================================================
//                          TilePaletteEditModel.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレットの編集モデルの実装
//====================================================================================
#include "Editor/Windows/Tilemap/TilePaletteEditModel.h"

#include <algorithm>

namespace mye {

bool TilePaletteEditModel::Load(const TilePalette& palette)
{
    Clear();
    palette_ = palette;
    saved_ = palette_.tiles;
    loaded_ = true;
    ++revision_;
    return true;
}

void TilePaletteEditModel::Clear()
{
    loaded_ = false;
    palette_ = TilePalette{};
    saved_.clear();
    undo_.clear();
    redo_.clear();
    gesture_ = false;
    gesturePushed_ = false;
    ++revision_;
}

bool TilePaletteEditModel::Dirty() const
{
    return loaded_ && palette_.tiles != saved_;
}

void TilePaletteEditModel::MarkSaved()
{
    saved_ = palette_.tiles;
    ++revision_;
}

bool TilePaletteEditModel::SameTiles(const TilePalette& other) const
{
    return loaded_ && palette_.tiles == other.tiles;
}

void TilePaletteEditModel::Commit(std::vector<TileDef>&& before)
{
    ++revision_;
    redo_.clear();
    if (gesture_ && gesturePushed_ && !undo_.empty()) {
        undo_.back().after = palette_.tiles; // 連続した変更は最初の before を残して 1 段にまとめる
        return;
    }
    Step step;
    step.before = std::move(before);
    step.after = palette_.tiles;
    undo_.push_back(std::move(step));
    gesturePushed_ = gesture_;
    while (undo_.size() > kMaxUndo) {
        undo_.pop_front();
    }
}

bool TilePaletteEditModel::AddTile(const std::string& name, AssetID sprite, uint32_t* outId)
{
    if (!loaded_) {
        return false;
    }
    std::vector<TileDef> before = palette_.tiles;
    const TileDef* added = palette_.Add(name, sprite);
    if (added == nullptr) {
        return false;
    }
    if (outId != nullptr) {
        *outId = added->id;
    }
    Commit(std::move(before));
    return true;
}

size_t TilePaletteEditModel::AddTiles(const std::vector<std::pair<std::string, AssetID>>& entries)
{
    if (!loaded_) {
        return 0;
    }
    std::vector<TileDef> before = palette_.tiles;
    size_t added = 0;
    for (const auto& [name, sprite] : entries) {
        if (palette_.Add(name, sprite) == nullptr) {
            break;
        }
        ++added;
    }
    if (added > 0) {
        Commit(std::move(before));
    }
    return added;
}

bool TilePaletteEditModel::RemoveTile(uint32_t id)
{
    if (!loaded_ || palette_.Find(id) == nullptr) {
        return false;
    }
    std::vector<TileDef> before = palette_.tiles;
    palette_.tiles.erase(std::remove_if(palette_.tiles.begin(), palette_.tiles.end(),
                                        [id](const TileDef& t) { return t.id == id; }),
                         palette_.tiles.end());
    for (TileDef& t : palette_.tiles) {
        if (t.destroyedTile == id) {
            t.destroyedTile = 0;
        }
    }
    Commit(std::move(before));
    return true;
}

bool TilePaletteEditModel::SetTile(const TileDef& def)
{
    if (!loaded_) {
        return false;
    }
    const auto it = std::find_if(palette_.tiles.begin(), palette_.tiles.end(),
                                 [&def](const TileDef& t) { return t.id == def.id; });
    if (it == palette_.tiles.end()) {
        return false;
    }
    std::vector<TileDef> before = palette_.tiles;
    *it = def;
    TilePaletteLibrary::Sanitize(palette_);
    if (palette_.tiles == before) {
        return false;
    }
    Commit(std::move(before));
    return true;
}

bool TilePaletteEditModel::Undo()
{
    if (!CanUndo()) {
        return false;
    }
    Step step = std::move(undo_.back());
    undo_.pop_back();
    palette_.tiles = step.before;
    ++revision_;
    redo_.push_back(std::move(step));
    return true;
}

bool TilePaletteEditModel::Redo()
{
    if (!CanRedo()) {
        return false;
    }
    Step step = std::move(redo_.back());
    redo_.pop_back();
    palette_.tiles = step.after;
    ++revision_;
    undo_.push_back(std::move(step));
    return true;
}

void TilePaletteEditModel::BeginGesture()
{
    gesture_ = true;
    gesturePushed_ = false;
}

void TilePaletteEditModel::EndGesture()
{
    gesture_ = false;
    gesturePushed_ = false;
}

} // namespace mye
