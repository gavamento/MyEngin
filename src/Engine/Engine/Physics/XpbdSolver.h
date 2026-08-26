//====================================================================================
//                          XpbdSolver.h
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XPBD 距離拘束の射影 (純関数群)
//====================================================================================
#pragma once
#include <vector>

#include "Engine/Engine/Physics/XpbdBackend.h"

namespace mye {

// XPBD の 1 池ぶんの段 (M60'c)。PhysicsSystem のサブステップループが呼ぶ:
//   剛体の速度積分の後   → Predict (重力 → 減衰 → 位置予測)
//   剛体ソルバ/位置補正の後 → Solve  (拘束射影 固定反復 → v = (x − prev)/h)
// 全て scalar float・固定反復・固定順 (決定論契約。早期終了しない)。
// compliance / damping はコンポーネントから毎 tick 読む導出値なので引数で受ける
// (池に置くと snapshot 状態が増えるだけで正本が 2 つになる)。
namespace xpbd {

// 拘束射影の固定反復回数。剛体の kSolverIterations と同じ「収束判定で早期終了しない」規約
inline constexpr int kIterations = 8;

// 重力 → (最初のサブステップのみ) 減衰 → prev 退避 → 位置予測。
// damping は**毎 tick の率** (Rigidbody.linearDamping と同じ) なので applyDamping は
// sub == 0 だけ true にする — サブステップごとに掛けると N 乗になる (剛体と同じ罠)
void Predict(XpbdBackend::Pool& pool, float gx, float gy, float gz, float h, bool applyDamping,
             float damping);

// 距離拘束を λ 蓄積つきで固定反復射影し、v = (x − prev)/h で速度を確定する。
// α̃ = compliance/h² (XPBD)。lambdaScratch は呼び出し側が使い回すスクラッチ
// (サブステップ冒頭で 0 リセットされる。sim 状態ではない)
void Solve(XpbdBackend::Pool& pool, float compliance, float h, std::vector<float>& lambdaScratch);

} // namespace xpbd

} // namespace mye
