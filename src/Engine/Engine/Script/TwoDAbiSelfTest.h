//====================================================================================
//                          TwoDAbiSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/21/2026
//                                          ABI v31 (2D: 箱のクエリ・スプライト名・タイルマップ) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 10 スロットの充填、Tilemap の読み書き・損傷・座標変換・範囲、Tilemap が無いときの失敗値、
// 空間クエリ (Raycast / OverlapBoxMasked / BoxCast ほか) がタイルの箱を見ること、SpriteByName を検査する。
// D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunTwoDAbiSelfTest();

} // namespace mye
