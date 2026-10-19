//====================================================================================
//                          SpriteEditorSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトエディタの編集モデルとスライス純関数の回帰テスト
//====================================================================================
#include "Editor/SelfTest/SpriteEditorSelfTest.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "Editor/Windows/Sprite/SpriteEditModel.h"
#include "Engine/Core/Asset/ImportMetaResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"

namespace fs = std::filesystem;

namespace mye {
namespace {

using spriteedit::IRect;

// RGBA8 の透明な画像に不透明の矩形を塗る
void Fill(std::vector<uint8_t>& px, int texW, const IRect& r)
{
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * texW + x) * 4];
            p[0] = 255;
            p[1] = 255;
            p[2] = 255;
            p[3] = 255;
        }
    }
}

void FillPixel(std::vector<uint8_t>& px, int texW, int x, int y)
{
    Fill(px, texW, { x, y, 1, 1 });
}

bool SameRects(const std::vector<IRect>& a, const std::vector<IRect>& b)
{
    return a == b;
}

importmeta::SpriteSlice MakeSlice(uint32_t id, const char* name, int x, int y, int w, int h)
{
    importmeta::SpriteSlice s;
    s.id = id;
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    s.x = x;
    s.y = y;
    s.w = w;
    s.h = h;
    return s;
}

} // namespace

bool RunSpriteEditorSelfTest()
{
    MYE_LOG_INFO("==== SpriteEditor self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) グリッド分割 ----
    {
        const std::vector<IRect> grid = spriteedit::SliceGridByCellSize(64, 64, 16, 16, 0, 0, 0, 0, true);
        check(grid.size() == 16 && grid[0] == IRect{ 0, 0, 16, 16 } && grid[1] == IRect{ 16, 0, 16, 16 }
                  && grid[5] == IRect{ 16, 16, 16, 16 } && grid[15] == IRect{ 48, 48, 16, 16 },
              "grid by cell size: 64x64 / 16x16 gives 4x4 = 16 cells in row-major order");

        const std::vector<IRect> padded = spriteedit::SliceGridByCellSize(74, 74, 16, 16, 2, 2, 2, 2, true);
        check(padded.size() == 16 && padded[0] == IRect{ 2, 2, 16, 16 } && padded[1] == IRect{ 20, 2, 16, 16 }
                  && padded[15] == IRect{ 56, 56, 16, 16 },
              "grid by cell size: offset 2 / padding 2 shifts the cells");

        const std::vector<IRect> partial = spriteedit::SliceGridByCellSize(70, 70, 16, 16, 0, 0, 0, 0, true);
        check(partial.size() == 16, "grid by cell size: the remainder (70 = 4 * 16 + 6) is dropped");

        const std::vector<IRect> byCount = spriteedit::SliceGridByCellCount(64, 64, 4, 4, 0, 0, 0, 0, true);
        check(SameRects(byCount, grid), "grid by cell count 4x4 equals grid by cell size 16x16");
        const std::vector<IRect> byCountPad = spriteedit::SliceGridByCellCount(64, 64, 4, 4, 0, 0, 2, 2, true);
        check(byCountPad.size() == 16 && byCountPad[0] == IRect{ 0, 0, 14, 14 } && byCountPad[1] == IRect{ 16, 0, 14, 14 },
              "grid by cell count: cell = (64 - 2 * 3) / 4 = 14 with padding 2");

        check(spriteedit::SliceGridByCellSize(64, 64, 0, 16, 0, 0, 0, 0, true).empty()
                  && spriteedit::SliceGridByCellSize(8, 8, 16, 16, 0, 0, 0, 0, true).empty()
                  && spriteedit::SliceGridByCellCount(64, 64, 0, 4, 0, 0, 0, 0, true).empty()
                  && spriteedit::SliceGridByCellCount(4, 4, 8, 8, 0, 0, 0, 0, true).empty(),
              "grid: zero / oversized cells give no rectangles");

        // 空セルを捨てる: セル (1, 0) と (2, 3) だけ不透明
        std::vector<uint8_t> px(64 * 64 * 4, 0);
        Fill(px, 64, { 20, 3, 2, 2 });
        Fill(px, 64, { 33, 50, 1, 1 });
        const std::vector<IRect> kept = spriteedit::SliceGridByCellSize(64, 64, 16, 16, 0, 0, 0, 0, true, px.data());
        const std::vector<IRect> dropped = spriteedit::SliceGridByCellSize(64, 64, 16, 16, 0, 0, 0, 0, false, px.data());
        check(kept.size() == 16 && dropped.size() == 2 && dropped[0] == IRect{ 16, 0, 16, 16 }
                  && dropped[1] == IRect{ 32, 48, 16, 16 },
              "keepEmpty = false drops fully transparent cells (keepEmpty = true keeps all)");
    }

    // ---- (b) 自動分割 ----
    {
        // 64x32 に 16 画素の正方形を市松に 4 枚。斜めにしか接しないので 4 連結では別々
        std::vector<uint8_t> checker(64 * 32 * 4, 0);
        Fill(checker, 64, { 0, 0, 16, 16 });
        Fill(checker, 64, { 32, 0, 16, 16 });
        Fill(checker, 64, { 16, 16, 16, 16 });
        Fill(checker, 64, { 48, 16, 16, 16 });
        const std::vector<IRect> rects = spriteedit::SliceAutomatic(checker.data(), 64, 32, 4);
        const std::vector<IRect> expect = { { 0, 0, 16, 16 }, { 32, 0, 16, 16 }, { 16, 16, 16, 16 }, { 48, 16, 16, 16 } };
        check(SameRects(rects, expect), "automatic: a 4-tile checker gives 4 rectangles (diagonal tiles stay apart)");

        // 連結成分 3 個: 4x3 の塊 / L 字 / 孤立 1 画素
        std::vector<uint8_t> blobs(20 * 12 * 4, 0);
        Fill(blobs, 20, { 2, 1, 4, 3 });
        Fill(blobs, 20, { 10, 2, 1, 5 });
        Fill(blobs, 20, { 10, 6, 4, 1 });
        FillPixel(blobs, 20, 17, 9);
        const std::vector<IRect> all = spriteedit::SliceAutomatic(blobs.data(), 20, 12, 1);
        check(all.size() == 3 && all[0] == IRect{ 2, 1, 4, 3 } && all[1] == IRect{ 10, 2, 4, 5 }
                  && all[2] == IRect{ 17, 9, 1, 1 },
              "automatic: 3 connected components -> 3 bounding boxes (L shape covers its bounding box)");
        const std::vector<IRect> big = spriteedit::SliceAutomatic(blobs.data(), 20, 12, 2);
        check(big.size() == 2 && big[0] == IRect{ 2, 1, 4, 3 } && big[1] == IRect{ 10, 2, 4, 5 },
              "automatic: minSize 2 drops the 1-pixel speck");

        // アルファ 0 の画素 (色があっても) は背景
        std::vector<uint8_t> invisible(8 * 8 * 4, 0);
        for (size_t i = 0; i < invisible.size(); i += 4) {
            invisible[i] = 255; // RGB だけ有色、A = 0
        }
        check(spriteedit::SliceAutomatic(invisible.data(), 8, 8, 1).empty(), "automatic: alpha 0 is background");
        check(spriteedit::SliceAutomatic(nullptr, 8, 8, 1).empty(), "automatic: null pixels give nothing");
    }

    // ---- (c) ApplySlices ----
    {
        const std::vector<IRect> cells = spriteedit::SliceGridByCellSize(32, 32, 16, 16, 0, 0, 0, 0, true);
        std::vector<importmeta::SpriteSlice> existing;
        existing.push_back(MakeSlice(7, "hero_0", 0, 0, 16, 16));
        existing.push_back(MakeSlice(8, "keep_me", 8, 8, 4, 4));

        const auto safe = spriteedit::ApplySlices(existing, cells, spriteedit::SliceMethod::Safe, 0.5f, 0.0f, "hero", 9);
        // 既存 2 個 + 新規 4 個のうち (0,0) のセルは既存と重なるので捨てる = 5 個
        check(safe.size() == 5 && safe[0].id == 7 && safe[1].id == 8 && safe[2].id == 9 && safe[4].id == 11,
              "Safe: keeps existing, drops new rects that overlap them, new ids continue from firstId");
        check(safe[2].x == 16 && safe[2].y == 0 && safe[2].pivotX == 0.5f && safe[2].pivotY == 0.0f,
              "Safe: new slices take the requested pivot");
        bool uniqueNames = true;
        for (size_t i = 0; i < safe.size(); ++i) {
            for (size_t j = i + 1; j < safe.size(); ++j) {
                uniqueNames = uniqueNames && std::strcmp(safe[i].name, safe[j].name) != 0;
            }
        }
        check(uniqueNames, "Safe: generated names do not collide with existing names");

        const auto del = spriteedit::ApplySlices(existing, cells, spriteedit::SliceMethod::DeleteExisting, 0.5f, 0.5f,
                                                 "hero", 9);
        check(del.size() == 4 && del[0].id == 9 && del[3].id == 12 && std::strcmp(del[0].name, "hero_0") == 0
                  && std::strcmp(del[3].name, "hero_3") == 0,
              "Delete Existing: replaces everything, ids from firstId, names hero_0..3");
        check(spriteedit::RectsOverlap({ 0, 0, 4, 4 }, { 3, 3, 4, 4 }) && !spriteedit::RectsOverlap({ 0, 0, 4, 4 }, { 4, 0, 4, 4 }),
              "RectsOverlap: sharing an edge is not an overlap");
    }

    // ---- (d) pivot プリセット ----
    {
        const float expect[9][2] = { { 0.5f, 0.5f }, { 0.0f, 1.0f }, { 0.5f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.5f },
                                     { 1.0f, 0.5f }, { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 1.0f, 0.0f } };
        bool ok = true;
        for (int k = 0; k < spriteedit::kPivotPresetCount; ++k) {
            float x = -1.0f;
            float y = -1.0f;
            spriteedit::PivotPreset(k, x, y);
            ok = ok && x == expect[k][0] && y == expect[k][1] && spriteedit::PivotPresetOf(x, y) == k;
        }
        check(ok, "pivot presets: 9 values (Center, TopLeft ... BottomRight) round-trip through PivotPresetOf");
        check(spriteedit::PivotPresetOf(0.3f, 0.3f) == spriteedit::kPivotCustom, "pivot: a free value is Custom");
    }

    // ---- (e) 編集モデル: Undo / Redo / ジェスチャ / Dirty ----
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / L"mye_sprite_editor_selftest";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    {
        const fs::path tex = root / L"edit.png"; // .meta の無いテクスチャ
        SpriteEditModel model;
        check(model.Load(tex.wstring(), 64, 64) && model.IsLoaded() && model.Sprites().empty() && !model.Dirty(),
              "model: a texture without a .meta opens with no sprites and is clean");

        model.AddRect({ 0, 0, 16, 16 }, 0.5f, 0.5f, "a");
        model.AddRect({ 16, 0, 16, 16 }, 0.5f, 0.5f, "b");
        model.AddRect({ 32, 0, 16, 16 }, 0.5f, 0.5f, "c");
        check(model.Sprites().size() == 3 && model.Dirty() && model.UndoDepth() == 3, "model: 3 edits are 3 undo steps");
        check(model.Sprites()[2].id == 3, "model: new ids are max + 1");
        model.Undo();
        model.Undo();
        model.Undo();
        check(model.Sprites().empty() && !model.Dirty() && !model.CanUndo() && model.CanRedo(),
              "model: Undo x3 returns to the opened state (clean again)");
        model.Redo();
        model.Redo();
        model.Redo();
        check(model.Sprites().size() == 3 && std::strcmp(model.Sprites()[2].name, "c") == 0 && model.Dirty(),
              "model: Redo x3 restores the 3 sprites");

        // ジェスチャは 1 段
        const size_t depth = model.UndoDepth();
        model.BeginGesture();
        for (int i = 1; i <= 5; ++i) {
            model.SetRect(0, { i, i, 16, 16 });
        }
        check(!model.CanUndo(), "model: Undo is closed while a gesture is open");
        model.EndGesture();
        check(model.UndoDepth() == depth + 1 && model.Sprites()[0].x == 5, "model: a 5-step drag is 1 undo step");
        model.Undo();
        check(model.Sprites()[0].x == 0, "model: undoing the gesture restores the position before the drag");

        // 値が変わらない呼び出しは段を積まない
        const size_t before = model.UndoDepth();
        model.SetRect(0, { 0, 0, 16, 16 });
        model.SetName(0, "a");
        model.SetPivot(0, 0.5f, 0.5f);
        check(model.UndoDepth() == before, "model: a no-op edit does not push an undo step");

        // 範囲に収める
        model.SetRect(1, { 60, 60, 100, 100 });
        check(model.Sprites()[1].x == 60 && model.Sprites()[1].w == 4 && model.Sprites()[1].h == 4,
              "model: SetRect clamps the rectangle into the texture");
        model.SetBorder(1, 10, 10, 10, 10);
        const importmeta::SpriteSlice& b = model.Sprites()[1];
        check(b.borderL == 4 && b.borderR == 0 && b.borderB == 4 && b.borderT == 0,
              "model: SetBorder keeps left+right <= width and bottom+top <= height");

        // 履歴は 128 段まで
        SpriteEditModel deep;
        deep.Load(tex.wstring(), 64, 64);
        deep.AddRect({ 0, 0, 8, 8 }, 0.5f, 0.5f, "x");
        for (int i = 0; i < 200; ++i) {
            deep.SetRect(0, { i % 50, 0, 8, 8 });
            deep.SetRect(0, { (i % 50) + 1, 0, 8, 8 });
        }
        check(deep.UndoDepth() == SpriteEditModel::kMaxUndo, "model: undo history is capped at 128 steps");

        model.RevertToSaved();
        check(model.Sprites().empty() && !model.Dirty(), "model: RevertToSaved goes back to the opened state");
        model.Undo();
        check(!model.Sprites().empty(), "model: Revert itself can be undone");
    }

    // ---- (f) 保存: sprites だけ差し替え、filter / ppu を保ち、SpriteLibrary を登録し直す ----
    {
        const fs::path tex = root / L"save.png";
        {
            std::ofstream f(tex, std::ios::binary);
            f << "dummy";
        }
        AssetMeta seed;
        seed.guid = 0xCD12EF3456789ABCull;
        seed.type = AssetType::Texture;
        seed.version = 2;
        seed.tex.filter = 2;
        seed.tex.pixelsPerUnit = 16;
        seed.tex.srgb = 2;
        AssetDatabase::WriteMeta(tex.wstring() + L".meta", seed);

        AssetDatabase db;
        db.ScanAndSync(root.wstring());
        db.InstallAsKeyResolver();
        SpriteLibrary lib;
        spritelib::Install(&lib);

        SpriteEditModel model;
        model.Load(tex.wstring(), 64, 64);
        model.ReplaceAll(spriteedit::ApplySlices(model.Sprites(), spriteedit::SliceGridByCellSize(64, 64, 32, 32, 0, 0, 0, 0, true),
                                                 spriteedit::SliceMethod::DeleteExisting, 0.5f, 0.0f, "save", model.NextId()));
        check(model.Sprites().size() == 4 && model.Dirty(), "save: Apply produced 4 sprites (dirty)");

        // Inspector が Import Settings を書き換えたあとでも、保存は sprites だけを差し替える
        AssetMeta mid;
        AssetDatabase::ReadMeta(tex.wstring() + L".meta", mid);
        mid.tex.pixelsPerUnit = 32;
        AssetDatabase::WriteMeta(tex.wstring() + L".meta", mid);

        check(model.Save() && !model.Dirty(), "save: Save succeeds and clears Dirty");
        AssetMeta after;
        AssetDatabase::ReadMeta(tex.wstring() + L".meta", after);
        check(after.tex.sprites.size() == 4 && after.tex.filter == 2 && after.tex.pixelsPerUnit == 32 && after.tex.srgb == 2
                  && after.guid == seed.guid,
              "save: .meta has 4 sprites and keeps guid / filter / srgb / the newer ppu");
        const SpriteDef* second = lib.Get(SpriteLibrary::SliceId(tex.wstring(), after.tex.sprites[1].id));
        check(second != nullptr && second->rect[0] == 32 && second->rect[1] == 0 && second->ppu == 32
                  && second->pivot[1] == 0.0f,
              "save: SpriteLibrary was re-registered from the saved .meta");

        SpriteEditModel reopened;
        reopened.Load(tex.wstring(), 64, 64);
        check(reopened.Sprites().size() == 4 && !reopened.Dirty() && reopened.NextId() == after.tex.sprites[3].id + 1,
              "save: reopening reads the same 4 sprites");

        // ---- (g) .meta の重複 id は後勝ち (spec R6) ----
        AssetMeta dup = after;
        dup.tex.sprites.clear();
        dup.tex.sprites.push_back(MakeSlice(1, "first", 0, 0, 8, 8));
        dup.tex.sprites.push_back(MakeSlice(2, "other", 8, 0, 8, 8));
        dup.tex.sprites.push_back(MakeSlice(1, "second", 16, 0, 8, 8));
        AssetDatabase::WriteMeta(tex.wstring() + L".meta", dup);
        const int registered = lib.Register(tex.wstring());
        const SpriteDef* winner = lib.Get(SpriteLibrary::SliceId(tex.wstring(), 1));
        check(registered == 2 && winner != nullptr && std::strcmp(winner->name, "second") == 0 && winner->rect[0] == 16,
              "duplicate id in the .meta: the later entry wins (and a warning is logged)");
        std::vector<SpriteListing> list;
        lib.Enumerate(list);
        check(list.size() == 3, "duplicate id: the picker lists the full texture + 2 distinct sprites");

        spritelib::Install(nullptr);
        AssetDatabase::UninstallKeyResolver();
    }

    fs::remove_all(root, ec);
    MYE_LOG_INFO("==== SpriteEditor self test %s (%d failure(s)) ====", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
