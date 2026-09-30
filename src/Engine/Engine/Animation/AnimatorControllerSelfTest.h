#pragma once

namespace mye {

// Animator Controller のヘッドレス回帰テスト (M22)。D3D/ウィンドウ不要。
// ステート遷移 (param 駆動 Idle↔Walk) / ブレンド / **決定論 (同一シーン2個で per-tick ハッシュ一致)** を検証。
bool RunAnimatorControllerSelfTest();

} // namespace mye
