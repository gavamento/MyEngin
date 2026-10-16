//====================================================================================
//                          XpbdSolver.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XPBD 距離拘束の射影の実装
//====================================================================================
#include "Engine/Engine/Physics/Xpbd/XpbdSolver.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Ecs/Components.h" // collidershape::

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

void CollectContacts(const XpbdBackend::Pool& pool, float radius,
                     const std::vector<WorldShape>& shapes, EntityID exclude0, EntityID exclude1,
                     std::vector<ParticleContact>& out)
{
    out.clear();
    const size_t n = pool.px.size();
    if (n == 0 || shapes.empty()) {
        return;
    }
    // radius > 0 の判定は負値と NaN の堰 (コンポーネント由来の値は使う直前で堰き止める)
    const float r = radius > 0.0f ? radius : 0.0f;
    const float reach = r + kContactMargin;
    // 池全体の AABB で形状を先に間引く (ロープ 1 本に対して世界の大半は遠い)
    float pminX = pool.px[0], pminY = pool.py[0], pminZ = pool.pz[0];
    float pmaxX = pminX, pmaxY = pminY, pmaxZ = pminZ;
    for (size_t i = 1; i < n; ++i) {
        pminX = std::min(pminX, pool.px[i]);
        pminY = std::min(pminY, pool.py[i]);
        pminZ = std::min(pminZ, pool.pz[i]);
        pmaxX = std::max(pmaxX, pool.px[i]);
        pmaxY = std::max(pmaxY, pool.py[i]);
        pmaxZ = std::max(pmaxZ, pool.pz[i]);
    }
    std::vector<uint32_t> near; // 池の AABB に掛かる形状の index (昇順のまま)
    for (size_t s = 0; s < shapes.size(); ++s) {
        const WorldShape& ws = shapes[s];
        if (ws.entity == exclude0 || ws.entity == exclude1) {
            continue;
        }
        if (ws.maxX < pminX - reach || ws.minX > pmaxX + reach || ws.maxY < pminY - reach
            || ws.minY > pmaxY + reach || ws.maxZ < pminZ - reach || ws.minZ > pmaxZ + reach) {
            continue;
        }
        near.push_back(static_cast<uint32_t>(s));
    }
    if (near.empty()) {
        return;
    }
    ShapePose sphere;
    sphere.shape = collidershape::kSphere;
    sphere.identityRot = 1;
    sphere.radius = reach; // 先読みぶん膨らませて測り、planeD で差し引く
    for (size_t i = 0; i < n; ++i) {
        if (pool.invMass[i] <= 0.0f) {
            continue; // ピンは動かせない (押し出しようがない)
        }
        const float x = pool.px[i], y = pool.py[i], z = pool.pz[i];
        for (const uint32_t s : near) {
            const WorldShape& ws = shapes[s];
            if (ws.maxX < x - reach || ws.minX > x + reach || ws.maxY < y - reach
                || ws.minY > y + reach || ws.maxZ < z - reach || ws.minZ > z + reach) {
                continue;
            }
            sphere.px = x;
            sphere.py = y;
            sphere.pz = z;
            float nx, ny, nz, depth;
            // normal は b→a = 形状 → 粒子 (押し出す向き)
            if (!shapes::Collide(sphere, ws.pose, nx, ny, nz, depth)) {
                continue;
            }
            ParticleContact c;
            c.particle = static_cast<uint32_t>(i);
            c.nx = nx;
            c.ny = ny;
            c.nz = nz;
            // 膨らませた球の貫通 depth から先読みぶんを引くと、真の貫通 = r − 表面距離。
            // C(x) = n·x − planeD は予測位置で −(depth − margin) になる
            c.planeD = nx * x + ny * y + nz * z + (depth - kContactMargin);
            c.muS = ws.muS;
            c.muK = ws.muK;
            out.push_back(c);
        }
    }
}

void Solve(XpbdBackend::Pool& pool, float compliance, float h, std::vector<float>& lambdaScratch,
           AttachContext* attach, std::vector<ParticleContact>* contacts)
{
    const size_t m = pool.ca.size();
    const bool hasContacts = contacts != nullptr && !contacts->empty();
    if (m == 0 && attach == nullptr && !hasContacts) {
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
        // ---- 世界との接触 (M60'e)。固定順の最後 = 反復の出口で貫通が残らない側を優先 ----
        // 相手は不動 (重み 0) なので Δλ = −C/w、粒子の移動は −C·n (w に依らない)
        if (hasContacts) {
            for (ParticleContact& c : *contacts) {
                const size_t pi = c.particle;
                const float cval =
                    c.nx * pool.px[pi] + c.ny * pool.py[pi] + c.nz * pool.pz[pi] - c.planeD;
                if (cval >= 0.0f) {
                    continue; // 不等式拘束: 離れていれば何もしない
                }
                pool.px[pi] -= c.nx * cval;
                pool.py[pi] -= c.ny * cval;
                pool.pz[pi] -= c.nz * cval;
                c.pushed -= cval;
            }
        }
    }
    // ---- 摩擦 (M60'e)。反復の後・速度確定の前 ----
    // このサブステップの接線変位 Δx_t を、静止なら全部 / 動摩擦なら μk·押し量ぶん打ち消す
    // (Macklin 2014 式 24-26。相手は不動なので相対変位 = 粒子の変位)。押していない接触
    // (pushed == 0) は何もしない = 触れていない平面が粒子を引き止めない
    if (hasContacts) {
        for (const ParticleContact& c : *contacts) {
            if (c.pushed <= 0.0f) {
                continue;
            }
            const size_t pi = c.particle;
            const float dx = pool.px[pi] - pool.prevX[pi];
            const float dy = pool.py[pi] - pool.prevY[pi];
            const float dz = pool.pz[pi] - pool.prevZ[pi];
            const float dn = dx * c.nx + dy * c.ny + dz * c.nz;
            const float tx = dx - c.nx * dn;
            const float ty = dy - c.ny * dn;
            const float tz = dz - c.nz * dn;
            const float tLen = std::sqrt(tx * tx + ty * ty + tz * tz);
            if (tLen <= 0.0f) {
                continue;
            }
            float k = 1.0f; // 打ち消す割合
            if (tLen >= c.muS * c.pushed) {
                k = std::min(c.muK * c.pushed / tLen, 1.0f);
            }
            pool.px[pi] -= tx * k;
            pool.py[pi] -= ty * k;
            pool.pz[pi] -= tz * k;
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
