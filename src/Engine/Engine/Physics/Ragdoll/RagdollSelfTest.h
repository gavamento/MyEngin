#pragma once

namespace mye {

// ラグドール (M60g1) の自己テスト。D3D もウィンドウも作らない。
//
// **スケルトンは実アセットではなく手で組む** — 3 骨の直鎖を試験内で作って
// `SkinnedModelLibrary` に登録する。実アセット (CesiumMan.glb 等) に依存すると
// 「そのモデルの骨名と bind ポーズ」に試験が縛られるし、検証したいのは
// 「剛体 → 骨の逆駆動」であってローダではない (ローダ側は PartSelfTest の担当)。
bool RunRagdollSelfTest();

} // namespace mye
