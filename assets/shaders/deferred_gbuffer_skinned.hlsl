// Deferred ジオメトリパス スキニング版 (M18)。VS だけが deferred_gbuffer と異なる。
// PS / cbuffer / テクスチャは deferred_gbuffer.hlsl と同一 — 変更時は両方を更新すること。

#include "common.hlsli" // PerturbNormal (M17.3)

// RenderTypes.h の mye::kMaxBones と必ず一致させること (check_rules.ps1 規則 9 が検査)
#define MYE_MAX_BONES 128

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    float    _pad0;
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

Texture2D    gAlbedoTex : register(t0);
Texture2D    gNormalTex : register(t1);
SamplerState gSampler   : register(s0);

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

struct PSOut
{
    float4 albedo   : SV_Target0;
    float4 normal   : SV_Target1;
    float4 position : SV_Target2;
    float4 material : SV_Target3;
};

// --- 以下 PS は deferred_gbuffer.hlsl と同一 ---
PSOut PSMain(VSOut i)
{
    PSOut o;
    float3 n = normalize(i.normalW);
    if (gHasNormal != 0) {
        const float3 tsN = gNormalTex.Sample(gSampler, i.uv).xyz * 2.0f - 1.0f;
        n = PerturbNormal(n, i.posW, i.uv, tsN);
    }
    const float4 albedo = gAlbedoTex.Sample(gSampler, i.uv) * gBaseColor;
    o.albedo = float4(albedo.rgb, 1.0f);
    o.normal = float4(n * 0.5f + 0.5f, 1.0f);
    o.position = float4(i.posW, 1.0f);
    o.material = float4(gMetallic, gRoughness, 0.0f, 1.0f);
    return o;
}
