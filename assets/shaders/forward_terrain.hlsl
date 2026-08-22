// 地形の Forward パス (M58c、spec §6.5)。deferred_terrain.hlsl と対で、
// 「Forward / Deferred のどちらで描いても地形が同じように見える」を成立させる。
//
// ★**b0 の PerFrame は forward_lit.hlsl と同一レイアウトの完全ミラー。** 地形は
//   ライティング一式 (ライト配列 / CSM / IBL / フォグ) を読むので前半だけでは足りない。
//   **Forward 系 CB に末尾 append したら本ファイルにも足すこと** — 同期対象は
//   forward_lit / forward_lit_instanced / forward_skinned / **forward_terrain** の 4 本 +
//   C++ ミラー (ForwardPath.cpp / DeferredPath.cpp の PerFrameCB)。食い違うと定数バッファの
//   ずれとして静かに壊れる (絵が暗くなる/影が消える形で出る)。
// ★**地形固有の値は b4** (b1-b3 を張り替えるとホストの透明描画が壊れる。
//   理由は deferred_terrain.hlsl の頭のコメントに書いた)。b4 の中身とレイヤの bind /
//   ブレンド本体は terrain_common.hlsli — deferred_terrain.hlsl と共有している。
// ★t0 (albedo) / t2 (normal) は宣言しない — 地形はマテリアルテクスチャを持たない
//   (`Material` はテクスチャ 2 枚までで 4 レイヤ x (albedo+normal) = 8 枚が入らない。M58d)。
//   レイヤのテクスチャは t20 以降の自前スロットへ逃がしてある。
//   t1 (CSM) と t3-t5 (IBL) は ForwardPath がフレーム頭で張ったものをそのまま読む。

#include "common.hlsli"
#include "terrain_common.hlsli"

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
    // ---- フォグ (M29d) ----
    float3   gFogColor;
    int      gFogMode; // -1=無効
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float    _fogPad;
    // ---- IBL (M38c) ----
    int      gIblEnabled;
    float    gIblSpecMips;
    float2   _iblPad;
    // ---- CSM (M38d)。gShadowVP はカスケード 0 ----
    float4x4 gShadowVP12[2];
    float4   gCascadeInfo; // xyz = split far 境界 / w = カスケード数
    // ---- M43a: ハイトフォグ + 太陽インスキャッタ ----
    float    gFogHeightFalloff;
    float    gFogBaseHeight;
    float    gFogInscatterIntensity;
    float    gFogInscatterPower;
    float3   gSunDirection;
    float    _fogPad2;
    float3   gSunColor;
    float    _fogPad3;
};

Texture2DArray         gShadowMap     : register(t1); // M38d: CSM カスケード配列
TextureCube            gIblIrradiance : register(t3); // M38c
TextureCube            gIblPrefiltered: register(t4);
Texture2D              gIblBrdfLut    : register(t5);
SamplerState           gLayerSampler  : register(s0); // 異方性 WRAP (レイヤの繰り返し)
SamplerComparisonState gShadowSampler : register(s1);
SamplerState           gIblSampler    : register(s2); // LINEAR/CLAMP (IBL + スプラット)

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
    const float4 posW = mul(float4(v.pos, 1.0f), gTerrainWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)gTerrainWorld));
    o.uv = v.uv;
    o.posW = posW.xyz;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float3 nGeom = normalize(i.normalW);
    // ★スプラットのサンプルは**分岐の外**で行う (ddx/ddy を使う PerturbNormal が
    //   非一様フローに入らないように)。影の分岐より先に済ませておく
    const TerrainSurfaceSample surf = SampleTerrainSurface(gLayerSampler, gIblSampler, i.uv);
    const float3 n = PerturbNormal(nGeom, i.posW, i.uv, normalize(surf.normalTS));

    float dirShadow = 1.0f;
    if (gShadowEnabled != 0) {
        dirShadow = SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0],
                                    gShadowVP12[1], (int)gCascadeInfo.w, i.posW, gShadowTexel);
    }
    // SSAO は Deferred 専用なので Forward は 1.0 固定 (forward_lit と同じ)
    float3 color = ApplyLighting(surf.albedo, n, i.posW, gCameraPos, gTerrainSurface.x,
                                 gTerrainSurface.y, gAmbient, gLights, gLightCount, dirShadow,
                                 gIblEnabled, gIblSpecMips, gIblIrradiance, gIblPrefiltered,
                                 gIblBrdfLut, gIblSampler, 1.0f);
    color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                     gCameraPos, i.posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                     gSunColor, gFogInscatterIntensity, gFogInscatterPower);
    return float4(color, 1.0f);
}
