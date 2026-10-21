//====================================================================================
//                          GridNav2DSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          GridAgent2D (タイルマップ上のグリッド A*) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 10x10 の面 (段差 2・穴 1) を歩いて越えて Arrived、到達不能で NoPath、展開上限を超える広い面で Moving のまま近づく、
// 目的地の取り下げで止まる、CC が無い / 2D でないと Inactive、2 ワールド並走の一致と Debug / Release ハッシュの焼き込み、
// SimSnapshot 往復後に続きが一致することを検査する。D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunGridNav2DSelfTest();

} // namespace mye
