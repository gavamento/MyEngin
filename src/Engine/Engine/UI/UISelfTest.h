#pragma once

namespace mye {

// ゲーム内 UI のヘッドレス回帰テスト (M21)。D3D/ウィンドウ不要。
// アンカー解決 (9-grid → 画面基準点) の純関数を検証する。全 PASS で true。
bool RunUISelfTest();

} // namespace mye
