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

void Solve(XpbdBackend::Pool& pool, float compliance, float h, std::vector<float>& lambdaScratch)
{
    const size_t m = pool.ca.size();
    if (m == 0) {
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
