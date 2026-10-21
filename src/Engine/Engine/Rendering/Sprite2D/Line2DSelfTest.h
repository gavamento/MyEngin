//====================================================================================
//                          Line2DSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          Line2D の帯・塗りの頂点生成と描画リストの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// BuildStroke (継ぎ目・ループ・境界) / Triangulate (凸・凹・向き) / Sprite2DRenderer の Line2D 収集を検査する。
// WARP デバイスだけ作る (Editor.exe --selftest)
bool RunLine2DSelfTest();

} // namespace mye
