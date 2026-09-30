#pragma once

namespace mye {

// ラグドール生成器の回帰 (M60g2)。**Editor 層に置く** — 生成器そのものがエディタ時の
// 器なので、Engine 層の `RagdollSelfTest` (駆動側 = M60g1) とは持ち場が違う。
// D3D もウィンドウも作らない (スケルトンは手で組み、RenderResources は Init しない)。
bool RunRagdollBuildSelfTest();

} // namespace mye
