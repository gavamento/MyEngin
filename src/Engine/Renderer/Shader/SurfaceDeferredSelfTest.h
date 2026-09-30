//====================================================================================
//                          SurfaceDeferredSelfTest.h
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          M79 sub-03: Deferred サーフェス段・CSM 影の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// WARP で DeferredPath / ShadowPass を実際に動かし、gTime 駆動の頂点変位が
// 画面速度 (gbVelocity) と CSM シャドウ深度に反映されること、サーフェスが GBuffer を
// 経由せず HDR シーンへ直接描かれることを read-back で検証する (M79 sub-03 round 2)。
bool RunSurfaceDeferredSelfTest();

} // namespace mye
