//====================================================================================
//                          XpbdSolver.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XPBD 距離拘束の射影の実装
//====================================================================================
#include "Engine/Engine/Physics/XpbdSolver.h"

#include <cmath>

namespace mye {
namespace xpbd {

void Predict(XpbdBackend::Pool& pool, float gx, float gy, float gz, float h, bool applyDamping,
             float damping)
{
    const size_t n = pool.px.size();
    for (size_t i = 0; i < n; ++i) {
        if (pool.invMass[i] > 0.0f) {
            pool.vx[i] += gx * h;
            pool.vy[i] += gy * h;
            pool.vz[i] += gz * h;
            // damping > 0 の判定は NaN の堰も兼ねる (NaN > 0 は false — JSON 経由では
            // 何でも入りうるので、コンポーネント由来の値は使う直前で堰き止める)
            if (applyDamping && damping > 0.0f) {
                float damp = 1.0f - damping;
                if (damp < 0.0f) {
                    damp = 0.0f;
                }
                pool.vx[i] *= damp;
                pool.vy[i] *= damp;
                pool.vz[i] *= damp;
            }
        }
        // prev はピン留めも含めて退避する — ピンは v=0 なので x が動かず、
        // Solve の v=(x−prev)/h が自然に 0 を返す (分岐を増やさない)
        pool.prevX[i] = pool.px[i];
        pool.prevY[i] = pool.py[i];
        pool.prevZ[i] = pool.pz[i];
        pool.px[i] += pool.vx[i] * h;
        pool.py[i] += pool.vy[i] * h;
        pool.pz[i] += pool.vz[i] * h;
    }
}

void Solve(XpbdBackend::Pool& pool, float compliance, float h, std::vector<float>& lambdaScratch,
           AttachContext* attach)
{
    const size_t m = pool.ca.size();
    if (m == 0 && attach == nullptr) {
        return;
    }
    // λ はサブステップ内でのみ蓄積する (XPBD の標準形。substep を跨いで持ち越さない)
    lambdaScratch.assign(m, 0.0f);
    // compliance > 0 の判定は負値と NaN の堰 (どちらも α̃ に入れると発散/汚染する)
    const float alphaTilde = compliance > 0.0f ? compliance / (h * h) : 0.0f; // α̃ = α/h²
    for (int iter = 0; iter < kIterations; ++iter) {
        for (size_t c = 0; c < m; ++c) { // 拘束 index 昇順 = 固定順 (決定論)
            const size_t a = pool.ca[c];
            const size_t b = pool.cb[c];
            const float wa = pool.invMass[a];
            const float wb = pool.invMass[b];
            const float wSum = wa + wb;
            if (wSum <= 0.0f) {
                continue; // 両端ピンの拘束は解きようがない (分母 0 の堰)
            }
            const float dx = pool.px[a] - pool.px[b];
            const float dy = pool.py[a] - pool.py[b];
            const float dz = pool.pz[a] - pool.pz[b];
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len < 1e-9f) {
                continue; // 完全重合は法線が定義できない (次の反復で他拘束が離してくれる)
            }
            const float nx = dx / len;
            const float ny = dy / len;
            const float nz = dz / len;
            const float constraint = len - pool.rest[c];
            const float dLambda =
                (-constraint - alphaTilde * lambdaScratch[c]) / (wSum + alphaTilde);
            lambdaScratch[c] += dLambda;
            pool.px[a] += nx * (wa * dLambda);
            pool.py[a] += ny * (wa * dLambda);
            pool.pz[a] += nz * (wa * dLambda);
            pool.px[b] -= nx * (wb * dLambda);
            pool.py[b] -= ny * (wb * dLambda);
            pool.pz[b] -= nz * (wb * dLambda);
        }
        // ---- 終端アタッチ (M60'd)。鎖の全行の後 = 固定順の末尾行 ----
        // 距離 0 拘束なので「一致していれば len<1e-9 で何もしない」が特異点処理を兼ねる
        // (完全重合 = 収束そのもの。距離拘束の重合スキップとは意味が違う)
        if (attach != nullptr) {
            const size_t pi = attach->particle;
            // 蓄積補正ぶんアンカーを追従させる (r 固定の線形化 — ヘッダのコメント参照)
            const float axNow = attach->ax + attach->outDx
                              + (attach->outTy * attach->rz - attach->outTz * attach->ry);
            const float ayNow = attach->ay + attach->outDy
                              + (attach->outTz * attach->rx - attach->outTx * attach->rz);
            const float azNow = attach->az + attach->outDz
                              + (attach->outTx * attach->ry - attach->outTy * attach->rx);
            const float dx = pool.px[pi] - axNow;
            const float dy = pool.py[pi] - ayNow;
            const float dz = pool.pz[pi] - azNow;
            const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len >= 1e-9f) {
                const float nx = dx / len;
                const float ny = dy / len;
                const float nz = dz / len;
                // w_b = invM + (r×n)·I⁻¹(r×n) (FinalizeConstraintBlock の対角項と同式)
                const float cx = attach->ry * nz - attach->rz * ny;
                const float cy = attach->rz * nx - attach->rx * nz;
                const float cz = attach->rx * ny - attach->ry * nx;
                const float ix = attach->invI[0][0] * cx + attach->invI[0][1] * cy
                               + attach->invI[0][2] * cz;
                const float iy = attach->invI[1][0] * cx + attach->invI[1][1] * cy
                               + attach->invI[1][2] * cz;
                const float iz = attach->invI[2][0] * cx + attach->invI[2][1] * cy
                               + attach->invI[2][2] * cz;
                const float wp = pool.invMass[pi];
                const float wb = attach->invMass + cx * ix + cy * iy + cz * iz;
                const float wSum = wp + wb;
                if (wSum > 0.0f) {
                    // rest=0・α̃=0 (剛結合)。λ を持たないのは α̃=0 では蓄積項が消えるため
                    const float dLambda = -len / wSum;
                    pool.px[pi] += nx * (wp * dLambda);
                    pool.py[pi] += ny * (wp * dLambda);
                    pool.pz[pi] += nz * (wp * dLambda);
                    attach->outAbsCorr -= wp * dLambda; // dλ ≤ 0 → 総和は正で単調

                    // 剛体側 (b 側): アンカーへの位置力積 p = -n·dλ を
                    // ΔCOM = invM·p / Δθ = I⁻¹(r×p) に分解して蓄積 (適用は呼び側)
                    attach->outDx -= nx * (attach->invMass * dLambda);
                    attach->outDy -= ny * (attach->invMass * dLambda);
                    attach->outDz -= nz * (attach->invMass * dLambda);
                    attach->outTx -= ix * dLambda;
                    attach->outTy -= iy * dLambda;
                    attach->outTz -= iz * dLambda;
                }
            }
        }
    }
    // 位置差から速度を確定する (XPBD の速度更新)。ピンは x==prev なので自然に 0
    const float invH = 1.0f / h;
    const size_t n = pool.px.size();
    for (size_t i = 0; i < n; ++i) {
        pool.vx[i] = (pool.px[i] - pool.prevX[i]) * invH;
        pool.vy[i] = (pool.py[i] - pool.prevY[i]) * invH;
        pool.vz[i] = (pool.pz[i] - pool.prevZ[i]) * invH;
    }
}

} // namespace xpbd
} // namespace mye
