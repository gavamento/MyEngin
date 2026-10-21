//====================================================================================
//                          TwoDDemoSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          --2d-demo の 3000 tick 不変量と焼き込みハッシュの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// デモシーン (--2d-demo) を HeadlessSim で合成入力のまま 3000 tick 回し、全 plane2D の剛体と CC の z / vz / wx / wy / qx / qy が
// 初期値 / 0.0f のままであること、ワールドハッシュが焼き込み値 (Debug == Release) と一致することを検査する。
// 1 tick の sim 時間と、供給口つきの空間クエリ 1 本の時間もログに出す (StaticShapeSource の Collect の費用判断用)。
// GameLogic.dll の ZTwoDHero が無い構成ではハッシュの照合だけを飛ばす (D3D もウィンドウも作らない)
bool RunTwoDDemoSelfTest();

} // namespace mye
