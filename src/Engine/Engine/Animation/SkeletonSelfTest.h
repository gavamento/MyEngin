#pragma once

namespace mye {

// 骨ポーズ決定論スパイク (M48a)。実アセット (CesiumMan.glb / skinned_beam.fbx) をヘッドレス
// ロードし、ComputeJointGlobal の再評価ビット一致・構成間チェックサム (Debug/Release で同一の
// コード埋め込み期待値)・「部位ワールド = jointGlobal * エンティティ world」規約の成立を検証する。
// ウィンドウも D3D も開かない (ライブラリの Init 前ガードでローダを素通しする)。
bool RunSkeletonSelfTest();

} // namespace mye
