// M79: プロジェクト側サーフェスシェーダー (*.surface.hlsl) 共通 include。
// 作者はこれを #include し、続けて MyEnginePerMaterial (Properties に対応する cbuffer) と
// Texture2D を register 無しで宣言し、VSIn/VSOut/VSMain/PSMain を書く。
//
// ここが供給する識別子はすべてエンジン予約 (再宣言禁止):
//   ・cbuffer 名が "MyEngine" で始まるもの
//   ・"gMye" / "_mye" で始まる識別子
//   ・位置に効く static: gViewProj / gWorld / gTime / gWaterTime
//   ・予約テクスチャ/サンプラ (gShadowMap 等、下記宣言のもの)
//
// **位置に効く値は static のみを使うこと**。予約 CB のフィールドを直接読むと、
// 影・速度パスでは代入前の値 (今フレームの色パス用の値) のままになり、
// 変位が影・TAA に反映されない (spec §4.1 作者規約「位置に効く値」)。

#include "common.hlsli"
#include "froxel_common.hlsli"
#include "acoustic_common.hlsli"

// ---- 位置に効く値 (生成エントリがパスごとに代入する。M79 sub-01 最大のリスク箇所) ----
static float4x4 gViewProj;
static float4x4 gWorld;
static float gTime;
static float gWaterTime;

// ---- MyEnginePerFrame: 既存 Forward/Deferred の PerFrame と同じ内容 (先頭の camera VP を除く。
//      理由は SurfaceShaderTypes.h の MyEnginePerFrameCB コメントを参照) ----
cbuffer MyEnginePerFrame
{
    float3 gCameraPos;
    int gLightCount;
    float3 gAmbient;
    float _myePad0;
    Light gLights[MAX_LIGHTS];
    float4x4 gShadowVP; // transpose(lightView*lightProj) カスケード 0
    float gShadowTexel;
    int gShadowEnabled;
    float2 _myePad1;
    float3 gFogColor;
    int gFogMode; // -1=無効
    float gFogDensity;
    float gFogStart;
    float gFogEnd;
    float _myeFogPad;
    int gIblEnabled;
    float gIblSpecMips;
    float2 _myeIblPad;
    float4x4 gShadowVP12[2]; // カスケード 1,2
    float4 gCascadeInfo; // xyz = split far 境界 (デバッグ用) / w = カスケード数
    float gFogHeightFalloff;
    float gFogBaseHeight;
    float gFogInscatterIntensity;
    float gFogInscatterPower;
    float3 gSunDirection; // 光の進行方向 (正規化)
    float _myeFogPad2;
    float3 gSunColor; // リニア・強度込み
    float _myeFogPad3;
    int gShadowAtlasEnabled;
    float gShadowAtlasTexel;
    float2 _myeAtlasPad;
    ShadowTile gShadowTiles[MYE_MAX_SHADOW_TILES];
    int gFroxelEnabled;
    float gFroxelNearZ;
    float gFroxelFarZ;
    float gFroxelSlices;
    float4 gFroxelViewZRow;
    float2 gFroxelScreenSize;
    float2 _myeFroxelPad;
    float4 gAcousticGridMin;
    float4 gAcousticInvSize;
    float4 gAcousticParams;
    float4 gAcousticFront;
    float4 gAcousticWaves[MYE_ACOUSTIC_WAVE_SLOTS * 2];
};

// ---- MyEngineSurfaceFrame: TAA 速度・CSM 影のための今/前の値 (生成エントリ専用) ----
cbuffer MyEngineSurfaceFrame
{
    float4x4 gMyeCurViewProj;
    float4x4 gMyePrevViewProj;
    float4x4 gMyeShadowViewProj;
    float gMyeCurTime;
    float gMyePrevTime;
    float gMyeCurWaterTime;
    float gMyePrevWaterTime;
    float2 gMyeJitterNdc;
    float2 gMyeScreenSize;
    int gMyeHistoryValid; // 0 = 前フレーム履歴なし
    float3 _myeSurfaceFramePad;
};

// ---- MyEnginePerObject ----
cbuffer MyEnginePerObject
{
    float4x4 gMyeWorld;
    float4x4 gMyePrevWorld;
    float4 gBaseColor; // マテリアル baseColor のリニア値 (作者が直接読んでよい)
};

// ---- MyEngineWater (sub-05 で配線。無効時は全 0 + enabled=0) ----
struct MyeGerstnerWave {
    float amplitude;
    float wavelength;
    float speed;
    float dirAngleDeg;
    float steepness;
    float3 _pad;
};

cbuffer MyEngineWater
{
    int gMyeWaterEnabled;
    float gMyeWaterBaseHeight;
    float gMyeWaterOverallScale;
    int gMyeWaterWaveCount;
    MyeGerstnerWave gMyeWaterWaves[4];
    float4 gMyeWaterDeepColor;
    float4 gMyeWaterShallowColor;
};

// ---- 予約テクスチャ / サンプラ ----
Texture2DArray gShadowMap; // M38d CSM カスケード配列 (register は自動割当)
TextureCube gIblIrradiance;
TextureCube gIblPrefiltered;
Texture2D gIblBrdfLut;
Texture3D gFroxelVolume;
SamplerState gSampler;             // 線形 Wrap
SamplerComparisonState gShadowSampler; // CSM の比較サンプラ
SamplerState gIblSampler;          // 線形 Clamp

// ---- ヘルパ (v1: 太陽 CSM の影・解析フォグ・環境光/太陽色のみ。IBL/局所影/音響/フロクセルの
//      合成は後回し — CB のフィールドは確保済みだが、ここでは使わない) ----

// CSM の減衰 (1=影なし)
float MyeSunShadow(float3 posW)
{
    if (gShadowEnabled == 0) {
        return 1.0f;
    }
    return SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0], gShadowVP12[1],
                           (int)gCascadeInfo.w, posW, gShadowTexel);
}

// 距離フォグ (解析式のみ。フロクセルは後回し)
float3 MyeApplyFog(float3 color, float3 posW)
{
    return ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd, gCameraPos, posW,
                    gFogHeightFalloff, gFogBaseHeight, gSunDirection, gSunColor,
                    gFogInscatterIntensity, gFogInscatterPower);
}
