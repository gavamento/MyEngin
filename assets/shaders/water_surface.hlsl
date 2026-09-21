// 水面波シェーダー (Gerstner Wave Water Surface)
// 頂点シェーダーで三角関数 (Gerstner 波) による波高・法線変位を行い、
// ピクセルシェーダーでフレネル反射・水深グラデーション・白波 (Foam) を描画する。

#include "common.hlsli"
#include "froxel_common.hlsli"
#include "acoustic_common.hlsli"

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    int      gLightCount;
    float3   gAmbient;
    float    _pad0;
    Light    gLights[MAX_LIGHTS];
    float4x4 gShadowVP;
    float    gShadowTexel;
    int      gShadowEnabled;
    float2   _pad1;
    float3   gFogColor;
    int      gFogMode;
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float    _fogPad;
    int      gIblEnabled;
    float    gIblSpecMips;
    float2   _iblPad;
    float4x4 gShadowVP12[2];
    float4   gCascadeInfo;
    float    gFogHeightFalloff;
    float    gFogBaseHeight;
    float    gFogInscatterIntensity;
    float    gFogInscatterPower;
    float3   gSunDirection;
    float    _fogPad2;
    float3   gSunColor;
    float    _fogPad3;
    int      gShadowAtlasEnabled;
    float    gShadowAtlasTexel;
    float2   _atlasPad;
    ShadowTile gShadowTiles[MYE_MAX_SHADOW_TILES];
    int      gFroxelEnabled;
    float    gFroxelNearZ;
    float    gFroxelFarZ;
    float    gFroxelSlices;
    float4   gFroxelViewZRow;
    float2   gFroxelScreenSize;
    float2   _froxelPad;
    float4   gAcousticGridMin;
    float4   gAcousticInvSize;
    float4   gAcousticParams;
    float4   gAcousticFront;
    float4   gAcousticWaves[MYE_ACOUSTIC_WAVE_SLOTS * 2];
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4   gBaseColor;
    int      gInstanceBase;
    float    gRtReceiver;
    float2   _instPad;
};

// 水面パラメータ定数バッファ (MaterialParams スロット b2 にバインド、またはカスタム定数)
cbuffer WaterMaterialParams : register(b2)
{
    float4 gWaterDeepColor;    // 深水色 (RGBA)
    float4 gWaterShallowColor; // 浅水色 (RGBA)
    float4 gWaveParams0;       // x=amp, y=wavelength, z=speed, w=dirAngleDeg
    float4 gWaveParams1;       // x=amp, y=wavelength, z=speed, w=dirAngleDeg
    float4 gWaveParams2;       // x=amp, y=wavelength, z=speed, w=dirAngleDeg
    float4 gWaveParams3;       // x=amp, y=wavelength, z=speed, w=dirAngleDeg
    float4 gWaveSteepness;     // x=s0, y=s1, z=s2, w=s3
    float4 gWaterSettings;     // x=baseHeight, y=overallScale, z=time, w=foamStrength
    float4 gWaterOptics;       // x=fresnelPower, y=smoothness, z=pad, w=pad
};

Texture2D                gSkyPanoramic  : register(t0);
Texture2DArray           gShadowMap     : register(t1);
Texture2D                gNormalTex     : register(t2);
TextureCube              gIblIrradiance : register(t3);
TextureCube              gIblPrefiltered: register(t4);
Texture2D                gIblBrdfLut    : register(t5);
Texture2D                gShadowAtlas   : register(t6);
Texture3D                gFroxelVolume  : register(t7);
Texture2D                gRtRefl        : register(t8);

SamplerState             gSampler       : register(s0);
SamplerComparisonState   gShadowSampler : register(s1);
SamplerState             gIblSampler    : register(s2);

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

struct VSOut
{
    float4 pos       : SV_Position;
    float3 normalW   : NORMAL;
    float2 uv        : TEXCOORD0;
    float3 posW      : TEXCOORD1;
    float  waveCrest : TEXCOORD3; // 波頭の尖り具合 (白波計算用)
};

static const float kPi = 3.14159265359f;
static const float kTwoPi = 6.28318530718f;
static const float kDegToRad = kPi / 180.0f;

// 1 本の Gerstner 波を評価して変位と法線偏微分を積算
void AccumulateGerstnerWave(float4 waveP, float steepness, float overallScale, float time,
                            float x, float z,
                            inout float3 disp, inout float3 nDiff)
{
    const float a = max(0.0f, waveP.x) * overallScale;
    if (a <= 1e-6f) {
        return;
    }
    const float len = max(0.1f, waveP.y);
    const float k = kTwoPi / len;
    const float omega = k * waveP.z;
    const float rad = waveP.w * kDegToRad;
    const float dirX = cos(rad);
    const float dirZ = sin(rad);

    const float q = saturate(steepness);
    const float qa = q * a;

    const float phi = k * (dirX * x + dirZ * z) - omega * time;
    const float s = sin(phi);
    const float c = cos(phi);

    disp.x -= dirX * qa * s;
    disp.z -= dirZ * qa * s;
    disp.y += a * c;

    const float ka = k * a;
    nDiff.x -= dirX * ka * s;
    nDiff.z -= dirZ * ka * s;
    nDiff.y -= q * ka * c;
}

VSOut VSMain(VSIn v)
{
    VSOut o;

    // 平面メッシュの初期ワールド位置 (変位前)
    const float4 basePosW = mul(float4(v.pos, 1.0f), gWorld);
    const float time = gWaterSettings.z;
    const float overallScale = gWaterSettings.y;
    const float baseHeight = gWaterSettings.x;

    float3 disp = float3(0.0f, 0.0f, 0.0f);
    float3 nDiff = float3(0.0f, 1.0f, 0.0f);

    AccumulateGerstnerWave(gWaveParams0, gWaveSteepness.x, overallScale, time, basePosW.x, basePosW.z, disp, nDiff);
    AccumulateGerstnerWave(gWaveParams1, gWaveSteepness.y, overallScale, time, basePosW.x, basePosW.z, disp, nDiff);
    AccumulateGerstnerWave(gWaveParams2, gWaveSteepness.z, overallScale, time, basePosW.x, basePosW.z, disp, nDiff);
    AccumulateGerstnerWave(gWaveParams3, gWaveSteepness.w, overallScale, time, basePosW.x, basePosW.z, disp, nDiff);

    float3 posW = basePosW.xyz + disp;
    posW.y += baseHeight;

    o.pos = mul(float4(posW, 1.0f), gViewProj);
    o.posW = posW;
    o.normalW = normalize(nDiff);
    o.uv = v.uv;

    // 波頭の尖り具合 (disp.y が高く、法線が立っている部分)
    o.waveCrest = saturate((disp.y / max(0.01f, overallScale * 0.5f)) * 0.5f + 0.5f);

    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float3 n = normalize(i.normalW);
    float3 v = normalize(gCameraPos - i.posW);

    // フレネル反射 (Schlick 近似)
    const float ndotv = saturate(dot(n, v));
    const float fresnelPower = max(1.0f, gWaterOptics.x);
    const float f0 = 0.10f; // 水面の基本垂直反射率 (見下ろし時でも背景・空が綺麗に反射するよう適度に確保)
    const float fresnel = f0 + (1.0f - f0) * pow(1.0f - ndotv, fresnelPower);

    // 水深・傾きに基づく基本水色ブレンド
    const float depthFactor = saturate(ndotv * 0.8f + 0.2f);
    float4 waterColor = lerp(gWaterDeepColor, gWaterShallowColor, depthFactor);

    // 太陽光・環境光ライティング
    float dirShadow = 1.0f;
    if (gShadowEnabled != 0) {
        dirShadow = SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0],
                                    gShadowVP12[1], (int)gCascadeInfo.w, i.posW, gShadowTexel);
    }

    float localShadow[MAX_LIGHTS];
    ResolveLocalShadows(gShadowAtlas, gShadowSampler, gShadowTiles, gLights, gLightCount,
                        gShadowAtlasEnabled, i.posW, gShadowAtlasTexel, localShadow);

    const float metallic = 0.0f;
    const float roughness = 1.0f - saturate(gWaterOptics.y); // smoothness → roughness

    float3 litColor = ApplyLighting(waterColor.rgb, n, i.posW, gCameraPos, metallic, roughness,
                                    gAmbient, gLights, gLightCount, dirShadow, localShadow,
                                    gIblEnabled, gIblSpecMips, gIblIrradiance, gIblPrefiltered,
                                    gIblBrdfLut, gIblSampler, 1.0f);

    // スペキュラ反射・スカイボックス反射のサンプリング
    const float3 reflDir = reflect(-v, n);
    float3 skyReflection = gAmbient;

    const int skyMode = (int)(gWaterOptics.w + 0.5f);
    if (skyMode == 2) {
        // パノラマ (2D Equirectangular) スカイボックスを反射方向から直接高解像度サンプリング
        float2 panoUV;
        panoUV.x = 0.5f + atan2(reflDir.z, reflDir.x) / (2.0f * kPi);
        panoUV.y = 0.5f - asin(clamp(reflDir.y, -1.0f, 1.0f)) / kPi;
        skyReflection = gSkyPanoramic.SampleLevel(gSampler, panoUV, roughness * 3.0f).rgb;
    } else if (gIblEnabled != 0 || skyMode == 1) {
        skyReflection = gIblPrefiltered.SampleLevel(gIblSampler, reflDir, roughness * gIblSpecMips).rgb;
    }

    // RT 反射のサンプリングと IBL とのハイブリッド合成 (M69b)
    float3 reflColor = skyReflection;
    const bool rtReflOn = (gWaterOptics.z > 0.5f);
    if (rtReflOn && gFroxelScreenSize.x > 1.0f && gFroxelScreenSize.y > 1.0f) {
        const float2 screenUV = i.pos.xy / gFroxelScreenSize;
        // 水面の波法線によるスクリーン空間歪みオフセット
        const float distortScale = 0.05f * saturate(1.0f - ndotv);
        const float2 sampleUV = clamp(screenUV + n.xz * distortScale, 0.001f, 0.999f);

        const float3 rtSample = gRtRefl.SampleLevel(gSampler, sampleUV, 0).rgb;
        const float rtLuma = dot(rtSample, float3(0.2126f, 0.7152f, 0.0722f));

        // 画面端フェード (サンプリング UV が画面境界に近付くほど IBL へ戻す)
        const float2 edgeDist = abs(sampleUV - 0.5f) * 2.0f;
        const float edgeFade = saturate((1.0f - max(edgeDist.x, edgeDist.y)) * 8.0f);

        // RT 反射の有効データがあるピクセルのみ採用、背景・空は IBL 反射を維持
        const float rtBlend = saturate(edgeFade * (rtLuma > 1e-4f ? 1.0f : 0.0f));
        reflColor = lerp(skyReflection, rtSample, rtBlend);
    }

    // フレネルおよび滑らかさ (smoothness) に応じた反射像のブレンド (見下ろし角度でも 40% 反射、浅い角度で 100% 鏡面反射)
    const float reflWeight = saturate(fresnel * 1.5f + (1.0f - roughness) * 0.35f);
    litColor = lerp(litColor, reflColor, reflWeight);

    // カメラ距離
    const float viewDist = length(i.posW - gCameraPos);

    // 波頭の白波 (Foam) 合成 (遠方のサブピクセル・エイリアシング抑制)
    const float foamStrength = gWaterSettings.w;
    if (foamStrength > 0.01f) {
        // 遠方では Foam を自然に減衰させて砂嵐ノイズを防止
        const float foamDistanceFade = saturate(1.0f - (viewDist - 15.0f) / 60.0f);
        const float effectiveFoam = foamStrength * foamDistanceFade;

        const float foamThreshold = 0.75f;
        const float foam = saturate((i.waveCrest - foamThreshold) / (1.0f - foamThreshold)) * effectiveFoam;
        const float3 foamColor = float3(1.0f, 1.0f, 1.0f);
        litColor = lerp(litColor, foamColor, foam);
        waterColor.a = max(waterColor.a, foam * 0.9f);
    }

    // フォグ
    litColor = ApplyFog(litColor, viewDist, i.posW.y, gFogMode, gFogColor, gFogStart, gFogEnd,
                        gFogDensity, gFogHeightFalloff, gFogBaseHeight, gSunDirection, gSunColor,
                        gFogInscatterIntensity, gFogInscatterPower);

    // メッシュ外周のソフトエッジ (正方形プレーンの境界が空中に露出するのを防ぐ)
    const float2 uvDist = abs(i.uv - 0.5f) * 2.0f;
    const float meshEdgeFade = saturate((1.0f - max(uvDist.x, uvDist.y)) * 12.0f);

    const float finalAlpha = saturate(waterColor.a + reflWeight * 0.6f) * meshEdgeFade;
    return float4(litColor, finalAlpha);
}
