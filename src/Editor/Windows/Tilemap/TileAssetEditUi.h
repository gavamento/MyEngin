//====================================================================================
//                          TileAssetEditUi.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルマップのレイヤーとタイルパレットの編集 UI
//====================================================================================
#pragma once
#include <cstdint>

namespace mye {

struct EngineContext;
class TilemapEditModel;
class TilePaletteEditModel;

// タイルパレット窓の下半分に並べる描画だけの関数群。状態は呼び手 (窓) とモデルが持つ。
// 編集は全部モデル経由 (独自 Undo)。ImGui の連続した変更 (ドラッグ・入力) はモデルの BeginGesture / EndGesture で 1 段にまとめる
namespace tileui {

// レイヤーの一覧と、選択中のレイヤーの設定 (追加 / 削除 / 上下 / 名前 / ソート層 / 層内の順序 / 衝突)。
// layerIndex は窓が持つ現在のレイヤー添字で、一覧のクリックと追加・削除で動く
void DrawLayerPanel(EngineContext& ctx, TilemapEditModel& model, int& layerIndex);

// パレットのタイル追加 (スプライトから / スライス済みテクスチャから一括) と削除。
// 戻り値は新しく選ぶタイル id (0 = 変えない)。selectedTile を消したら 0 を返さず、呼び手が存在を見直す
uint32_t DrawPaletteToolbar(TilePaletteEditModel& model, uint32_t selectedTile);

// タイル 1 種の設定 (名前 / スプライト / 衝突 / 一方通行 / HP / 壊れた後 / エフェクト / ルール / アニメ)
void DrawTileInspector(EngineContext& ctx, TilePaletteEditModel& model, uint32_t tileId);

} // namespace tileui
} // namespace mye
