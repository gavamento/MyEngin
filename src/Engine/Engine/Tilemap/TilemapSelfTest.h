//====================================================================================
//                          TilemapSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産と座標変換の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// .tilemap.json / .tilepalette.json の往復と空チャンク、GetCell と座標変換、パレットの id 割り当て、
// タイルの flip / 回転 UV を検査する。D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunTilemapSelfTest();

} // namespace mye
