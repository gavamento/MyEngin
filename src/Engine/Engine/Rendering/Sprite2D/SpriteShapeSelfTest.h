//====================================================================================
//                          SpriteShapeSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          SpriteShape2D の輪郭・縁・塗り・衝突の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// EvaluateSpline / SelectEdge / BuildEdgeQuads / BuildFill の純関数、プロファイルの JSON、
// コンポーネントの登録 (hash 対象)、Sprite2DRenderer の収集、辺の箱の供給、丘を転がる球の決定論を検査する。
// GPU は使わない (Editor.exe --selftest)
bool RunSpriteShapeSelfTest();

} // namespace mye
