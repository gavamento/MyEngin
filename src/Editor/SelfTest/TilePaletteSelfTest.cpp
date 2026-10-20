//====================================================================================
//                          TilePaletteSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット窓の編集モデル (塗りの純関数と Undo) の回帰テスト
//====================================================================================
#include "Editor/SelfTest/TilePaletteSelfTest.h"

#include <cstdint>
#include <string>

#include "Editor/Windows/Tilemap/TilePaletteEditModel.h"
#include "Editor/Windows/Tilemap/TilemapEditModel.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"

namespace mye {
namespace {

using tileedit::EditResult;

constexpr uint32_t kWall = 1;
constexpr uint32_t kFloor = 2;

TilemapAsset MakeAsset(size_t layerCount)
{
    TilemapAsset a;
    a.name = "selftest";
    for (size_t i = 0; i < layerCount; ++i) {
        TilemapLayer l;
        l.name = "L" + std::to_string(i);
        a.layers.push_back(std::move(l));
    }
    return a;
}

// (x0, y0) - (x1, y1) の外周を壁で囲む
void BuildRing(TilemapEditModel& m, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    for (int32_t x = x0; x <= x1; ++x) {
        m.PaintCell(0, x, y0, kWall);
        m.PaintCell(0, x, y1, kWall);
    }
    for (int32_t y = y0; y <= y1; ++y) {
        m.PaintCell(0, x0, y, kWall);
        m.PaintCell(0, x1, y, kWall);
    }
}

} // namespace

bool RunTilePaletteSelfTest()
{
    MYE_LOG_INFO("==== TilePalette self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) 塗り潰し: 4 連結 ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(1));
        BuildRing(m, 0, 0, 4, 4);
        m.PaintCell(0, 4, 4, 0); // 角を抜く。内側 (3, 3) とは斜めにしか繋がらない
        size_t count = 0;
        const EditResult r = m.FloodFill(0, 2, 2, kFloor, &count);
        bool interiorFilled = true;
        for (int32_t y = 1; y <= 3; ++y) {
            for (int32_t x = 1; x <= 3; ++x) {
                interiorFilled = interiorFilled && m.GetCell(0, x, y) == kFloor;
            }
        }
        check(r == EditResult::Changed && count == 9 && interiorFilled, "flood fill: the 3x3 interior is filled (9 cells)");
        check(m.GetCell(0, 4, 4) == 0 && m.GetCell(0, 5, 5) == 0 && m.GetCell(0, -1, -1) == 0,
              "flood fill is 4-connected: it does not leak through the diagonal gap");
        check(m.GetCell(0, 0, 0) == kWall && m.GetCell(0, 4, 3) == kWall, "flood fill keeps the surrounding walls");

        // 同値の領域だけ: 壁 15 セル (16 - 抜いた角) が繋がっている
        size_t wallCount = 0;
        const EditResult wr = m.FloodFill(0, 0, 0, kFloor + 5, &wallCount);
        check(wr == EditResult::Changed && wallCount == 15 && m.GetCell(0, 2, 2) == kFloor && m.GetCell(0, 0, 0) == kFloor + 5,
              "flood fill: only the connected region of the same value is replaced (15 wall cells)");

        check(m.FloodFill(0, 2, 2, kFloor) == EditResult::NoChange, "flood fill: filling with the same value changes nothing");
        check(m.FloodFill(1, 0, 0, kFloor) == EditResult::Invalid && m.FloodFill(-1, 0, 0, kFloor) == EditResult::Invalid
                  && m.FloodFill(0, 40000, 0, kFloor) == EditResult::Invalid,
              "flood fill: out-of-range layer / cell is Invalid");
    }

    // ---- (b) 塗り潰しの上限 65536 ----
    {
        TilemapEditModel exact;
        exact.Load(MakeAsset(1));
        BuildRing(exact, -1, -1, 256, 256); // 内側 256 x 256 = 65536
        size_t count = 0;
        const EditResult r = exact.FloodFill(0, 0, 0, kFloor, &count);
        check(r == EditResult::Changed && count == TilemapEditModel::kMaxFloodCells && exact.GetCell(0, 255, 255) == kFloor,
              "flood fill limit: exactly 65536 cells is allowed");

        TilemapEditModel over;
        over.Load(MakeAsset(1));
        BuildRing(over, -1, -1, 256, 257); // 内側 256 x 257 = 65792
        const size_t depth = over.UndoDepth();
        const EditResult tooLarge = over.FloodFill(0, 0, 0, kFloor, &count);
        check(tooLarge == EditResult::TooLarge && count == 0 && over.GetCell(0, 0, 0) == 0 && over.GetCell(0, 255, 256) == 0
                  && over.UndoDepth() == depth,
              "flood fill limit: 65537+ cells is TooLarge and writes nothing (no Undo step)");

        TilemapEditModel open;
        open.Load(MakeAsset(1));
        check(open.FloodFill(0, 0, 0, kFloor) == EditResult::TooLarge, "flood fill limit: an unbounded empty area is TooLarge");
    }

    // ---- (c) 矩形 ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(2));
        size_t count = 0;
        check(m.FillRect(0, 3, 1, 1, 0, kFloor, &count) == EditResult::Changed && count == 6 && m.GetCell(0, 1, 0) == kFloor
                  && m.GetCell(0, 3, 1) == kFloor && m.GetCell(0, 4, 1) == 0,
              "rect: corners in any order, both ends inclusive (3x2 = 6 cells)");
        check(m.FillRect(0, 3, 1, 1, 0, kFloor, &count) == EditResult::NoChange && count == 0, "rect: the same value is NoChange");
        // チャンク境界 (-1 と 0、15 と 16) と負座標をまたぐ。既に kFloor の 6 セルは数えない
        check(m.FillRect(0, -2, -2, 17, 17, kFloor, &count) == EditResult::Changed && count == 20 * 20 - 6
                  && m.GetCell(0, -2, -2) == kFloor && m.GetCell(0, 17, 17) == kFloor && m.GetCell(0, 18, 18) == 0,
              "rect: crosses chunk boundaries and negative coordinates");
        check(m.GetCell(1, 0, 0) == 0, "rect: another layer is untouched");
        check(m.FillRect(0, 0, 0, 1000, 1000, kWall) == EditResult::TooLarge && m.GetCell(0, 1000, 1000) == 0,
              "rect: more than 262144 cells is TooLarge");
        check(m.FillRect(5, 0, 0, 1, 1, kWall) == EditResult::Invalid, "rect: out-of-range layer is Invalid");
    }

    // ---- (d) ピック・選択 ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(1));
        const uint32_t value = tilecell::Make(7, true, false, 3);
        m.PaintCell(0, -5, 9, value);
        check(m.PickCell(0, -5, 9) == value && tilecell::TileId(m.PickCell(0, -5, 9)) == 7
                  && (m.PickCell(0, -5, 9) & tilecell::kFlipX) != 0 && tilecell::Rotation(m.PickCell(0, -5, 9)) == 3,
              "pick: returns the tile id with flip and rotation");
        check(m.PickCell(0, 100, 100) == 0 && m.PickCell(3, 0, 0) == 0, "pick: an empty cell / invalid layer gives 0");

        m.FillRect(0, 0, 0, 3, 3, kFloor);
        m.SelectRect(2, 3, 1, 1);
        check(m.HasSelection() && m.Selection() == tileedit::CellRect{ 1, 1, 2, 3 }, "select: the rectangle is normalized");
        const size_t depth = m.UndoDepth();
        check(m.EraseSelection(0) == EditResult::Changed && m.GetCell(0, 1, 1) == 0 && m.GetCell(0, 2, 3) == 0
                  && m.GetCell(0, 0, 0) == kFloor && m.GetCell(0, 3, 3) == kFloor && m.UndoDepth() == depth + 1,
              "select: erasing the selection clears only the selected cells (1 Undo step)");
        m.Undo();
        check(m.GetCell(0, 1, 1) == kFloor && m.GetCell(0, 2, 3) == kFloor, "select: Undo restores the erased cells");
        m.ClearSelection();
        check(!m.HasSelection() && m.EraseSelection(0) == EditResult::NoChange, "select: without a selection nothing is erased");
    }

    // ---- (e) Undo / Redo (3 段)、ジェスチャ、上限、Dirty ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(1));
        check(!m.Dirty() && !m.CanUndo() && !m.CanRedo(), "undo: a freshly loaded model is clean with nothing to undo");
        m.PaintCell(0, 0, 0, kWall);
        m.PaintCell(0, 1, 0, kFloor);
        m.FillRect(0, 0, 1, 2, 2, kWall);
        check(m.UndoDepth() == 3 && m.Dirty(), "undo: 3 edits are 3 steps and the model is dirty");
        m.Undo();
        check(m.GetCell(0, 2, 2) == 0 && m.GetCell(0, 1, 0) == kFloor, "undo: step 3 (rect) is reverted");
        m.Undo();
        check(m.GetCell(0, 1, 0) == 0 && m.GetCell(0, 0, 0) == kWall, "undo: step 2 is reverted");
        m.Undo();
        check(m.GetCell(0, 0, 0) == 0 && !m.Dirty() && !m.CanUndo() && m.CanRedo(),
              "undo: step 1 is reverted; back to the saved state (clean)");
        m.Redo();
        m.Redo();
        m.Redo();
        check(m.GetCell(0, 0, 0) == kWall && m.GetCell(0, 1, 0) == kFloor && m.GetCell(0, 2, 2) == kWall && !m.CanRedo() && m.Dirty(),
              "redo: all 3 steps are re-applied");
        m.Undo();
        m.PaintCell(0, 9, 9, kWall);
        check(!m.CanRedo(), "redo: a new edit clears the redo stack");

        // ジェスチャ: ドラッグ 1 回 = 1 段
        TilemapEditModel g;
        g.Load(MakeAsset(1));
        g.BeginGesture();
        g.PaintCell(0, 0, 0, kWall);
        g.PaintCell(0, 1, 0, kWall);
        g.PaintCell(0, 2, 0, kWall);
        check(g.InGesture() && !g.CanUndo() && g.UndoDepth() == 1, "gesture: 3 paints inside one drag are 1 step (Undo is held back)");
        g.EndGesture();
        check(g.CanUndo() && g.UndoDepth() == 1, "gesture: after the drag Undo is available");
        g.Undo();
        check(g.GetCell(0, 0, 0) == 0 && g.GetCell(0, 1, 0) == 0 && g.GetCell(0, 2, 0) == 0 && !g.Dirty(),
              "gesture: one Undo reverts the whole drag");
        g.BeginGesture();
        g.EndGesture(); // 何も塗らないドラッグは段を積まない
        check(g.UndoDepth() == 0, "gesture: a drag that paints nothing leaves no step");

        // 上限 128 段
        TilemapEditModel cap;
        cap.Load(MakeAsset(1));
        for (int32_t i = 0; i < 140; ++i) {
            cap.PaintCell(0, i, 0, kWall);
        }
        check(cap.UndoDepth() == TilemapEditModel::kMaxUndo, "undo: the history is capped at 128 steps");

        // Dirty は保存時点との比較
        TilemapEditModel d;
        d.Load(MakeAsset(1));
        d.PaintCell(0, 0, 0, kWall);
        check(d.Dirty(), "dirty: an edit makes the model dirty");
        d.MarkSaved();
        check(!d.Dirty(), "dirty: MarkSaved makes it clean");
        d.Undo();
        check(d.Dirty(), "dirty: undoing past the saved state is dirty");
        d.Redo();
        check(!d.Dirty(), "dirty: redoing back to the saved state is clean");
        d.PaintCell(0, 5, 5, kFloor);
        d.EraseCell(0, 5, 5);
        check(!d.Dirty(), "dirty: painting and erasing the same cell compares equal to the saved state");
    }

    // ---- (f) 保存形式の往復 ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(2));
        m.FillRect(0, -3, -3, 20, 2, tilecell::Make(4, false, true, 1));
        m.PaintCell(1, 40, 40, kFloor);
        m.EraseCell(1, 40, 40); // チャンクごと消える (空のチャンクは JSON に書かれない)
        const nlohmann::json j = TilemapLibrary::ToJson(m.Asset());
        TilemapAsset back;
        const bool parsed = TilemapLibrary::FromJson(j, back);
        check(parsed && m.SameLayers(back) && back.layers.size() == 2 && back.layers[1].chunks.empty(),
              "json: the edited cells round-trip and the emptied chunk is not written");
        check(TilemapLibrary::ToJson(back) == j, "json: write -> read -> write is identical");
    }

    // ---- (g) SpriteLibrary の版 (チャンクキャッシュの無効化) ----
    {
        SpriteLibrary lib;
        const uint64_t e0 = lib.Epoch();
        lib.Register(L"selftest_tile_palette_nonexistent.png");
        const uint64_t e1 = lib.Epoch();
        lib.Register(L"selftest_tile_palette_nonexistent.png");
        const uint64_t e2 = lib.Epoch();
        lib.Clear();
        const uint64_t e3 = lib.Epoch();
        check(e1 > e0 && e2 > e1 && e3 > e2, "sprite library: Epoch increases on every Register and Clear");
    }

    // ---- (h) レイヤー: 3 枚の順序入れ替えの往復 JSON と Undo (M94m) ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(3)); // L0 L1 L2
        m.PaintCell(0, 1, 1, kWall);
        m.PaintCell(1, 2, 2, kFloor);
        m.PaintCell(2, 3, 3, kWall);
        m.MarkSaved();
        const TilemapAsset before = m.Asset();
        check(m.MoveLayer(0, 1) == EditResult::Changed && m.MoveLayer(1, 1) == EditResult::Changed,
              "layers: two moves change the order");
        check(m.Asset().layers[0].name == "L1" && m.Asset().layers[1].name == "L2" && m.Asset().layers[2].name == "L0",
              "layers: the order is L1 L2 L0 after Move(0, +1) and Move(1, +1)");
        check(m.GetCell(0, 2, 2) == kFloor && m.GetCell(1, 3, 3) == kWall && m.GetCell(2, 1, 1) == kWall,
              "layers: the cells travel with their layer");
        const nlohmann::json j = TilemapLibrary::ToJson(m.Asset());
        TilemapAsset back;
        check(TilemapLibrary::FromJson(j, back) && m.SameLayers(back) && TilemapLibrary::ToJson(back) == j
                  && back.layers[0].name == "L1" && back.layers[1].name == "L2" && back.layers[2].name == "L0",
              "layers: the reordered layers round-trip through JSON in the same order");
        check(m.Dirty(), "layers: reordering makes the model dirty even though no cell value changed");
        check(m.Undo() && m.Undo() && m.SameLayers(before), "layers: two Undo steps restore the original order and cells");
        check(!m.Dirty(), "layers: undoing all the way back is clean again");
        check(m.Redo() && m.Redo() && m.Asset().layers[0].name == "L1", "layers: Redo re-applies the moves");

        check(m.MoveLayer(0, -1) == EditResult::NoChange && m.MoveLayer(2, 1) == EditResult::NoChange
                  && m.MoveLayer(5, 1) == EditResult::Invalid,
              "layers: moving past either end is NoChange, an out-of-range layer is Invalid");
    }

    // ---- (i) レイヤー: 追加 / 削除 / 設定と、Undo の添字 ----
    {
        TilemapEditModel m;
        m.Load(MakeAsset(1));
        for (size_t i = 1; i < kMaxTilemapLayers; ++i) {
            check(m.AddLayer("A" + std::to_string(i)) == EditResult::Changed, "layers: add up to the limit");
        }
        check(m.AddLayer("over") == EditResult::Invalid && m.Asset().layers.size() == kMaxTilemapLayers,
              "layers: the 9th layer is rejected");
        // 削除をまたぐ Undo: 先に塗った (layer 3) のセルが、削除の Undo 後も同じ layer 3 に戻る
        m.PaintCell(3, 5, 5, kWall);
        check(m.RemoveLayer(1) == EditResult::Changed && m.GetCell(2, 5, 5) == kWall && m.GetCell(3, 5, 5) == 0,
              "layers: removing layer 1 shifts the cells of the later layers down");
        check(m.Undo() && m.GetCell(3, 5, 5) == kWall && m.Asset().layers.size() == kMaxTilemapLayers,
              "layers: Undo of a removal restores the layer and the cell index");
        check(m.Undo() && m.GetCell(3, 5, 5) == 0, "layers: the next Undo reverts the paint made before the removal");
        m.Redo();
        m.Redo();
        TilemapEditModel single;
        single.Load(MakeAsset(1));
        check(single.RemoveLayer(0) == EditResult::Invalid, "layers: the last layer cannot be removed");

        // 設定のドラッグ 1 回 = Undo 1 段
        TilemapEditModel p;
        p.Load(MakeAsset(2));
        TilemapLayer props;
        props.name = "Ground";
        props.sortingLayerId = 0;
        props.orderInLayer = 0;
        props.collision = true;
        const size_t depth0 = p.UndoDepth();
        p.BeginGesture();
        for (int order = 1; order <= 5; ++order) {
            props.orderInLayer = order;
            p.SetLayerProps(1, props);
        }
        p.EndGesture();
        check(p.UndoDepth() == depth0 + 1 && p.Asset().layers[1].orderInLayer == 5 && p.Asset().layers[1].name == "Ground",
              "layers: a drag of the order is one Undo step");
        check(p.Undo() && p.Asset().layers[1].orderInLayer == 0 && p.Asset().layers[1].name == "L1",
              "layers: undoing the drag restores the original settings");
        check(p.SetLayerProps(1, p.Asset().layers[1]) == EditResult::NoChange, "layers: the same settings are NoChange");
    }

    // ---- (j) パレット編集: タイルの追加 / 削除 / 設定 / Undo / 往復 (M94m) ----
    {
        TilePalette base;
        base.name = "selftest";
        base.Add("grass", AssetID{ 11 });
        TilePaletteEditModel m;
        m.Load(base);
        uint32_t id = 0;
        check(m.AddTile("dirt", AssetID{ 12 }, &id) && id == 2 && m.Palette().tiles.size() == 2, "palette: a new tile takes max id + 1");
        check(m.AddTiles({ { "a", AssetID{ 21 } }, { "b", AssetID{ 22 } }, { "c", AssetID{ 23 } } }) == 3
                  && m.Palette().tiles.back().id == 5 && m.UndoDepth() == 2,
              "palette: adding the slices of a texture is one Undo step (ids 3..5)");
        check(m.Undo() && m.Palette().tiles.size() == 2, "palette: one Undo removes all the added slices");
        check(m.Redo() && m.Palette().tiles.size() == 5, "palette: Redo adds them again");

        // 設定: ルール / アニメ / 破壊
        TileDef d = *m.Palette().Find(1);
        d.hasRule = true;
        d.variants = { { "...ox...", AssetID{ 31 } }, { "...xx...", AssetID{ 32 } } };
        d.hasAnim = true;
        d.ticksPerFrame = 8;
        d.animFrames = { AssetID{ 41 }, AssetID{ 42 } };
        d.hp = 3;
        d.destroyedTile = 2;
        d.collider = 1;
        check(m.SetTile(d), "palette: SetTile changes the tile");
        check(!m.SetTile(d), "palette: the same definition is not a change");
        TilePalette round;
        check(TilePaletteLibrary::FromJson(TilePaletteLibrary::ToJson(m.Palette()), round) && m.SameTiles(round),
              "palette: rule, anim and destruction settings round-trip through JSON");
        check(m.Dirty(), "palette: edited palette is dirty");

        // 範囲外は丸める
        TileDef wild = *m.Palette().Find(1);
        wild.ticksPerFrame = 0;
        wild.hp = 9999;
        m.SetTile(wild);
        check(m.Palette().Find(1)->ticksPerFrame == 1 && m.Palette().Find(1)->hp == 255,
              "palette: out-of-range ticksPerFrame / hp are clamped like a loaded file");

        // ドラッグ 1 回 = 1 段
        const size_t depth = m.UndoDepth();
        m.BeginGesture();
        for (int hp = 1; hp <= 6; ++hp) {
            TileDef t = *m.Palette().Find(2);
            t.hp = hp;
            m.SetTile(t);
        }
        m.EndGesture();
        check(m.UndoDepth() == depth + 1 && m.Palette().Find(2)->hp == 6, "palette: a drag of hp is one Undo step");

        // 削除: 破壊後のタイルとして指されていたら 0 に戻す
        check(m.RemoveTile(2) && m.Palette().Find(2) == nullptr && m.Palette().Find(1)->destroyedTile == 0,
              "palette: removing a tile clears the destroyedTile that pointed to it");
        check(m.Undo() && m.Palette().Find(2) != nullptr && m.Palette().Find(1)->destroyedTile == 2,
              "palette: Undo of a removal restores the tile and the reference");
        check(!m.RemoveTile(99) && !m.SetTile(TileDef{}), "palette: unknown ids are rejected");

        // 保存済みに戻ればクリーン
        TilePaletteEditModel c;
        c.Load(base);
        c.AddTile("x", AssetID{ 1 });
        c.Undo();
        check(!c.Dirty(), "palette: undoing back to the loaded content is clean");
    }

    MYE_LOG_INFO("==== TilePalette self test %s (%d failure(s)) ====", failCount == 0 ? "PASS" : "FAIL", failCount);
    return failCount == 0;
}

} // namespace mye
