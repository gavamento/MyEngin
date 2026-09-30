//====================================================================================
//                          WaterSurfaceSelfTest.h
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          M79 sub-05: 水面サーフェス経路の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// WARP で ForwardPath / DeferredPath / ShadowPass / WaterPass を実際に動かし、
// RenderView::water (WaterDrawData) 経由で供給する MyEngineWater CB の値と
// gWaterTime (前後の水面時刻) が正しく反映されること、WaterPass::Render が
// useSurfaceRoute のときは重ねて描かないことを read-back で検証する (M79 sub-05)。
bool RunWaterSurfaceSelfTest();

} // namespace mye
