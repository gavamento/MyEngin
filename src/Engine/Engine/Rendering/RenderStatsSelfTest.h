//====================================================================================
//                          RenderStatsSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          描画統計のビュー別集計と JSON ダンプの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// prof::RenderStats のビュー別集計 (和が累積値と一致する / 影が本描画の欄に混ざらない) と、
// --render-stats-dump の JSON (決定的な数と GPU 時間が別の節) を検査する。
// D3D もウィンドウも作らない (Editor.exe --selftest)
bool RunRenderStatsSelfTest();

} // namespace mye
