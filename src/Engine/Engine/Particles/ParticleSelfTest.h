#pragma once

namespace mye {

// パーティクル (CPU バックエンド + 放出計画 + グラデーション) のヘッドレス回帰テスト (M32a)。
// - PlanParticleEmission: burst/duration/loop/playing の放出数を検証
// - EvalParticleColor/SizeScale: 中間キー無しは 2 点線形と一致、中間キーで折れ線補間
// - SIMD/スカラー Simulate のビット一致 (決定論)
// - 同一エミッタ 2 個の per-tick sim ハッシュ一致 (決定論)
bool RunParticleSelfTest();

} // namespace mye
