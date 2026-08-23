#pragma once

namespace mye {

// 反射プローブのシーンキャプチャ基盤 (M56e) の自己テスト。
// 焼き上がりそのものは D3D が要るのでヘッドレスでは触れない — ここで固定するのは
// 「6 面のカメラが向いている方向」と「プリフィルタがサンプルする方向」が同じである、
// という**絵からは追えない**不変量のほう。
bool RunProbeBakerSelfTest();

} // namespace mye
