// 地面の霧 (M96 sub-03): かき乱しフィールドの 2D 層を、各ピクセルで板のレイマーチとして描く。
//
// フロクセル (奥行きを 64 枚ほどに割った 3D) は浅い角度で見ると深度スライスが横縞に見えるので、
// 地面の霧はフロクセルに入れず、カメラ → 面の線分と層の板の交差区間を固定段数で積分して直接合成する。
//
// ★束縛番号は**全ての PS 共通の固定番号**。RenderSystem が 1 フレームに 1 回だけ PS へ張る (DisturbanceFieldPass.h
//   の kGroundFog* と一致させる。tools\check_rules.ps1 規則 9)。各シェーダは include するだけで束縛を持たない。
// ★層が 0 枚、または線分がどの板とも交わらないピクセルは入力の color をそのまま返す (ビット恒等)。
// ★順序は「地面の霧 → 既存の解析フォグとフロクセル」。地面の霧は面のすぐ手前にあるので先に掛け、
//   その外側の大気を後に掛ける。
#ifndef MYE_GROUND_FOG_HLSLI
#define MYE_GROUND_FOG_HLSLI

// PS 用の束縛番号 (C++ の kGroundFogSrvFlowSlot ほか)
#define MYE_GFOG_SRV_FLOW 32
#define MYE_GFOG_SRV_AUX 33
#define MYE_GFOG_SAMPLER 8
#define MYE_GFOG_CB 6
#define MYE_GFOG_VIEW_CB 7
// 板の積分の段数 (C++ の kGroundFogSteps)
#define MYE_GFOG_STEPS 24
// スカイの代わりに使う「遠方」までの距離 [m]
#define MYE_GFOG_SKY_DISTANCE 2000.0f

#define MYE_DFLD_SRV_FLOW MYE_GFOG_SRV_FLOW
#define MYE_DFLD_SRV_AUX MYE_GFOG_SRV_AUX
#define MYE_DFLD_SAMPLER MYE_GFOG_SAMPLER
#define MYE_DFLD_CB MYE_GFOG_CB
#include "disturbance_common.hlsli"

// 1 回の描画 (ビュー) ごとに変わる値。C++ の GroundFogViewCB と同じ並び (float4 x 8 = 128 バイト)
cbuffer GroundFogViewCB : MYE_DFLD_REG(b, MYE_GFOG_VIEW_CB)
{
    float4 gGfogCamPos;   // xyz = カメラ位置, w = HG の異方性 g
    float4 gGfogSunDir;   // xyz = 太陽 (平行光) の進行方向 (正規化), w = 1: 平行光あり
    float4 gGfogSunColor; // xyz = 太陽の色 × 強度 (リニア), w = 散乱アルベド
    float4 gGfogAmbient;  // xyz = 環境光 (リニア)
    float4x4 gGfogInvViewProj; // transpose(inverse(view * proj))。背景 (ジオメトリ無し) の視線の復元用
};

// フロクセル注入の HenyeyGreenstein と同一式 (cosTheta = dot(光の進行方向, 散乱点 → カメラ方向))
float GfogPhase(float cosTheta, float g)
{
    const float gg = clamp(g, -0.95f, 0.95f);
    const float d = max(1.0f + gg * gg - 2.0f * gg * cosTheta, 1e-4f);
    return (1.0f - gg * gg) / (4.0f * 3.14159265f * (d * sqrt(d)));
}

// 段の始まりのずらし [0,1)。ピクセル位置だけで決まる (interleaved gradient noise)。
// フレーム番号を使わない = 同じ入力から同じ絵
float GfogStepJitter(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// 光線 o + d·t を [lo, hi] の板へ当てた区間へ、t の区間 range を狭める。空になったら range.x > range.y
float2 GfogSlab(float o, float d, float lo, float hi, float2 range)
{
    if (abs(d) < 1e-6f) {
        return (o < lo || o > hi) ? float2(1.0f, 0.0f) : range;
    }
    const float a = (lo - o) / d;
    const float b = (hi - o) / d;
    return float2(max(range.x, min(a, b)), min(range.y, max(a, b)));
}

// 段 1 つぶんの散乱放射輝度 (単位 σ あたり)。局所ライトの寄与はここへ 1 項足す (sub-15)
float3 GfogStepRadiance(DisturbanceLayer L, float3 p, float3 rayDir)
{
    float3 radiance = gGfogSunColor.w * gGfogAmbient.xyz + L.fogColor.rgb * L.look.y; // 環境光 + 自己照明
    if (L.fogColor.w > 0.5f && gGfogSunDir.w > 0.5f) {
        // 太陽光 (影なし)。位相関数の向きは「光の進行方向」と「散乱点 → カメラ」
        radiance += gGfogSunColor.w * gGfogSunColor.xyz * GfogPhase(dot(gGfogSunDir.xyz, -rayDir), gGfogCamPos.w);
    }
    return radiance;
}

// 層 i の板を [t0, t1] で積分して、(color × T + inscatter) を返す。additive のときは inscatter を足さない
float3 GfogIntegrateLayer(uint i, float3 origin, float3 rayDir, float t0, float t1, float jitter, float3 color,
                          bool additive)
{
    const DisturbanceLayer L = gDfldLayers[i];
    const float dt = (t1 - t0) / (float)MYE_GFOG_STEPS;
    float transmittance = 1.0f;
    float3 inscatter = 0.0f;
    [loop] for (int k = 0; k < MYE_GFOG_STEPS; ++k) {
        const float t = t0 + ((float)k + jitter) * dt;
        const float3 p = origin + rayDir * t;
        const float sigma = DfldLayerSigma(i, p);
        if (sigma <= 0.0f) {
            continue;
        }
        const float stepT = exp(-sigma * dt);
        // 1 段の中で σ が一定として厳密に積分する: ∫ σ·exp(-σs) ds = 1 - exp(-σ·dt)
        if (!additive) {
            inscatter += transmittance * (1.0f - stepT) * GfogStepRadiance(L, p, rayDir);
        }
        transmittance *= stepT;
    }
    return color * transmittance + inscatter;
}

// カメラから posW までの間にある地面の霧を color へ合成する。pixel = SV_Position.xy (段のずらしに使う)。
// additive = 加算合成の描画物 (火花など): 霧の in-scatter は足さず透過率だけ掛ける
float3 GroundFogCompositeImpl(float3 color, float3 posW, float2 pixel, bool additive)
{
    const uint count = min(gDfldHeader.x, (uint)MYE_DFLD_MAX_LAYERS);
    if (count == 0) {
        return color;
    }
    const float3 origin = gGfogCamPos.xyz;
    const float3 toSurface = posW - origin;
    const float len = length(toSurface);
    if (len < 1e-4f) {
        return color;
    }
    const float3 rayDir = toSurface / len;

    // 層ごとの交差区間。奥の層から順に重ねる必要があるので、先に全部集めて t0 の大きい順に並べる
    float2 span[MYE_DFLD_MAX_LAYERS];
    uint order[MYE_DFLD_MAX_LAYERS];
    uint hit = 0;
    [unroll] for (uint i = 0; i < (uint)MYE_DFLD_MAX_LAYERS; ++i) {
        span[i] = float2(1.0f, 0.0f);
        order[i] = 0;
    }
    [loop] for (uint li = 0; li < count; ++li) {
        const DisturbanceLayer L = gDfldLayers[li];
        if (L.look.w < 0.5f || L.look.x <= 0.0f) {
            continue;
        }
        const float cs = L.originSize.z;
        const float yLo = L.originSize.w + L.texScale.z;
        const float yHi = L.originSize.w + L.texScale.w + L.extent.z;
        float2 r = float2(0.0f, len);
        r = GfogSlab(origin.x, rayDir.x, L.originSize.x * cs, (L.originSize.x + L.extent.x) * cs, r);
        r = GfogSlab(origin.z, rayDir.z, L.originSize.y * cs, (L.originSize.y + L.extent.y) * cs, r);
        r = GfogSlab(origin.y, rayDir.y, yLo, yHi, r);
        if (r.x >= r.y) {
            continue;
        }
        span[li] = r;
        order[hit] = li;
        ++hit;
    }
    if (hit == 0) {
        return color;
    }
    // 挿入ソート (最大 4 枚): 奥 (t0 が大きい) が先
    [unroll] for (uint a = 1; a < (uint)MYE_DFLD_MAX_LAYERS; ++a) {
        [unroll] for (uint b = a; b > 0; --b) {
            if (b < hit && span[order[b]].x > span[order[b - 1]].x) {
                const uint tmp = order[b];
                order[b] = order[b - 1];
                order[b - 1] = tmp;
            }
        }
    }
    const float jitter = GfogStepJitter(pixel);
    [loop] for (uint h = 0; h < hit; ++h) {
        const uint li = order[h];
        color = GfogIntegrateLayer(li, origin, rayDir, span[li].x, span[li].y, jitter, color, additive);
    }
    return color;
}

float3 GroundFogComposite(float3 color, float3 posW, float2 pixel)
{
    return GroundFogCompositeImpl(color, posW, pixel, false);
}

// 加算合成の描画物用 (透過率だけ掛ける)
float3 GroundFogCompositeAdditive(float3 color, float3 posW, float2 pixel)
{
    return GroundFogCompositeImpl(color, posW, pixel, true);
}

// スカイ用: 視線方向に MYE_GFOG_SKY_DISTANCE だけ先の点を面として扱う
float3 GroundFogCompositeSky(float3 color, float3 viewDir, float2 pixel)
{
    return GroundFogComposite(color, gGfogCamPos.xyz + normalize(viewDir) * MYE_GFOG_SKY_DISTANCE, pixel);
}

// ジオメトリの無い画素 (Deferred の背景) 用: 逆ビュー射影から視線を復元し、スカイと同じく遠方を面として扱う。
// skybox.hlsl の視線の復元と同じ式
float3 GroundFogCompositeBackground(float3 color, float2 pixel, float2 screenSize)
{
    if (gDfldHeader.x == 0) {
        return color; // 層が無ければ逆行列を読まない (CB が未束縛のとき 0 行列)
    }
    const float2 uv = pixel / screenSize;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 pf = mul(float4(ndc, 1.0f, 1.0f), gGfogInvViewProj);
    const float4 pn = mul(float4(ndc, 0.0f, 1.0f), gGfogInvViewProj);
    return GroundFogCompositeSky(color, pf.xyz / pf.w - pn.xyz / pn.w, pixel);
}

#endif
