//====================================================================================
//                          TilemapSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          タイルパレット / タイルマップ資産・座標変換・衝突・書き換え・破壊の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// .tilemap.json / .tilepalette.json の往復と空チャンク、GetCell と座標変換、パレットの id 割り当て、
// タイルの flip / 回転 UV、タイル衝突 (行結合の箱・一方通行・チャンク絞り込み)、セル書き換えの hash / snapshot、
// 破壊を検査する。D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunTilemapSelfTest();

} // namespace mye
