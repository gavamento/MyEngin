//====================================================================================
//                          TilemapEditModel.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット窓の編集モデルの実装
//====================================================================================
#include "Editor/Windows/Tilemap/TilemapEditModel.h"

#include <algorithm>
#include <unordered_set>

#include "Engine/Engine/Tilemap/TilemapSystem.h"

namespace mye {
namespace {

constexpr int32_t kCoordMin = TilemapSystem::kCellCoordMin;
constexpr int32_t kCoordMax = TilemapSystem::kCellCoordMax;

bool InCoordRange(int32_t v)
{
    return v >= kCoordMin && v <= kCoordMax;
}

bool ChunksEqual(const std::map<ChunkKey, TileChunk>& a, const std::map<ChunkKey, TileChunk>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (ia->first != ib->first || ia->second.cells != ib->second.cells) {
            return false;
        }
    }
    return true;
}

bool LayersEqual(const std::vector<TilemapLayer>& a, const std::vector<TilemapLayer>& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (!ChunksEqual(a[i].chunks, b[i].chunks)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace tileedit {

CellRect MakeCellRect(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    CellRect r;
    r.x0 = (std::min)(ax, bx);
    r.y0 = (std::min)(ay, by);
    r.x1 = (std::max)(ax, bx);
    r.y1 = (std::max)(ay, by);
    return r;
}

} // namespace tileedit

bool TilemapEditModel::Load(const TilemapAsset& asset)
{
    Clear();
    asset_ = asset;
    saved_ = asset_.layers;
    loaded_ = true;
    ++revision_;
    return true;
}

void TilemapEditModel::Clear()
{
    loaded_ = false;
    asset_ = TilemapAsset{};
    saved_.clear();
    undo_.clear();
    redo_.clear();
    gesture_ = false;
    gesturePushed_ = false;
    hasSelection_ = false;
    ++revision_;
}

bool TilemapEditModel::Dirty() const
{
    if (!loaded_) {
        return false;
    }
    if (dirtyRevision_ != revision_) {
        dirtyRevision_ = revision_;
        dirtyCache_ = !LayersEqual(saved_, asset_.layers);
    }
    return dirtyCache_;
}

bool TilemapEditModel::SameCells(const TilemapAsset& other) const
{
    return loaded_ && LayersEqual(asset_.layers, other.layers);
}

void TilemapEditModel::MarkSaved()
{
    saved_ = asset_.layers;
    ++revision_;
}

bool TilemapEditModel::ValidCell(int32_t layer, int32_t x, int32_t y) const
{
    return loaded_ && layer >= 0 && static_cast<size_t>(layer) < asset_.layers.size() && InCoordRange(x)
           && InCoordRange(y);
}

uint32_t TilemapEditModel::GetCell(int32_t layer, int32_t x, int32_t y) const
{
    if (!ValidCell(layer, x, y)) {
        return 0;
    }
    return asset_.layers[static_cast<size_t>(layer)].GetCell(x, y);
}

void TilemapEditModel::ApplyStep(const Step& step, bool forward)
{
    // 逆方向は後ろから戻す (同じセルを 2 回書いた段でも元の値に戻る)
    if (forward) {
        for (const CellEdit& e : step) {
            asset_.layers[static_cast<size_t>(e.layer)].SetCell(e.x, e.y, e.after);
        }
    } else {
        for (auto it = step.rbegin(); it != step.rend(); ++it) {
            asset_.layers[static_cast<size_t>(it->layer)].SetCell(it->x, it->y, it->before);
        }
    }
    ++revision_;
}

tileedit::EditResult TilemapEditModel::Commit(Step&& edits)
{
    if (edits.empty()) {
        return tileedit::EditResult::NoChange;
    }
    ApplyStep(edits, true);
    redo_.clear();
    if (gesture_ && gesturePushed_ && !undo_.empty()) {
        Step& open = undo_.back();
        open.insert(open.end(), edits.begin(), edits.end());
    } else {
        undo_.push_back(std::move(edits));
        gesturePushed_ = gesture_;
        while (undo_.size() > kMaxUndo) {
            undo_.pop_front();
        }
    }
    return tileedit::EditResult::Changed;
}

tileedit::EditResult TilemapEditModel::PaintCell(int32_t layer, int32_t x, int32_t y, uint32_t value)
{
    if (!ValidCell(layer, x, y)) {
        return tileedit::EditResult::Invalid;
    }
    const uint32_t before = GetCell(layer, x, y);
    if (before == value) {
        return tileedit::EditResult::NoChange;
    }
    Step edits;
    edits.push_back(CellEdit{ layer, x, y, before, value });
    return Commit(std::move(edits));
}

tileedit::EditResult TilemapEditModel::EraseCell(int32_t layer, int32_t x, int32_t y)
{
    return PaintCell(layer, x, y, 0);
}

tileedit::EditResult TilemapEditModel::FloodFill(int32_t layer, int32_t x, int32_t y, uint32_t value,
                                                 size_t* outCount)
{
    if (outCount != nullptr) {
        *outCount = 0;
    }
    if (!ValidCell(layer, x, y)) {
        return tileedit::EditResult::Invalid;
    }
    const uint32_t target = GetCell(layer, x, y);
    if (target == value) {
        return tileedit::EditResult::NoChange;
    }
    // 幅優先。訪問済みは集合で見るだけで、走査順は queue の積み順で決まる
    std::vector<uint32_t> queue; // cellKey
    std::unordered_set<uint32_t> visited;
    queue.push_back(TilemapSystem::MakeCellKey(x, y));
    visited.insert(queue.back());
    for (size_t head = 0; head < queue.size(); ++head) {
        int32_t cx = 0;
        int32_t cy = 0;
        TilemapSystem::SplitCellKey(queue[head], cx, cy);
        static constexpr int32_t kDx[4] = { 1, -1, 0, 0 };
        static constexpr int32_t kDy[4] = { 0, 0, 1, -1 };
        for (int d = 0; d < 4; ++d) {
            const int32_t nx = cx + kDx[d];
            const int32_t ny = cy + kDy[d];
            if (!InCoordRange(nx) || !InCoordRange(ny) || GetCell(layer, nx, ny) != target) {
                continue;
            }
            const uint32_t key = TilemapSystem::MakeCellKey(nx, ny);
            if (!visited.insert(key).second) {
                continue;
            }
            queue.push_back(key);
            if (queue.size() > kMaxFloodCells) {
                return tileedit::EditResult::TooLarge;
            }
        }
    }
    Step edits;
    edits.reserve(queue.size());
    for (const uint32_t key : queue) {
        int32_t cx = 0;
        int32_t cy = 0;
        TilemapSystem::SplitCellKey(key, cx, cy);
        edits.push_back(CellEdit{ layer, cx, cy, target, value });
    }
    if (outCount != nullptr) {
        *outCount = edits.size();
    }
    return Commit(std::move(edits));
}

tileedit::EditResult TilemapEditModel::FillRect(int32_t layer, int32_t ax, int32_t ay, int32_t bx, int32_t by,
                                                uint32_t value, size_t* outCount)
{
    if (outCount != nullptr) {
        *outCount = 0;
    }
    if (!ValidCell(layer, ax, ay) || !ValidCell(layer, bx, by)) {
        return tileedit::EditResult::Invalid;
    }
    const tileedit::CellRect r = tileedit::MakeCellRect(ax, ay, bx, by);
    const int64_t cells = (static_cast<int64_t>(r.x1) - r.x0 + 1) * (static_cast<int64_t>(r.y1) - r.y0 + 1);
    if (cells > kMaxRectCells) {
        return tileedit::EditResult::TooLarge;
    }
    Step edits;
    for (int32_t cy = r.y0; cy <= r.y1; ++cy) {
        for (int32_t cx = r.x0; cx <= r.x1; ++cx) {
            const uint32_t before = GetCell(layer, cx, cy);
            if (before != value) {
                edits.push_back(CellEdit{ layer, cx, cy, before, value });
            }
        }
    }
    if (outCount != nullptr) {
        *outCount = edits.size();
    }
    return Commit(std::move(edits));
}

void TilemapEditModel::SelectRect(int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    selection_ = tileedit::MakeCellRect(std::clamp(ax, kCoordMin, kCoordMax), std::clamp(ay, kCoordMin, kCoordMax),
                                        std::clamp(bx, kCoordMin, kCoordMax), std::clamp(by, kCoordMin, kCoordMax));
    hasSelection_ = true;
}

tileedit::EditResult TilemapEditModel::EraseSelection(int32_t layer)
{
    if (!hasSelection_) {
        return tileedit::EditResult::NoChange;
    }
    return FillRect(layer, selection_.x0, selection_.y0, selection_.x1, selection_.y1, 0);
}

bool TilemapEditModel::Undo()
{
    if (!CanUndo()) {
        return false;
    }
    Step step = std::move(undo_.back());
    undo_.pop_back();
    ApplyStep(step, false);
    redo_.push_back(std::move(step));
    return true;
}

bool TilemapEditModel::Redo()
{
    if (!CanRedo()) {
        return false;
    }
    Step step = std::move(redo_.back());
    redo_.pop_back();
    ApplyStep(step, true);
    undo_.push_back(std::move(step));
    return true;
}

void TilemapEditModel::BeginGesture()
{
    gesture_ = true;
    gesturePushed_ = false;
}

void TilemapEditModel::EndGesture()
{
    gesture_ = false;
    gesturePushed_ = false;
}

} // namespace mye
