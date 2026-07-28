// Deferred ライティングパス: フルスクリーン三角形で GBuffer を解決する。
// ライティング計算は Forward と同じ common.hlsli の関数を使う (見た目の一致)

#include "common.hlsli"

cbuffer LightPass : register(b0)
{
    float3 gAmbient;
    int    gLightCount;
    float4 gClearColor; // ジオメトリの無いピクセルの色
    Light  gLights[MAX_LIGHTS];
    float4x4 gShadowVP;
    float    gShadowTexel;
    int      gShadowEnabled;
    float2   _pad1;
    float3   gCameraPos;
    float    _pad2;
    // ---- フォグ (M29d、末尾 append) ----
    float3   gFogColor;
    int      gFogMode; // -1=無効
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float    _fogPad;
    // ---- IBL (M38c、末尾 append) ----
    int      gIblEnabled;
    float    gIblSpecMips;
    float2   _iblPad;
    // ---- CSM (M38d、末尾 append)。gShadowVP はカスケード 0 ----
    float4x4 gShadowVP12[2];
    float4   gCascadeInfo; // xyz = split far 境界 / w = カスケード数
    // ---- SSAO (M38e、末尾 append) ----
    float2   gScreenSize;  // フル解像度 (SV_Position → uv 変換用)
    int      gSsaoEnabled; // 0=無効
    float    _ssaoPad;
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

Texture2D gAlbedo    : register(t0);
Texture2D gNormal    : register(t1);
Texture2D gPosition  : register(t2); // ワールド座標 (GBuffer)
Texture2D gMaterial  : register(t3); // r=metallic g=roughness
Texture2DArray gShadowMap : register(t4); // M38d: CSM カスケード配列
TextureCube gIblIrradiance  : register(t5); // M38c
TextureCube gIblPrefiltered : register(t6);
Texture2D   gIblBrdfLut     : register(t7);
Texture2D   gSsao           : register(t8); // M38e (半解像度、ブラー済み AO)
SamplerState gIblSampler : register(s0); // LINEAR/CLAMP (M38c、s0 は光パスで空きだった)
SamplerComparisonState gShadowSampler : register(s1);

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // フルスクリーン三角形
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const int3 pixel = int3(int2(i.pos.xy), 0);
    const float4 albedo = gAlbedo.Load(pixel);
    if (albedo.a < 0.5f) {
        return gClearColor; // ジオメトリ無し
    }
    const float3 n = normalize(gNormal.Load(pixel).xyz * 2.0f - 1.0f);
    const float3 posW = gPosition.Load(pixel).xyz;
    const float2 mr = gMaterial.Load(pixel).rg; // metallic, roughness
    float dirShadow = 1.0f;
    if (gShadowEnabled != 0) {
        dirShadow = SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0],
                                    gShadowVP12[1], (int)gCascadeInfo.w, posW, gShadowTexel);
    }
    float ao = 1.0f;
    if (gSsaoEnabled != 0) {
        ao = gSsao.SampleLevel(gIblSampler, i.pos.xy / gScreenSize, 0).r; // M38e
    }
    float3 color = ApplyLighting(albedo.rgb, n, posW, gCameraPos, mr.x, mr.y, gAmbient,
                                 gLights, gLightCount, dirShadow, gIblEnabled, gIblSpecMips,
                                 gIblIrradiance, gIblPrefiltered, gIblBrdfLut, gIblSampler, ao);
    color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                     gCameraPos, posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                     gSunColor, gFogInscatterIntensity, gFogInscatterPower); // M29d+M43a
    return float4(color, 1.0f);
}
