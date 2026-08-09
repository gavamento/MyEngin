#pragma once

namespace mye {

// ゲームフローの回帰テスト (M51g)。
// TimeControl の tick ゲート位相 / PersistStore と WorldHash の被覆・挿入順不変 /
// SaveGameFile の往復と破損耐性 / PlayModeController の Play/Stop スナップショット
// (永続値の漏れ) をまとめて検証する。
// **Editor 層に置いてある**のは PlayModeController が Editor 層だから (PartSelfTest と同じ理由)
bool RunGameFlowSelfTest();

} // namespace mye
