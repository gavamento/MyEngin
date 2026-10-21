//====================================================================================
//                          Camera2DFollowSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          Camera2DFollow (2D カメラ追従) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// デッドゾーン内で動かない / 外れると damping で追う数値 / 軸ごとの damping / 先読み (Rigidbody と CC) / オフセット /
// 手書きの境界と Tilemap 衝突レイヤーの境界 (非衝突レイヤーは無視、衝突レイヤーの和) / 親子の連鎖 / 対象なし /
// 2 ワールド並走の一致と Debug / Release ハッシュの焼き込みを検査する。D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunCamera2DFollowSelfTest();

} // namespace mye
