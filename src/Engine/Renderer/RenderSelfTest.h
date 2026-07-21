#pragma once

namespace mye {

// レンダ系の回帰テスト (Editor.exe --selftest で実行)。
// 現状は視錐台カリング (FrustumCull.h) の内外判定を検証する (M16)。
bool RunRenderSelfTest();

} // namespace mye
