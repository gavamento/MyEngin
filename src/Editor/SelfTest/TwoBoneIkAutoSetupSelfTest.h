#pragma once

namespace mye {

// TwoBoneIK の自動設定 (骨名から手足を探し、鎖と手の目標の子を埋める) の回帰。
// **Editor 層に置く** — 自動設定がエディタ操作 (Add Component / 注意欄のボタン) の器だから
// (RagdollBuildSelfTest と同じ判断)。D3D もウィンドウも作らない (スケルトンは手で組む)
bool RunTwoBoneIkAutoSetupSelfTest();

} // namespace mye
