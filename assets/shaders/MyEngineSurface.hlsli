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
//
// ★影エントリ (CSM) では MyEnginePerFrame は全 0 で渡る (ShadowPass が光/霧/IBL の
//   確定より前に走るため)。VSMain の頂点変位に gCameraPos 等 MyEnginePerFrame の値を
//   使うと、影エントリだけ他パス (色・速度) と違う (常に 0 の) 値を見て変位の形が食い違う
//   (spec §4.1、M79 sub-03 round 2 で確定)。変位に使ってよいのは static (gTime/gWaterTime 等)・
//   MyEngineSurfaceFrame・MyEnginePerObject・MyEnginePerMaterial・作者 Texture2D のみ。

#include "common.hlsli"
#include "froxel_common.hlsli"
#include "acoustic_common.hlsli"
#include "ground_fog.hlsli" // M96: 地面の霧 (b6 / b7 / t32 / t33 / s8 を宣言する)

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

// かき乱しフィールド (M96) の 2D 層を位置 posW で読む (VS / PS どちらでも可、SampleLevel)。
// 戻り値: xy = 流れ (x, z) [m/s]、z = 濃さ ρ (最大の層)、w = 波紋の高さ h [m]。
// 層の格子の外・高さの範囲 (baseY ± thickness) の外・層が 0 枚 (場が無い) は全 0 = 草は真っ直ぐ。
// 縁の 2 マスで 0 へ落とす (境界で絵が割れないように)。複数の層は流れと波紋を足す。
// ★VS の位置に効く値として使ってよい (束縛は RenderSystem が描画の前に 1 回張る。影・速度エントリも同じ値を見る)
float4 MyeFieldSample(float3 posW)
{
    float4 result = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const uint count = min(gDfldHeader.x, (uint)MYE_DFLD_MAX_LAYERS);
    [loop] for (uint li = 0; li < count; ++li) {
        const DisturbanceLayer L = gDfldLayers[li];
        if (L.extent.w < 0.5f || abs(posW.y - L.originSize.w) > L.extent.z) {
            continue;
        }
        const float cellSize = L.originSize.z;
        const float gx = posW.x / cellSize - L.originSize.x;
        const float gz = posW.z / cellSize - L.originSize.y;
        const float nx = L.extent.x;
        const float nz = L.extent.y;
        if (!(gx > 0.0f && gz > 0.0f && gx < nx && gz < nz)) {
            continue;
        }
        const float edge = min(min(gx, nx - gx), min(gz, nz - gz));
        const float edgeFade = saturate(edge / MYE_DFLD_EDGE_FADE_CELLS);
        const float2 uvLo = float2(0.5f, 0.5f) * L.texScale.xy;
        const float2 uvHi = float2(nx - 0.5f, nz - 0.5f) * L.texScale.xy;
        const float2 uv = clamp(float2(gx, gz) * L.texScale.xy, uvLo, uvHi);
        const float4 flow = gDfldFlow.SampleLevel(gDfldSampler, float3(uv, (float)li), 0);
        const float2 aux = gDfldAux.SampleLevel(gDfldSampler, float3(uv, (float)li), 0);
        result.xy += flow.xy * edgeFade;
        result.z = max(result.z, flow.z * edgeFade);
        result.w += aux.x * edgeFade;
    }
    return result;
}

// 波紋チャネルの水平勾配 (dh/dx, dh/dz)。water_surface.hlsl::WaterRippleGradient と同じ式 (縁 2 マスで 0)。
// 水面の法線へ (-grad.x, 0, -grad.y) を足す。層が無い / 波紋が無いときは 0
float2 MyeFieldRippleGradient(float3 posW)
{
    float2 grad = float2(0.0f, 0.0f);
    const uint count = min(gDfldHeader.x, (uint)MYE_DFLD_MAX_LAYERS);
    [loop] for (uint li = 0; li < count; ++li) {
        const DisturbanceLayer L = gDfldLayers[li];
        if (L.extent.w < 0.5f || abs(posW.y - L.originSize.w) > L.extent.z) {
            continue;
        }
        const float cellSize = L.originSize.z;
        const float gx = posW.x / cellSize - L.originSize.x;
        const float gz = posW.z / cellSize - L.originSize.y;
        const float nx = L.extent.x;
        const float nz = L.extent.y;
        if (!(gx > 0.0f && gz > 0.0f && gx < nx && gz < nz)) {
            continue;
        }
        const float edge = min(min(gx, nx - gx), min(gz, nz - gz));
        const float edgeFade = saturate(edge / MYE_DFLD_EDGE_FADE_CELLS);
        const float2 uvLo = float2(0.5f, 0.5f) * L.texScale.xy;
        const float2 uvHi = float2(nx - 0.5f, nz - 0.5f) * L.texScale.xy;
        const float2 uv = float2(gx, gz) * L.texScale.xy;
        const float2 dx = float2(L.texScale.x, 0.0f);
        const float2 dz = float2(0.0f, L.texScale.y);
        const float hxp = gDfldAux.SampleLevel(gDfldSampler, float3(clamp(uv + dx, uvLo, uvHi), (float)li), 0).x;
        const float hxm = gDfldAux.SampleLevel(gDfldSampler, float3(clamp(uv - dx, uvLo, uvHi), (float)li), 0).x;
        const float hzp = gDfldAux.SampleLevel(gDfldSampler, float3(clamp(uv + dz, uvLo, uvHi), (float)li), 0).x;
        const float hzm = gDfldAux.SampleLevel(gDfldSampler, float3(clamp(uv - dz, uvLo, uvHi), (float)li), 0).x;
        grad += edgeFade * float2(hxp - hxm, hzp - hzm) / (2.0f * cellSize);
    }
    return grad;
}

// CSM の減衰 (1=影なし)
float MyeSunShadow(float3 posW)
{
    if (gShadowEnabled == 0) {
        return 1.0f;
    }
    return SampleShadowCSM(gShadowMap, gShadowSampler, gShadowVP, gShadowVP12[0], gShadowVP12[1],
                           (int)gCascadeInfo.w, posW, gShadowTexel);
}

// 距離フォグ + フロクセル (M79 sub-02)。受け持ちの分け方は forward_lit.hlsl / deferred_light.hlsl
// (M57d/e) と同一: グリッドの中はフロクセル、グリッドの奥は解析フォグ (起点を押し出して残り区間だけ)。
// gFroxelEnabled==0 なら下の 1 行 (ApplyFog のみ) に落ちる = フロクセル無しシーンとビット恒等。
// ★screenPos は SV_Position を引数で渡さず、posW を gViewProj で再投影して求める —
//   作者規約の関数シグネチャ (color, posW) を変えないための代償。全 PS エントリ (色・速度) は
//   PSMain を呼ぶ前に gViewProj へ今フレームの値を代入し直す (MyeEngineSurfaceEntries.hlsli の
//   MyePSColor/MyePSVelocity) ので、ここで読む gViewProj は常に今フレームの VP になる
float3 MyeApplyFog(float3 color, float3 posW)
{
    // clip.w <= 0 (カメラ背面/近平面付近) は解析フォグのみへ縮退する (ComputeVelocityUv /
    // SampleShadowCSM と同じ「w を割る前に符号を見る」規約。0 除算 / NaN を作らない)。
    // ★if の分岐それぞれで return せず 1 つの result に代入してから最後に 1 回だけ return する
    //   (fxc の初期化解析が早期 return の形だと誤検知の警告を出すため、形だけ変えて回避)
    const float4 clip = mul(float4(posW, 1.0f), gViewProj);
    // M96: 地面の霧。外側の大気 (下の解析フォグ / フロクセル) より先に掛ける。段のずらしに画素位置が要る
    const float2 gfogNdc = clip.xy / max(clip.w, 1e-5f);
    color = GroundFogComposite(color, posW,
                               float2(gfogNdc.x * 0.5f + 0.5f, 0.5f - gfogNdc.y * 0.5f) * gFroxelScreenSize);
    float3 result;
    if (gFroxelEnabled == 0 || clip.w <= 1e-5f) {
        result = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd, gCameraPos,
                          posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection, gSunColor,
                          gFogInscatterIntensity, gFogInscatterPower);
    } else {
        const float fviewZ = dot(float4(posW, 1.0f), gFroxelViewZRow);
        const float2 ndc = clip.xy / max(clip.w, 1e-5f);
        const float2 screenPos =
            float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f) * gFroxelScreenSize;
        const float3 c = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                                  FroxelFogOrigin(gCameraPos, posW, fviewZ, gFroxelFarZ), posW,
                                  gFogHeightFalloff, gFogBaseHeight, gSunDirection, gSunColor,
                                  gFogInscatterIntensity, gFogInscatterPower);
        result = FroxelComposite(gFroxelVolume, gIblSampler, screenPos, gFroxelScreenSize, fviewZ,
                                 gFroxelSlices, gFroxelNearZ, gFroxelFarZ, c);
    }
    return result;
}
