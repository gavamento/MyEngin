// Forward パス スキニング版 (M18)。VS だけが forward_lit と異なる (GPU スキニング)。
// PS / cbuffer / テクスチャは forward_lit.hlsl と同一 — 変更時は両方を更新すること。
// エントリ: VSMain / PSMain (ShaderManager の規約)

#include "common.hlsli"

#define MYE_MAX_BONES 64

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    int      gLightCount;
    float3   gAmbient;
    float    _pad0;
    Light    gLights[MAX_LIGHTS];
    float4x4 gShadowVP;      // transpose(lightView*lightProj)
    float    gShadowTexel;   // 1/解像度
    int      gShadowEnabled; // 0=影無効
    float2   _pad1;
    // ---- フォグ (M29d、末尾 append。forward_lit と同一レイアウト) ----
    float3   gFogColor;
    int      gFogMode; // -1=無効
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float    _fogPad;
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4   gBaseColor;
};

cbuffer MaterialParams : register(b2)
{
    float  gMetallic;
    float  gRoughness;
    int    gHasNormal; // 0=ノーマルマップ無し (幾何法線をそのまま使う)
    float  _matPad;
};

// ボーンパレット (M18)。各行列 = transpose(inverseBind * jointWorld) (行ベクトル規約)。
cbuffer BonePalette : register(b3)
{
    float4x4 gBones[MYE_MAX_BONES];
};

Texture2D                gAlbedo        : register(t0);
Texture2D                gShadowMap     : register(t1);
Texture2D                gNormalTex     : register(t2);
SamplerState             gSampler       : register(s0);
SamplerComparisonState   gShadowSampler : register(s1);

struct VSIn
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD0;
    uint4  bones   : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

struct VSOut
{
    float4 pos     : SV_Position;
    float3 normalW : NORMAL;
    float2 uv      : TEXCOORD0;
    float3 posW    : TEXCOORD1;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    float3 localPos = v.pos;
    float3 localNrm = v.normal;
    const float wsum = v.weights.x + v.weights.y + v.weights.z + v.weights.w;
    if (wsum > 1e-4f) {
        // 行ベクトル規約: skinnedPos = sum_i weight_i * (pos * gBones[idx_i])
        const float4 p = float4(v.pos, 1.0f);
        float3 sp = mul(p, gBones[v.bones.x]).xyz * v.weights.x;
        sp += mul(p, gBones[v.bones.y]).xyz * v.weights.y;
        sp += mul(p, gBones[v.bones.z]).xyz * v.weights.z;
        sp += mul(p, gBones[v.bones.w]).xyz * v.weights.w;
        float3 sn = mul(v.normal, (float3x3)gBones[v.bones.x]) * v.weights.x;
        sn += mul(v.normal, (float3x3)gBones[v.bones.y]) * v.weights.y;
        sn += mul(v.normal, (float3x3)gBones[v.bones.z]) * v.weights.z;
        sn += mul(v.normal, (float3x3)gBones[v.bones.w]) * v.weights.w;
        localPos = sp;
        localNrm = sn;
    }
    const float4 posW = mul(float4(localPos, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(localNrm, (float3x3)gWorld));
    o.uv = v.uv;
    o.posW = posW.xyz;
    return o;
}

// --- 以下 PS は forward_lit.hlsl と同一 ---
float4 PSMain(VSOut i) : SV_Target
{
    float3 n = normalize(i.normalW);
    if (gHasNormal != 0) {
        const float3 tsN = gNormalTex.Sample(gSampler, i.uv).xyz * 2.0f - 1.0f;
        n = PerturbNormal(n, i.posW, i.uv, tsN);
    }
    const float4 albedo = gAlbedo.Sample(gSampler, i.uv) * gBaseColor;
    float dirShadow = 1.0f;
    if (gShadowEnabled != 0) {
        dirShadow = SampleShadowPCF(gShadowMap, gShadowSampler, gShadowVP, i.posW, gShadowTexel);
    }
    float3 color = ApplyLighting(albedo.rgb, n, i.posW, gCameraPos, gMetallic, gRoughness,
                                 gAmbient, gLights, gLightCount, dirShadow);
    color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                     length(gCameraPos - i.posW)); // M29d
    return float4(color, albedo.a);
}
