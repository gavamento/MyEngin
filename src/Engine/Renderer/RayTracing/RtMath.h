#pragma once
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Renderer/RayTracing/RtTypes.h"

// レイトレーシングの数式を C++ に複製した純関数群 (D3D 非依存)。
// HLSL 側 assets/shaders/rt_common.hlsli とコメント同期で複製し、selftest がこちらを検証する。
// 描画専用 (sim / WorldHash 非関与)。**変更時は HLSL と両方更新すること**。
namespace mye {

// 0 除算を避けた逆数。rd の成分が 0 のとき (bmin-ro)*inf が NaN になるのを防ぐ。
// HLSL の RtSafeInv と同一式
inline float RtSafeInv(float d)
{
    constexpr float kEps = 1e-8f;
    if (d > kEps || d < -kEps) {
        return 1.0f / d;
    }
    return (d >= 0.0f) ? (1.0f / kEps) : (-1.0f / kEps);
}

// AABB スラブテスト。[0, tMax] の範囲で交差があれば true。
// HLSL の RtSlabTest と同一式
inline bool RtSlabTest(const DirectX::XMFLOAT3& bmin, const DirectX::XMFLOAT3& bmax,
                       const DirectX::XMFLOAT3& ro, const DirectX::XMFLOAT3& invD, float tMax)
{
    const float t0x = (bmin.x - ro.x) * invD.x, t1x = (bmax.x - ro.x) * invD.x;
    const float t0y = (bmin.y - ro.y) * invD.y, t1y = (bmax.y - ro.y) * invD.y;
    const float t0z = (bmin.z - ro.z) * invD.z, t1z = (bmax.z - ro.z) * invD.z;
    const float nx = (t0x < t1x) ? t0x : t1x, fx = (t0x < t1x) ? t1x : t0x;
    const float ny = (t0y < t1y) ? t0y : t1y, fy = (t0y < t1y) ? t1y : t0y;
    const float nz = (t0z < t1z) ? t0z : t1z, fz = (t0z < t1z) ? t1z : t0z;
    float tNear = (nx > ny) ? nx : ny;
    tNear = (tNear > nz) ? tNear : nz;
    tNear = (tNear > 0.0f) ? tNear : 0.0f;
    float tFar = (fx < fy) ? fx : fy;
    tFar = (tFar < fz) ? tFar : fz;
    tFar = (tFar < tMax) ? tFar : tMax;
    return tNear <= tFar;
}

// Möller-Trumbore (両面)。Physics の shapes::RayTri と同じ式を辺の前計算済み形で。
// HLSL の RtRayTri と同一式
inline bool RtRayTri(const DirectX::XMFLOAT3& ro, const DirectX::XMFLOAT3& rd, const RtTri& tri,
                     float& outT, float& outU, float& outV)
{
    using namespace DirectX;
    const XMVECTOR o = XMLoadFloat3(&ro), d = XMLoadFloat3(&rd);
    const XMVECTOR p0 = XMLoadFloat3(&tri.p0);
    const XMVECTOR e1 = XMLoadFloat3(&tri.e1), e2 = XMLoadFloat3(&tri.e2);
    const XMVECTOR pv = XMVector3Cross(d, e2);
    const float det = XMVectorGetX(XMVector3Dot(e1, pv));
    if (det > -1e-12f && det < 1e-12f) {
        return false; // レイと三角形が平行
    }
    const float inv = 1.0f / det;
    const XMVECTOR tv = XMVectorSubtract(o, p0);
    const float u = XMVectorGetX(XMVector3Dot(tv, pv)) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const XMVECTOR qv = XMVector3Cross(tv, e1);
    const float v = XMVectorGetX(XMVector3Dot(d, qv)) * inv;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = XMVectorGetX(XMVector3Dot(e2, qv)) * inv;
    if (t <= 0.0f) {
        return false;
    }
    outT = t;
    outU = u;
    outV = v;
    return true;
}

// ---- サンプリング (HLSL の同名関数と一致。変更時は両方更新) ----

// PCG3D ハッシュ (状態レス)。同じ入力からは常に同じ値 = スクリーンショットの決定性が保てる
struct RtSeed {
    uint32_t x = 0, y = 0, z = 0;
};

inline RtSeed RtPcg3d(RtSeed v)
{
    v.x = v.x * 1664525u + 1013904223u;
    v.y = v.y * 1664525u + 1013904223u;
    v.z = v.z * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x ^= v.x >> 16u;
    v.y ^= v.y >> 16u;
    v.z ^= v.z >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// 呼ぶたびに seed.z を進める (HLSL の RtNextRand2 と同一)
inline DirectX::XMFLOAT2 RtNextRand2(RtSeed& seed)
{
    seed.z += 1u;
    const RtSeed h = RtPcg3d(seed);
    constexpr float kInv2p32 = 2.3283064365386963e-10f; // 1 / 2^32
    return { static_cast<float>(h.x) * kInv2p32, static_cast<float>(h.y) * kInv2p32 };
}

// コサイン重点サンプリング (法線半球、pdf = cos/PI)。
// 基底は Duff らの分岐なし ONB。HLSL の RtCosineHemisphere と同一式
inline DirectX::XMFLOAT3 RtCosineHemisphere(const DirectX::XMFLOAT3& n,
                                            const DirectX::XMFLOAT2& u)
{
    using namespace DirectX;
    const float r = std::sqrt(u.x);
    const float phi = 6.28318530718f * u.y;
    const float sgn = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + n.z);
    const float b = n.x * n.y * a;
    const XMFLOAT3 t1 = { 1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x };
    const XMFLOAT3 t2 = { b, sgn + n.y * n.y * a, -n.y };
    const float c1 = r * std::cos(phi);
    const float c2 = r * std::sin(phi);
    const float c3 = std::sqrt((std::max)(0.0f, 1.0f - u.x));
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(XMVectorSet(
                            t1.x * c1 + t2.x * c2 + n.x * c3, t1.y * c1 + t2.y * c2 + n.y * c3,
                            t1.z * c1 + t2.z * c2 + n.z * c3, 0.0f)));
    return out;
}

// ---- M46d: テンポラル蓄積 (HLSL の rt_temporal.cs.hlsl と一致。変更時は両方更新) ----

// 前フレームのクリップ座標 → 履歴バッファの UV。
// 背後 (w<=0) と画面外は false = 履歴なし。HLSL の RtClipToPrevUv と同一式
inline bool RtClipToPrevUv(const DirectX::XMFLOAT4& clip, DirectX::XMFLOAT2& outUv)
{
    outUv = { 0.0f, 0.0f };
    if (clip.w <= 1e-6f) {
        return false; // 前フレームのカメラの背後 (または退化)
    }
    const float nx = clip.x / clip.w;
    const float ny = clip.y / clip.w;
    outUv = { nx * 0.5f + 0.5f, ny * -0.5f + 0.5f };
    return outUv.x >= 0.0f && outUv.x < 1.0f && outUv.y >= 0.0f && outUv.y < 1.0f;
}

// 再投影先の履歴が現在の面と同じものか。
// expectedDepth = 現在の可視点を前フレームのカメラから測った距離、
// storedDepth   = 履歴バッファがそのピクセルに記録している距離 (0 以下 = 未記録)。
// HLSL の RtReprojectValid と同一式
inline bool RtReprojectValid(float expectedDepth, float storedDepth,
                             const DirectX::XMFLOAT3& n, const DirectX::XMFLOAT3& prevN,
                             float depthThreshold, float normalThreshold)
{
    if (!(storedDepth > 0.0f) || !(expectedDepth > 0.0f)) {
        return false;
    }
    const float d = std::fabs(expectedDepth - storedDepth);
    if (d > depthThreshold * (std::max)(expectedDepth, 1e-3f)) {
        return false; // 別の面が手前/奥にある (disocclusion)
    }
    const float c = n.x * prevN.x + n.y * prevN.y + n.z * prevN.z;
    return c >= normalThreshold;
}

// 履歴長を 1 進める。無効なら 1 (= 今フレームの 1spp をそのまま採用) に若返る。
// HLSL の RtAdvanceHistory と同一式
inline float RtAdvanceHistory(float prevLen, bool valid, float maxLen)
{
    const float base = valid ? prevLen : 0.0f;
    return (std::min)(base + 1.0f, maxLen);
}

// 移動平均の重み (新サンプルの寄与)。履歴長 1 で 1.0 = 履歴を使わない。
// HLSL の RtTemporalAlpha と同一式
inline float RtTemporalAlpha(float histLen)
{
    return 1.0f / (std::max)(histLen, 1.0f);
}

// ---- M46e: SVGF (HLSL の rt_variance.cs.hlsl / rt_atrous.cs.hlsl と一致。両方更新) ----

// 輝度 (Rec.709)。ポスプロ (postfx_tonemap.hlsl 等) と同じ係数
inline float RtLuminance(const DirectX::XMFLOAT3& c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// 1 サンプルあたりの分散 (μ² から μ の 2 乗を引く)。丸めで負に落ちるので 0 で止める。
// HLSL の RtVarianceFromMoments と同一式
inline float RtVarianceFromMoments(float m1, float m2)
{
    const float v = m2 - m1 * m1;
    return (v > 0.0f) ? v : 0.0f;
}

// サンプル分散 → 蓄積後の推定値の分散。N 個の平均の分散は 1/N になるので履歴長で割る。
// SVGF 原論文はサンプル分散をそのまま使うが、それだと収束後もぼけ続けて GI の
// コンタクト陰影が溶ける。forceSpatial (シード凍結 = 毎フレーム同じ 1 サンプル) は
// いくら履歴が伸びても実効サンプル数が 1 なので割らない。
// HLSL の RtVarianceEstimate と同一式
inline float RtVarianceEstimate(float sampleVar, float histLen, bool forceSpatial)
{
    if (forceSpatial) {
        return sampleVar;
    }
    return sampleVar / ((histLen > 1.0f) ? histLen : 1.0f);
}

// 深度 (カメラ距離) のエッジ停止重み。真の深度勾配を持たないので
// 「タップが遠いほど許容を広げる」相対差で近似する (平面上で重みが落ちないように)。
// HLSL の RtAtrousDepthWeight と同一式
inline float RtAtrousDepthWeight(float zc, float zq, float tapDist, float sigma)
{
    const float tol = sigma * ((zc > 1e-3f) ? zc : 1e-3f) * ((tapDist > 1.0f) ? tapDist : 1.0f);
    return std::exp(-std::fabs(zc - zq) / ((tol > 1e-6f) ? tol : 1e-6f));
}

// 法線のエッジ停止重み (cos の冪)。裏向きは 0。HLSL の RtAtrousNormalWeight と同一式
inline float RtAtrousNormalWeight(const DirectX::XMFLOAT3& nc, const DirectX::XMFLOAT3& nq,
                                  float power)
{
    const float c = nc.x * nq.x + nc.y * nq.y + nc.z * nq.z;
    return std::pow((c > 0.0f) ? c : 0.0f, power);
}

// 輝度のエッジ停止重み。推定標準偏差でスケールするので、ノイズが乗っている間は
// 緩く (よくぼける)、収束すると厳しく (エッジが残る)。HLSL の RtAtrousLumaWeight と同一式
inline float RtAtrousLumaWeight(float lc, float lq, float variance, float sigma)
{
    const float sd = std::sqrt((variance > 0.0f) ? variance : 0.0f);
    return std::exp(-std::fabs(lc - lq) / (sigma * sd + 1e-4f));
}

// A-Trous の 1 次元カーネル (B3 スプライン (1,4,6,4,1)/16)。d は中心からのタップ番号。
// HLSL の RtAtrousKernel と同一値
inline float RtAtrousKernel(int d)
{
    const int i = (d < 0) ? -d : d;
    switch (i) {
    case 0:
        return 6.0f / 16.0f;
    case 1:
        return 4.0f / 16.0f;
    case 2:
        return 1.0f / 16.0f;
    default:
        return 0.0f;
    }
}

// BLAS (単一メッシュ) のヒット結果。tri は連結三角形配列の絶対 index
struct RtBlasHit {
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    int32_t tri = -1;
    int32_t visited = 0; // 訪問ノード数 (ヒートマップ / 打ち切り判定)
};

// 1 つの BLAS をスタックで走査して最近ヒットを返す (ローカル空間)。
// nodes/tris は連結配列、root はその中の BLAS ルート index。
// HLSL の RtTraceBlas と同一ロジック — 走査順・打ち切り条件を変えないこと
inline bool RtTraceBlasCpu(const std::vector<RtBvhNode>& nodes, const std::vector<RtTri>& tris,
                           int32_t root, const DirectX::XMFLOAT3& ro,
                           const DirectX::XMFLOAT3& rd, RtBlasHit& hit)
{
    if (nodes.empty() || root < 0 || root >= static_cast<int32_t>(nodes.size())) {
        return false;
    }
    const DirectX::XMFLOAT3 invD = { RtSafeInv(rd.x), RtSafeInv(rd.y), RtSafeInv(rd.z) };
    int32_t stack[kRtStackDepth];
    int top = 0;
    stack[top++] = root;
    bool found = false;
    while (top > 0) {
        if (hit.visited >= kRtMaxVisit) {
            break; // TDR 保険 (HLSL と同条件)
        }
        const RtBvhNode& node = nodes[static_cast<size_t>(stack[--top])];
        ++hit.visited;
        if (!RtSlabTest(node.aabbMin, node.aabbMax, ro, invD, hit.t)) {
            continue;
        }
        if (node.left < 0) { // 葉: 三角形の連続範囲
            const int32_t start = -node.left - 1;
            for (int32_t i = 0; i < node.right; ++i) {
                const int32_t ti = start + i;
                float t = 0.0f, u = 0.0f, v = 0.0f;
                if (RtRayTri(ro, rd, tris[static_cast<size_t>(ti)], t, u, v) && t < hit.t) {
                    hit.t = t;
                    hit.u = u;
                    hit.v = v;
                    hit.tri = ti;
                    found = true;
                }
            }
            continue;
        }
        if (top + 2 <= kRtStackDepth) {
            stack[top++] = node.right; // 固定順 (right を先に積む = left 先行の DFS)
            stack[top++] = node.left;
        }
    }
    return found;
}

} // namespace mye
