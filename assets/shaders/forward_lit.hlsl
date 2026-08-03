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
    // ---- フォグ (M29d、末尾 append) ----
    float3   gFogColor;
    int      gFogMode; // -1=無効
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float    _fogPad;
    // ---- IBL (M38c、末尾 append) ----
    int      gIblEnabled;  // 0=定数アンビエント (従来)
    float    gIblSpecMips; // prefiltered の最終 mip (roughness*mips で LOD)
    float2   _iblPad;
    // ---- CSM (M38d、末尾 append)。gShadowVP はカスケード 0 ----
    float4x4 gShadowVP12[2];
    float4   gCascadeInfo; // xyz = split far 境界 (デバッグ用) / w = カスケード数
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append。既定 = 恒等) ----
    float    gFogHeightFalloff;      // 0 = 高さ一様 (従来)
    float    gFogBaseHeight;
    float    gFogInscatterIntensity; // 0 = 無効
    float    gFogInscatterPower;
    float3   gSunDirection;          // 光の進行方向 (正規化)
    float    _fogPad2;
    float3   gSunColor;              // リニア・強度込み (平行光無し = 黒 + intensity 0)
    float    _fogPad3;
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
    float  gEmissive; // M46i: 自己発光の強さ (0 = 発光なし)
};

Texture2D                gAlbedo        : register(t0);
Texture2DArray           gShadowMap     : register(t1); // M38d: CSM カスケード配列
Texture2D                gNormalTex     : register(t2);
TextureCube              gIblIrradiance : register(t3); // M38c
TextureCube              gIblPrefiltered: register(t4);
Texture2D                gIblBrdfLut    : register(t5);
SamplerState             gSampler       : register(s0);
SamplerComparisonState   gShadowSampler : register(s1);
SamplerState             gIblSampler    : register(s2); // LINEAR/CLAMP (M38c)

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
        dirShadow = SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0],
                                    gShadowVP12[1], (int)gCascadeInfo.w, i.posW, gShadowTexel);
    }
    float3 color = ApplyLighting(albedo.rgb, n, i.posW, gCameraPos, gMetallic, gRoughness,
                                 gAmbient, gLights, gLightCount, dirShadow, gIblEnabled,
                                 gIblSpecMips, gIblIrradiance, gIblPrefiltered, gIblBrdfLut,
                                 gIblSampler, 1.0f); // SSAO は Deferred のみ
    // M46i: 自己発光。ライティングに依らず放射する分を足す (フォグより前 =
    // 遠くの発光もフォグに減衰される)。gEmissive=0 なら加算項がちょうど 0
    color += albedo.rgb * gEmissive;
    color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                     gCameraPos, i.posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                     gSunColor, gFogInscatterIntensity, gFogInscatterPower); // M29d+M43a
    return float4(color, albedo.a);
}
