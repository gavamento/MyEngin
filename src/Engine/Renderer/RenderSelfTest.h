#pragma once

namespace mye {

// レンダ系の回帰テスト (Editor.exe --selftest で実行)。
// 視錐台カリング (FrustumCull.h、M16) の内外判定のほか、描画数式の CPU ミラー
// (深度線形化 / 発光の符号化 / フロクセルなど) を検証する。
bool RunRenderSelfTest();

} // namespace mye
