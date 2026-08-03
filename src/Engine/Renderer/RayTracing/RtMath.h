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
