// ハイブリッド・パストレーシング (M46) の共通定義: GPU データレイアウト + BVH トラバーサル。
// C++ 側 src/Engine/Renderer/RayTracing/RtTypes.h (構造体) と RtMath.h (数式) の写し。
// **変更時は C++ 側も必ず同時に更新すること** (selftest が RtMath.h 経由で数式を検証する)。

#ifndef MYE_RT_COMMON_INCLUDED
#define MYE_RT_COMMON_INCLUDED

// C++ の kRtStackDepth / kRtMaxVisit と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_STACK_DEPTH 32
#define MYE_RT_MAX_VISIT 512

// ---- GPU データレイアウト (RtTypes.h と一致) ----

struct RtBvhNode {
    float3 aabbMin;
    int left; // 内部 = 子 index / 葉 = -(start+1)
    float3 aabbMax;
    int right; // 内部 = 子 index / 葉 = 個数
};

struct RtTri {
    float3 p0;
    float pad0;
    float3 e1;
    float pad1;
    float3 e2;
    float pad2;
};

struct RtTriAttr {
    float4 n0u0;
    float4 n1v0;
    float4 n2u1;
    float4 uvRest;
};

struct RtInstance {
    float4 invRow0; // worldToLocal (行ベクトル規約 4x3)
    float4 invRow1;
    float4 invRow2;
    float4 invRow3;
    int blasRoot;
    int materialIndex;
    int pad0;
    int pad1;
};

struct RtMaterial {
    float3 baseColor;
    float metallic;
    float3 emissive;
    float roughness;
};

// シーンバッファ (全 RT シェーダ共通の t0-t5 / b0)
StructuredBuffer<RtBvhNode> gRtNodes : register(t0);     // 全 BLAS を連結したノード配列
StructuredBuffer<RtTri> gRtTris : register(t1);          // 同上 (三角形)
StructuredBuffer<RtTriAttr> gRtAttrs : register(t2);     // 同上 (頂点属性)
StructuredBuffer<RtBvhNode> gRtTlas : register(t3);      // TLAS (root = 0)
StructuredBuffer<RtInstance> gRtInstances : register(t4);
StructuredBuffer<RtMaterial> gRtMaterials : register(t5);

cbuffer RtSceneCB : register(b0)
{
    int gRtInstanceCount; // 0 = シーンが空 → 全レイが miss
    int gRtPad0;
    int gRtPad1;
    int gRtPad2;
};

// ---- 数式 (RtMath.h と一致) ----

// 0 除算を避けた逆数 (成分 0 で (bmin-ro)*inf が NaN になるのを防ぐ)
float RtSafeInv(float d)
{
    const float kEps = 1e-8f;
    if (d > kEps || d < -kEps) {
        return 1.0f / d;
    }
    return (d >= 0.0f) ? (1.0f / kEps) : (-1.0f / kEps);
}

float3 RtSafeInv3(float3 d)
{
    return float3(RtSafeInv(d.x), RtSafeInv(d.y), RtSafeInv(d.z));
}

// AABB スラブテスト ([0, tMax] で交差すれば true)
bool RtSlabTest(float3 bmin, float3 bmax, float3 ro, float3 invD, float tMax)
{
    const float3 t0 = (bmin - ro) * invD;
    const float3 t1 = (bmax - ro) * invD;
    const float3 tsmall = min(t0, t1);
    const float3 tbig = max(t0, t1);
    const float tNear = max(max(tsmall.x, tsmall.y), max(tsmall.z, 0.0f));
    const float tFar = min(min(tbig.x, tbig.y), min(tbig.z, tMax));
    return tNear <= tFar;
}

// Möller-Trumbore (両面)。辺は前計算済み
bool RtRayTri(float3 ro, float3 rd, RtTri tri, out float outT, out float2 outBary)
{
    outT = 0.0f;
    outBary = float2(0.0f, 0.0f);
    const float3 pv = cross(rd, tri.e2);
    const float det = dot(tri.e1, pv);
    if (det > -1e-12f && det < 1e-12f) {
        return false; // レイと三角形が平行
    }
    const float inv = 1.0f / det;
    const float3 tv = ro - tri.p0;
    const float u = dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const float3 qv = cross(tv, tri.e1);
    const float v = dot(rd, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = dot(tri.e2, qv) * inv;
    if (t <= 0.0f) {
        return false;
    }
    outT = t;
    outBary = float2(u, v);
    return true;
}

// ---- トラバーサル ----

struct RtHit {
    float t;
    float2 bary; // (u, v) — 頂点 1 / 2 の重み。頂点 0 は 1-u-v
    int tri;     // 連結三角形配列の絶対 index (-1 = miss)
    int inst;
    int visited; // 訪問ノード数 (ヒートマップ / 打ち切り判定)
};

// インスタンス 1 個の BLAS を走査する。レイはローカルへ移すが方向は正規化しないので
// t はワールド空間のパラメータのまま = インスタンス間で直接比較できる
void RtTraceInstance(int instIdx, float3 ro, float3 rd, inout RtHit hit)
{
    const RtInstance inst = gRtInstances[instIdx];
    const float3 roL = ro.x * inst.invRow0.xyz + ro.y * inst.invRow1.xyz
        + ro.z * inst.invRow2.xyz + inst.invRow3.xyz;
    const float3 rdL =
        rd.x * inst.invRow0.xyz + rd.y * inst.invRow1.xyz + rd.z * inst.invRow2.xyz;
    const float3 invD = RtSafeInv3(rdL);

    int stack[MYE_RT_STACK_DEPTH];
    int top = 0;
    stack[top++] = inst.blasRoot;
    while (top > 0) {
        if (hit.visited >= MYE_RT_MAX_VISIT) {
            break; // TDR 保険
        }
        const RtBvhNode node = gRtNodes[stack[--top]];
        ++hit.visited;
        if (!RtSlabTest(node.aabbMin, node.aabbMax, roL, invD, hit.t)) {
            continue;
        }
        if (node.left < 0) { // 葉: 三角形の連続範囲
            const int start = -node.left - 1;
            for (int i = 0; i < node.right; ++i) {
                const int ti = start + i;
                float t;
                float2 bary;
                if (RtRayTri(roL, rdL, gRtTris[ti], t, bary) && t < hit.t) {
                    hit.t = t;
                    hit.bary = bary;
                    hit.tri = ti;
                    hit.inst = instIdx;
                }
            }
            continue;
        }
        if (top + 2 <= MYE_RT_STACK_DEPTH) {
            stack[top++] = node.right; // 固定順 (right を先に積む = left 先行の DFS)
            stack[top++] = node.left;
        }
    }
}

// TLAS → BLAS の 2 レベル走査で最近ヒットを返す
bool RtTraceClosest(float3 ro, float3 rd, float tMax, out RtHit hit)
{
    hit.t = tMax;
    hit.bary = float2(0.0f, 0.0f);
    hit.tri = -1;
    hit.inst = -1;
    hit.visited = 0;
    if (gRtInstanceCount <= 0) {
        return false;
    }
    const float3 invD = RtSafeInv3(rd);

    int stack[MYE_RT_STACK_DEPTH];
    int top = 0;
    stack[top++] = 0; // TLAS root
    while (top > 0) {
        if (hit.visited >= MYE_RT_MAX_VISIT) {
            break;
        }
        const RtBvhNode node = gRtTlas[stack[--top]];
        ++hit.visited;
        if (!RtSlabTest(node.aabbMin, node.aabbMax, ro, invD, hit.t)) {
            continue;
        }
        if (node.left < 0) { // 葉: インスタンスの連続範囲
            const int start = -node.left - 1;
            for (int i = 0; i < node.right; ++i) {
                RtTraceInstance(start + i, ro, rd, hit);
            }
            continue;
        }
        if (top + 2 <= MYE_RT_STACK_DEPTH) {
            stack[top++] = node.right;
            stack[top++] = node.left;
        }
    }
    return hit.tri >= 0;
}

// ---- ヒット属性 ----

// 補間法線をワールド空間で返す。nWorld = nLocal * transpose(worldToLocal) と等価
float3 RtHitNormal(RtHit hit)
{
    const RtTriAttr a = gRtAttrs[hit.tri];
    const float w = 1.0f - hit.bary.x - hit.bary.y;
    const float3 nL =
        normalize(w * a.n0u0.xyz + hit.bary.x * a.n1v0.xyz + hit.bary.y * a.n2u1.xyz);
    const RtInstance inst = gRtInstances[hit.inst];
    const float3x3 m = float3x3(inst.invRow0.xyz, inst.invRow1.xyz, inst.invRow2.xyz);
    return normalize(mul(m, nL));
}

float2 RtHitUv(RtHit hit)
{
    const RtTriAttr a = gRtAttrs[hit.tri];
    const float w = 1.0f - hit.bary.x - hit.bary.y;
    const float2 uv0 = float2(a.n0u0.w, a.n1v0.w);
    const float2 uv1 = float2(a.n2u1.w, a.uvRest.x);
    const float2 uv2 = float2(a.uvRest.y, a.uvRest.z);
    return w * uv0 + hit.bary.x * uv1 + hit.bary.y * uv2;
}

RtMaterial RtHitMaterial(RtHit hit)
{
    return gRtMaterials[gRtInstances[hit.inst].materialIndex];
}

#endif // MYE_RT_COMMON_INCLUDED
