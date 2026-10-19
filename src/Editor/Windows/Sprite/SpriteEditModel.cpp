//====================================================================================
//                          SpriteEditModel.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトエディタの編集モデルの実装
//====================================================================================
#include "Editor/Windows/Sprite/SpriteEditModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"

namespace mye {
namespace spriteedit {
namespace {

// 9 プリセットの pivot (左下原点)。並びは PivotPreset の説明と同じ
constexpr float kPresetPivots[kPivotPresetCount][2] = {
    { 0.5f, 0.5f }, { 0.0f, 1.0f }, { 0.5f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.5f },
    { 1.0f, 0.5f }, { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 1.0f, 0.0f },
};

bool CellHasAlpha(const uint8_t* rgba, int texW, const IRect& r)
{
    for (int y = r.y; y < r.y + r.h; ++y) {
        const uint8_t* row = rgba + (static_cast<size_t>(y) * texW + r.x) * 4;
        for (int x = 0; x < r.w; ++x) {
            if (row[x * 4 + 3] != 0) {
                return true;
            }
        }
    }
    return false;
}

void AppendCell(std::vector<IRect>& out, const IRect& cell, int texW, bool keepEmpty, const uint8_t* rgba)
{
    if (!keepEmpty && rgba != nullptr && !CellHasAlpha(rgba, texW, cell)) {
        return;
    }
    out.push_back(cell);
}

// "<prefix>_<n>" で既存と重ならない名前を作る。n は startIndex から探す
std::string UniqueName(const std::string& prefix, int startIndex, const std::vector<importmeta::SpriteSlice>& list)
{
    for (int n = startIndex;; ++n) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s_%d", prefix.c_str(), n);
        bool used = false;
        for (const importmeta::SpriteSlice& s : list) {
            if (std::strcmp(s.name, buf) == 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            return buf;
        }
    }
}

} // namespace

void PivotPreset(int preset, float& pivotX, float& pivotY)
{
    const int k = (preset >= 0 && preset < kPivotPresetCount) ? preset : 0;
    pivotX = kPresetPivots[k][0];
    pivotY = kPresetPivots[k][1];
}

int PivotPresetOf(float pivotX, float pivotY)
{
    for (int k = 0; k < kPivotPresetCount; ++k) {
        if (pivotX == kPresetPivots[k][0] && pivotY == kPresetPivots[k][1]) {
            return k;
        }
    }
    return kPivotCustom;
}

std::vector<IRect> SliceGridByCellSize(int texW, int texH, int cellW, int cellH, int offX, int offY, int padX,
                                       int padY, bool keepEmpty, const uint8_t* rgba)
{
    std::vector<IRect> out;
    if (cellW <= 0 || cellH <= 0 || texW <= 0 || texH <= 0) {
        return out;
    }
    offX = std::max(offX, 0);
    offY = std::max(offY, 0);
    padX = std::max(padX, 0);
    padY = std::max(padY, 0);
    for (int y = offY; y + cellH <= texH; y += cellH + padY) {
        for (int x = offX; x + cellW <= texW; x += cellW + padX) {
            AppendCell(out, { x, y, cellW, cellH }, texW, keepEmpty, rgba);
        }
    }
    return out;
}

std::vector<IRect> SliceGridByCellCount(int texW, int texH, int cols, int rows, int offX, int offY, int padX,
                                        int padY, bool keepEmpty, const uint8_t* rgba)
{
    std::vector<IRect> out;
    if (cols <= 0 || rows <= 0 || texW <= 0 || texH <= 0) {
        return out;
    }
    offX = std::max(offX, 0);
    offY = std::max(offY, 0);
    padX = std::max(padX, 0);
    padY = std::max(padY, 0);
    const int cellW = (texW - offX - padX * (cols - 1)) / cols;
    const int cellH = (texH - offY - padY * (rows - 1)) / rows;
    if (cellW <= 0 || cellH <= 0) {
        return out;
    }
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            AppendCell(out, { offX + c * (cellW + padX), offY + r * (cellH + padY), cellW, cellH }, texW, keepEmpty,
                       rgba);
        }
    }
    return out;
}

std::vector<IRect> SliceAutomatic(const uint8_t* rgba, int texW, int texH, int minSize)
{
    std::vector<IRect> out;
    if (rgba == nullptr || texW <= 0 || texH <= 0) {
        return out;
    }
    minSize = std::max(minSize, 1);
    std::vector<uint8_t> visited(static_cast<size_t>(texW) * texH, 0);
    std::vector<int> stack; // 再帰を避けて明示スタックで塗る
    auto opaque = [&](int x, int y) { return rgba[(static_cast<size_t>(y) * texW + x) * 4 + 3] != 0; };
    for (int y = 0; y < texH; ++y) {
        for (int x = 0; x < texW; ++x) {
            const size_t start = static_cast<size_t>(y) * texW + x;
            if (visited[start] || !opaque(x, y)) {
                continue;
            }
            int minX = x;
            int maxX = x;
            int minY = y;
            int maxY = y;
            visited[start] = 1;
            stack.push_back(static_cast<int>(start));
            while (!stack.empty()) {
                const int cur = stack.back();
                stack.pop_back();
                const int cx = cur % texW;
                const int cy = cur / texW;
                minX = std::min(minX, cx);
                maxX = std::max(maxX, cx);
                minY = std::min(minY, cy);
                maxY = std::max(maxY, cy);
                const int nx[4] = { cx - 1, cx + 1, cx, cx };
                const int ny[4] = { cy, cy, cy - 1, cy + 1 };
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= texW || ny[k] >= texH) {
                        continue;
                    }
                    const size_t idx = static_cast<size_t>(ny[k]) * texW + nx[k];
                    if (!visited[idx] && opaque(nx[k], ny[k])) {
                        visited[idx] = 1;
                        stack.push_back(static_cast<int>(idx));
                    }
                }
            }
            const IRect box{ minX, minY, maxX - minX + 1, maxY - minY + 1 };
            if (box.w >= minSize && box.h >= minSize) {
                out.push_back(box);
            }
        }
    }
    return out;
}

bool RectsOverlap(const IRect& a, const IRect& b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

IRect ClampRectToTexture(IRect r, int texW, int texH)
{
    if (texW <= 0 || texH <= 0) {
        return r;
    }
    r.x = std::clamp(r.x, 0, texW - 1);
    r.y = std::clamp(r.y, 0, texH - 1);
    r.w = std::clamp(r.w, 1, texW - r.x);
    r.h = std::clamp(r.h, 1, texH - r.y);
    return r;
}

std::vector<importmeta::SpriteSlice> ApplySlices(const std::vector<importmeta::SpriteSlice>& existing,
                                                 const std::vector<IRect>& newRects, SliceMethod method,
                                                 float pivotX, float pivotY, const std::string& namePrefix,
                                                 uint32_t firstId)
{
    std::vector<importmeta::SpriteSlice> result;
    if (method == SliceMethod::Safe) {
        result = existing;
    }
    uint32_t nextId = firstId;
    for (const IRect& r : newRects) {
        if (method == SliceMethod::Safe) {
            bool overlaps = false;
            for (const importmeta::SpriteSlice& e : existing) {
                if (RectsOverlap(r, { e.x, e.y, e.w, e.h })) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                continue;
            }
        }
        importmeta::SpriteSlice s;
        s.id = nextId++;
        const std::string name = UniqueName(namePrefix, static_cast<int>(result.size()), result);
        std::snprintf(s.name, sizeof(s.name), "%s", name.c_str());
        s.x = r.x;
        s.y = r.y;
        s.w = r.w;
        s.h = r.h;
        s.pivotX = pivotX;
        s.pivotY = pivotY;
        result.push_back(s);
    }
    return result;
}

} // namespace spriteedit

// ---------------------------------------------------------------------------
// SpriteEditModel
// ---------------------------------------------------------------------------

bool SpriteEditModel::Load(const std::wstring& texPath, int texW, int texH)
{
    Clear();
    path_ = texPath;
    texW_ = texW;
    texH_ = texH;
    AssetMeta meta;
    if (AssetDatabase::ReadMeta(texPath + L".meta", meta)) {
        sprites_ = meta.tex.sprites;
    }
    saved_ = sprites_;
    savedKey_ = Serialize(sprites_);
    loaded_ = true;
    ++revision_;
    return true;
}

void SpriteEditModel::Clear()
{
    loaded_ = false;
    path_.clear();
    texW_ = 0;
    texH_ = 0;
    sprites_.clear();
    saved_.clear();
    savedKey_.clear();
    undo_.clear();
    redo_.clear();
    gesture_ = false;
    gesturePushed_ = false;
    ++revision_;
}

uint32_t SpriteEditModel::NextId() const
{
    uint32_t maxId = 0;
    for (const importmeta::SpriteSlice& s : sprites_) {
        maxId = std::max(maxId, s.id);
    }
    return maxId + 1;
}

std::string SpriteEditModel::Serialize(const SpriteList& sprites)
{
    std::string out;
    char buf[256];
    for (const importmeta::SpriteSlice& s : sprites) {
        // 浮動小数は %a (16 進) で書く: 文字列が違えば値が違う、を保つ
        std::snprintf(buf, sizeof(buf), "%u|%s|%d,%d,%d,%d|%a,%a|%d,%d,%d,%d;", s.id, s.name, s.x, s.y, s.w, s.h,
                      static_cast<double>(s.pivotX), static_cast<double>(s.pivotY), s.borderL, s.borderB, s.borderR,
                      s.borderT);
        out += buf;
    }
    return out;
}

bool SpriteEditModel::Dirty() const
{
    if (!loaded_) {
        return false;
    }
    if (dirtyRevision_ != revision_) {
        dirtyCache_ = Serialize(sprites_) != savedKey_;
        dirtyRevision_ = revision_;
    }
    return dirtyCache_;
}

void SpriteEditModel::BeforeChange()
{
    if (gesture_) {
        if (gesturePushed_) {
            return;
        }
        gesturePushed_ = true;
    }
    undo_.push_back(sprites_);
    if (undo_.size() > kMaxUndo) {
        undo_.pop_front();
    }
    redo_.clear();
}

int SpriteEditModel::AddRect(const spriteedit::IRect& rect, float pivotX, float pivotY, const std::string& name)
{
    if (!loaded_ || rect.w < 1 || rect.h < 1) {
        return -1;
    }
    const spriteedit::IRect r = spriteedit::ClampRectToTexture(rect, texW_, texH_);
    BeforeChange();
    importmeta::SpriteSlice s;
    s.id = NextId();
    const std::string finalName = name.empty() ? "sprite_" + std::to_string(s.id) : name;
    std::snprintf(s.name, sizeof(s.name), "%s", finalName.c_str());
    s.x = r.x;
    s.y = r.y;
    s.w = r.w;
    s.h = r.h;
    s.pivotX = pivotX;
    s.pivotY = pivotY;
    sprites_.push_back(s);
    ++revision_;
    return static_cast<int>(sprites_.size()) - 1;
}

bool SpriteEditModel::RemoveAt(int index)
{
    if (!loaded_ || index < 0 || index >= static_cast<int>(sprites_.size())) {
        return false;
    }
    BeforeChange();
    sprites_.erase(sprites_.begin() + index);
    ++revision_;
    return true;
}

bool SpriteEditModel::SetRect(int index, const spriteedit::IRect& rect)
{
    if (!loaded_ || index < 0 || index >= static_cast<int>(sprites_.size())) {
        return false;
    }
    const spriteedit::IRect r = spriteedit::ClampRectToTexture(rect, texW_, texH_);
    importmeta::SpriteSlice& s = sprites_[index];
    if (s.x == r.x && s.y == r.y && s.w == r.w && s.h == r.h) {
        return false;
    }
    BeforeChange();
    s.x = r.x;
    s.y = r.y;
    s.w = r.w;
    s.h = r.h;
    // border は新しい寸法に収める (左右の合計 <= 幅、上下の合計 <= 高さ)
    s.borderL = std::clamp(s.borderL, 0, s.w);
    s.borderR = std::clamp(s.borderR, 0, s.w - s.borderL);
    s.borderB = std::clamp(s.borderB, 0, s.h);
    s.borderT = std::clamp(s.borderT, 0, s.h - s.borderB);
    ++revision_;
    return true;
}

bool SpriteEditModel::SetName(int index, const char* name)
{
    if (!loaded_ || index < 0 || index >= static_cast<int>(sprites_.size()) || name == nullptr) {
        return false;
    }
    importmeta::SpriteSlice& s = sprites_[index];
    char next[sizeof(s.name)] = {};
    std::snprintf(next, sizeof(next), "%s", name);
    if (std::strcmp(s.name, next) == 0) {
        return false;
    }
    BeforeChange();
    std::memcpy(s.name, next, sizeof(s.name));
    ++revision_;
    return true;
}

bool SpriteEditModel::SetPivot(int index, float pivotX, float pivotY)
{
    if (!loaded_ || index < 0 || index >= static_cast<int>(sprites_.size())) {
        return false;
    }
    // 範囲外 (描画の外へ出る pivot) は許す: Unity も許す。非有限だけ拒む
    if (!std::isfinite(pivotX) || !std::isfinite(pivotY)) {
        return false;
    }
    importmeta::SpriteSlice& s = sprites_[index];
    if (s.pivotX == pivotX && s.pivotY == pivotY) {
        return false;
    }
    BeforeChange();
    s.pivotX = pivotX;
    s.pivotY = pivotY;
    ++revision_;
    return true;
}

bool SpriteEditModel::SetBorder(int index, int left, int bottom, int right, int top)
{
    if (!loaded_ || index < 0 || index >= static_cast<int>(sprites_.size())) {
        return false;
    }
    importmeta::SpriteSlice& s = sprites_[index];
    left = std::clamp(left, 0, s.w);
    right = std::clamp(right, 0, s.w - left);
    bottom = std::clamp(bottom, 0, s.h);
    top = std::clamp(top, 0, s.h - bottom);
    if (s.borderL == left && s.borderB == bottom && s.borderR == right && s.borderT == top) {
        return false;
    }
    BeforeChange();
    s.borderL = left;
    s.borderB = bottom;
    s.borderR = right;
    s.borderT = top;
    ++revision_;
    return true;
}

bool SpriteEditModel::ReplaceAll(std::vector<importmeta::SpriteSlice> sprites)
{
    if (!loaded_ || Serialize(sprites) == Serialize(sprites_)) {
        return false;
    }
    BeforeChange();
    sprites_ = std::move(sprites);
    ++revision_;
    return true;
}

bool SpriteEditModel::RevertToSaved()
{
    return ReplaceAll(saved_);
}

bool SpriteEditModel::Undo()
{
    if (!CanUndo()) {
        return false;
    }
    redo_.push_back(std::move(sprites_));
    sprites_ = std::move(undo_.back());
    undo_.pop_back();
    ++revision_;
    return true;
}

bool SpriteEditModel::Redo()
{
    if (!CanRedo()) {
        return false;
    }
    undo_.push_back(std::move(sprites_));
    sprites_ = std::move(redo_.back());
    redo_.pop_back();
    ++revision_;
    return true;
}

bool SpriteEditModel::BeginGesture()
{
    if (gesture_) {
        return false;
    }
    gesture_ = true;
    gesturePushed_ = false;
    return true;
}

void SpriteEditModel::EndGesture()
{
    gesture_ = false;
    gesturePushed_ = false;
}

bool SpriteEditModel::Save()
{
    if (!loaded_) {
        return false;
    }
    const std::wstring metaPath = path_ + L".meta";
    AssetDatabase::EnsureMeta(path_); // 不在なら生成 (GUID 確定)
    AssetMeta meta;
    if (!AssetDatabase::ReadMeta(metaPath, meta)) {
        return false;
    }
    meta.type = AssetType::Texture; // 旧 .meta の型欠落でも tex を書けるように
    meta.version = 2;
    meta.tex.sprites = sprites_;
    if (!AssetDatabase::WriteMeta(metaPath, meta)) {
        return false;
    }
    // .meta 単体の変更は ReloadHub が拾わないので、ここで登録し直す
    if (SpriteLibrary* lib = spritelib::Library()) {
        lib->Register(path_);
    }
    saved_ = sprites_;
    savedKey_ = Serialize(sprites_);
    ++revision_;
    return true;
}

} // namespace mye
