#pragma once

namespace mye {

// フォント基盤のヘッドレス回帰テスト (M34)。D3D/ウィンドウ/フォントファイル不要。
// UTF-8 デコード境界とシェルフパッカーの純関数 (FontGeometry.h) を検証する。全 PASS で true。
bool RunFontSelfTest();

} // namespace mye
