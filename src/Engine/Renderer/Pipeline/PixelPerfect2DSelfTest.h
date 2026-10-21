//====================================================================================
//                          PixelPerfect2DSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          pixelperfect2d::Compute (整数倍率・低解像度 RT・orthoSize) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 画面 x 基準解像度の表 (960x540 -> zoom 3 / 320x180 / 5.625、1280x720 -> 4、959x540 -> 2 と奇数処理など) と、
// 画面を覆う最小の偶数 RT であることの総当たり検査。D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunPixelPerfect2DSelfTest();

} // namespace mye
