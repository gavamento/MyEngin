// Forward パス標準ライティング (平行光 + アンビエント)
// エントリ: VSMain / PSMain (ShaderManager の規約)

#include "common.hlsli"

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

Texture2D                gAlbedo        : register(t0);
Texture2D                gShadowMap     : register(t1);
Texture2D                gNormalTex     : register(t2);
SamplerState             gSampler       : register(s0);
SamplerComparisonState   gShadowSampler : register(s1);

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
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
    const float4 posW = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)gWorld));
    o.uv = v.uv;
    o.posW = posW.xyz;
    return o;
}

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
    const float3 color = ApplyLighting(albedo.rgb, n, i.posW, gCameraPos, gMetallic, gRoughness,
                                       gAmbient, gLights, gLightCount, dirShadow);
    return float4(color, albedo.a);
}
