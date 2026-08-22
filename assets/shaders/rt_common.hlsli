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

// GPU ライト 1 個 (C++ の GpuLight / common.hlsli の Light と同じ 64 バイト)
struct RtLight {
    float3 position;
    float range;
    float3 direction; // 光の進行方向 (正規化)
    float intensity;
    float3 color;
    int type; // 0=Directional 1=Point 2=Spot
    float cosInner;
    float cosOuter;
    float2 _pad;
};

// C++ の kMaxLights (RenderTypes.h) / common.hlsli の MAX_LIGHTS と同値。
// tools\check_rules.ps1 の規則 9 が 3 者の一致を静的に検査する (M55a で登録)
#define MYE_RT_MAX_LIGHTS 16

// シーンバッファ (全 RT シェーダ共通の t0-t6 / b0-b1 / s0)
StructuredBuffer<RtBvhNode> gRtNodes : register(t0);     // 全 BLAS を連結したノード配列
StructuredBuffer<RtTri> gRtTris : register(t1);          // 同上 (三角形)
StructuredBuffer<RtTriAttr> gRtAttrs : register(t2);     // 同上 (頂点属性)
StructuredBuffer<RtBvhNode> gRtTlas : register(t3);      // TLAS (root = 0)
StructuredBuffer<RtInstance> gRtInstances : register(t4);
StructuredBuffer<RtMaterial> gRtMaterials : register(t5);
TextureCube gRtSkyCube : register(t6); // skyMode==1 のときだけ有効
SamplerState gRtSampler : register(s0); // LINEAR / CLAMP

cbuffer RtSceneCB : register(b0)
{
    int gRtInstanceCount; // 0 = シーンが空 → 全レイが miss
    int gRtPad0;
    int gRtPad1;
    int gRtPad2;
};

// 環境 (ライト + スカイ)。ヒット点のシェーディングとレイのミス色に使う
cbuffer RtEnvCB : register(b1)
{
    float3 gRtAmbient;
    int gRtLightCount;
    float3 gRtSkyTop; // リニア (RenderSystem が変換済みで渡す)
    int gRtSkyMode;   // -1=スカイ無し 0=グラデーション 1=キューブマップ
    float3 gRtSkyHorizon;
    float gRtRayEps; // 自己交差回避のオフセット
    float3 gRtSkyBottom;
    float gRtEnvPad1;
    RtLight gRtLights[MYE_RT_MAX_LIGHTS];
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

// 影レイ: [0, tMax) に遮蔽物があれば true。最近ヒットを求めないので最初の交差で抜ける
bool RtTraceAnyHit(float3 ro, float3 rd, float tMax)
{
    if (gRtInstanceCount <= 0) {
        return false;
    }
    const float3 invD = RtSafeInv3(rd);
    int visited = 0;
    int stack[MYE_RT_STACK_DEPTH];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
        if (visited >= MYE_RT_MAX_VISIT) {
            break;
        }
        const RtBvhNode node = gRtTlas[stack[--top]];
        ++visited;
        if (!RtSlabTest(node.aabbMin, node.aabbMax, ro, invD, tMax)) {
            continue;
        }
        if (node.left < 0) {
            const int start = -node.left - 1;
            for (int i = 0; i < node.right; ++i) {
                const RtInstance inst = gRtInstances[start + i];
                const float3 roL = ro.x * inst.invRow0.xyz + ro.y * inst.invRow1.xyz
                    + ro.z * inst.invRow2.xyz + inst.invRow3.xyz;
                const float3 rdL = rd.x * inst.invRow0.xyz + rd.y * inst.invRow1.xyz
                    + rd.z * inst.invRow2.xyz;
                const float3 invDL = RtSafeInv3(rdL);
                int bstack[MYE_RT_STACK_DEPTH];
                int btop = 0;
                bstack[btop++] = inst.blasRoot;
                while (btop > 0) {
                    if (visited >= MYE_RT_MAX_VISIT) {
                        break;
                    }
                    const RtBvhNode bn = gRtNodes[bstack[--btop]];
                    ++visited;
                    if (!RtSlabTest(bn.aabbMin, bn.aabbMax, roL, invDL, tMax)) {
                        continue;
                    }
                    if (bn.left < 0) {
                        const int bstart = -bn.left - 1;
                        for (int k = 0; k < bn.right; ++k) {
                            float t;
                            float2 bary;
                            if (RtRayTri(roL, rdL, gRtTris[bstart + k], t, bary) && t < tMax) {
                                return true; // 遮蔽あり
                            }
                        }
                        continue;
                    }
                    if (btop + 2 <= MYE_RT_STACK_DEPTH) {
                        bstack[btop++] = bn.right;
                        bstack[btop++] = bn.left;
                    }
                }
            }
            continue;
        }
        if (top + 2 <= MYE_RT_STACK_DEPTH) {
            stack[top++] = node.right;
            stack[top++] = node.left;
        }
    }
    return false;
}

// ---- サンプリング (RtMath.h と一致) ----

// PCG3D ハッシュ (状態レス)。同じ入力からは常に同じ乱数列 = スクショの決定性が保てる
uint3 RtPcg3d(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// seed は (ピクセル, フレーム由来) で初期化する。呼ぶたびに z を進める
float2 RtNextRand2(inout uint3 seed)
{
    seed.z += 1u;
    const uint3 h = RtPcg3d(seed);
    return float2(h.x, h.y) * 2.3283064365386963e-10f; // 1 / 2^32
}

// コサイン重点サンプリング (法線半球)。pdf = cos/PI。
// 基底は Duff らの分岐なし ONB (z≈-1 でも安定)
float3 RtCosineHemisphere(float3 n, float2 u)
{
    const float r = sqrt(u.x);
    const float phi = 6.28318530718f * u.y;
    const float sgn = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + n.z);
    const float b = n.x * n.y * a;
    const float3 t1 = float3(1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x);
    const float3 t2 = float3(b, sgn + n.y * n.y * a, -n.y);
    return normalize(t1 * (r * cos(phi)) + t2 * (r * sin(phi))
                     + n * sqrt(max(0.0f, 1.0f - u.x)));
}

// 円錐 (半頂角 acos(cosMax)) の内側を立体角に対して一様にサンプルする (M46g)。
// cosMax = 1 で dir そのもの (点光源 = 完全に硬い影)。基底は RtCosineHemisphere と同じ
// Duff らの分岐なし ONB。**RtMath.h の RtSampleCone と同一式 (変更時は両方更新)**
float3 RtSampleCone(float3 dir, float cosMax, float2 u)
{
    const float cosT = cosMax + u.x * (1.0f - cosMax);
    const float sinT = sqrt(max(0.0f, 1.0f - cosT * cosT));
    const float phi = 6.28318530718f * u.y;
    const float sgn = (dir.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + dir.z);
    const float b = dir.x * dir.y * a;
    const float3 t1 = float3(1.0f + sgn * dir.x * dir.x * a, sgn * b, -sgn * dir.x);
    const float3 t2 = float3(b, sgn + dir.y * dir.y * a, -dir.y);
    return normalize(t1 * (sinT * cos(phi)) + t2 * (sinT * sin(phi)) + dir * cosT);
}

// GGX の可視法線分布 (VNDF) サンプリング (Heitz 2018, JCGT)。
// ワールド空間の法線 n と視線方向 v (面 → カメラ) から half vector を返す。
// alpha = roughness² (common.hlsli::DistributionGGX と同じ規約)。
// alpha = 0 で n そのものへ退化する = 完全鏡面。
// 接空間の基底は RtCosineHemisphere / RtSampleCone と同じ Duff らの分岐なし ONB。
// **RtMath.h の RtGgxVndf と同一式 (変更時は両方更新)**
float3 RtGgxVndf(float3 n, float3 v, float alpha, float2 u)
{
    // n を z 軸とする正規直交基底
    const float sgn = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + n.z);
    const float b = n.x * n.y * a;
    const float3 t1 = float3(1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x);
    const float3 t2 = float3(b, sgn + n.y * n.y * a, -n.y);

    // 視線を接空間へ → 楕円体を半球へ変形 (§3.2)
    const float3 ve = float3(dot(v, t1), dot(v, t2), dot(v, n));
    const float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
    // 投影面積の正規直交基底 (vh が z 軸に一致するときの特異点を分岐で回避、§4.1)
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 h1 = (lensq > 1e-12f) ? (float3(-vh.y, vh.x, 0.0f) * rsqrt(lensq))
                                       : float3(1.0f, 0.0f, 0.0f);
    const float3 h2 = cross(vh, h1);
    // 単位円板の一様サンプルを、可視半球の投影 (楕円) へ切り詰める (§4.2)
    const float r = sqrt(u.x);
    const float phi = 6.28318530718f * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5f * (1.0f + vh.z);
    p2 = (1.0f - s) * sqrt(max(0.0f, 1.0f - p1 * p1)) + s * p2;
    // 半球へ持ち上げ (§4.3) → 楕円体へ戻す (§3.4)
    const float3 nh =
        p1 * h1 + p2 * h2 + sqrt(max(0.0f, 1.0f - p1 * p1 - p2 * p2)) * vh;
    const float3 ne = normalize(float3(alpha * nh.x, alpha * nh.y, max(1e-6f, nh.z)));
    return normalize(ne.x * t1 + ne.y * t2 + ne.z * n);
}

// ---- シェーディング ----

// レイが何にも当たらなかったときの放射輝度。lod はキューブマップスカイの mip
// (拡散 GI は粗い mip でノイズを減らし、鏡面反射は lod 0 で像を保つ)
float3 RtSkyRadianceLod(float3 dir, float lod)
{
    // 単一の戻り値にまとめる (早期 return を混ぜると X4000 の誤検出が出る)
    float3 c = gRtAmbient; // スカイ無し = 従来の定数アンビエント
    if (gRtSkyMode == 1) {
        c = gRtSkyCube.SampleLevel(gRtSampler, dir, lod).rgb;
    } else if (gRtSkyMode == 0) {
        const float t = dir.y; // skybox.hlsl と同一式
        c = (t >= 0.0f) ? lerp(gRtSkyHorizon, gRtSkyTop, saturate(t * 1.4f))
                        : lerp(gRtSkyHorizon, gRtSkyBottom, saturate(-t * 1.4f));
    }
    return c;
}

// 二次光線 (拡散 GI) 既定: 粗い mip で十分 (ノイズも減る)
float3 RtSkyRadiance(float3 dir)
{
    return RtSkyRadianceLod(dir, 2.0f);
}

// ヒット点の直接光 (拡散のみ)。common.hlsli::ApplyLighting の減衰規約をそのまま複製し、
// 拡散の 1/PI 省略も踏襲する (ラスタと明るさの次元を揃えるため)。
// 影レイは太陽のみ — ローカルライトが影を落とさないのはラスタ側と同じ
float3 RtDirectLight(float3 P, float3 N, float3 albedo, float metallic)
{
    float3 Lo = float3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < gRtLightCount; ++i) {
        const RtLight L = gRtLights[i];
        float3 toL;
        float atten = 1.0f;
        if (L.type == 0) { // Directional
            toL = -L.direction;
            if (RtTraceAnyHit(P + N * gRtRayEps, toL, 1e16f)) {
                atten = 0.0f;
            }
        } else { // Point / Spot
            const float3 d = L.position - P;
            const float dist = length(d);
            toL = d / max(dist, 1e-4f);
            const float k = saturate(1.0f - dist / max(L.range, 1e-4f));
            atten = k * k;
            if (L.type == 2) {
                const float cosA = dot(-toL, L.direction);
                const float spot =
                    saturate((cosA - L.cosOuter) / max(L.cosInner - L.cosOuter, 1e-4f));
                atten *= spot * spot;
            }
        }
        Lo += L.color * (L.intensity * atten * saturate(dot(N, toL)));
    }
    return albedo * (1.0f - metallic) * Lo;
}

// レイに沿った放射輝度を bounces 回まで積む。skyLod = ミス時のスカイ mip。
// cosine 重点サンプリングと 1/PI 省略規約により、throughput は albedo の積そのものになる
// (BRDF の 1/PI を省いた分と pdf の PI が相殺する)。
//
// envOnLastHit = 1 で、打ち切り点のヒット面に「環境からの拡散入射」を近似で足す
// (法線方向のスカイ放射輝度 × albedo)。**反射 (M46h) では必須** — 反射は
// ラスタのスペキュラ環境項を丸ごと置き換えるので、映り込んだ面の明るさが
// ラスタで見たときの明るさと揃っていないと、日向を向いていない面が黒く抜ける。
// 逆に**拡散 GI (M46c) では 0** — GI はラスタの拡散環境項を置き換える側なので、
// バウンス先にも環境項を足すと同じ光を二重に数えることになる。
//
// v1 制限: ヒット点のシェーディングは拡散のみ — 二次ヒット面の鏡面反射は評価しない
// (金属に映った金属は黒く落ちる)。マテリアルは定数のみでテクスチャは引かない
float3 RtTraceRadianceLod(float3 ro, float3 rd, float tMax, int bounces, inout uint3 seed,
                          float skyLod, float envOnLastHit)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);
    for (int b = 0; b < bounces; ++b) {
        RtHit hit;
        if (!RtTraceClosest(ro, rd, tMax, hit)) {
            radiance += throughput * RtSkyRadianceLod(rd, skyLod);
            break;
        }
        const float3 P = ro + rd * hit.t;
        float3 N = RtHitNormal(hit);
        if (dot(N, rd) > 0.0f) {
            N = -N; // 裏面ヒットは法線を反転 (マテリアルは両面扱い)
        }
        const RtMaterial m = RtHitMaterial(hit);
        radiance += throughput * (RtDirectLight(P, N, m.baseColor, m.metallic) + m.emissive);
        if (b + 1 >= bounces) {
            // 打ち切り点。これ以上バウンスしないので、ラスタが同じ面に与える環境項
            // (IBL 拡散 + IBL スペキュラ) を追加のレイ無しで 1 発近似する。
            //   拡散  = 法線方向のスカイ放射輝度 × albedo (粗い mip = irradiance の代用)。
            //           スカイ無しなら gRtAmbient = ラスタの簡易アンビエントと同値
            //   鏡面  = 鏡面方向のスカイ放射輝度 × F0 (mip を roughness で選ぶのは
            //           ラスタの prefiltered サンプルと同じ規約)。**これが無いと
            //           映り込んだ金属面が真っ黒になる** — 金属は拡散項が 0 のため
            const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), m.baseColor, m.metallic);
            const float3 envDiffuse = RtSkyRadianceLod(N, 4.0f) * m.baseColor
                * (1.0f - m.metallic);
            const float3 envSpec = RtSkyRadianceLod(reflect(rd, N), m.roughness * 4.0f) * f0;
            radiance += throughput * envOnLastHit * (envDiffuse + envSpec);
            break;
        }
        throughput *= m.baseColor * (1.0f - m.metallic);
        rd = RtCosineHemisphere(N, RtNextRand2(seed));
        ro = P + N * gRtRayEps;
    }
    return radiance;
}

// 拡散 GI 既定 (skyLod = 2 / 環境項なし)。M46c からの呼び出しはこちら = 出力はビット不変
float3 RtTraceRadiance(float3 ro, float3 rd, float tMax, int bounces, inout uint3 seed)
{
    return RtTraceRadianceLod(ro, rd, tMax, bounces, seed, 2.0f, 0.0f);
}

#endif // MYE_RT_COMMON_INCLUDED
