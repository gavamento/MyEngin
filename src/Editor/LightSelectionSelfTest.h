#pragma once

namespace mye {

// ライト選別 (M54b: 範囲球カリング + 決定論ソート + 上限 + 影スロット) のヘッドレス回帰テスト。
// LightSelection は D3D も ECS も触らない純関数なので、入力を作って出力を検査できる —
// 描画ロードマップ (M54〜M58) で唯一まともに自動被覆が取れる論理がここ
bool RunLightSelectionSelfTest();

} // namespace mye
