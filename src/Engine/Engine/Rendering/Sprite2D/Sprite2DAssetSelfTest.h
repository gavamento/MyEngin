//====================================================================================
//                          Sprite2DAssetSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライト資産とソート層の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// .meta の sprites / filter / PPU の往復、SpriteLibrary の AssetID 解決、SortingLayers の編集と保存を検査する。
// D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunSprite2DAssetSelfTest();

} // namespace mye
