#pragma once

namespace mye {

// 剛体物理 + Raycast のヘッドレス回帰テスト (M20)。D3D/ウィンドウ不要。
// 落下 / 接地 / トリガー透過 / 決定論 (同一シーン 2 回で per-tick ハッシュ一致) / Raycast を検証。
// 全 PASS で true。--selftest から呼ばれる。
bool RunPhysicsSelfTest();

} // namespace mye
