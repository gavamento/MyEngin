// Forward パス標準ライティングのインスタンス版 (M38f)。
// ワールド行列を StructuredBuffer + SV_InstanceID で引く以外は forward_lit.hlsl と同一。
// PerObject の gWorld は未使用 (レイアウト互換のため残す)。gInstanceBase は末尾 append。

#include "common.hlsli"
// M57e: フロクセルのサンプル座標と合成 (register 宣言を持たないヘッダなので衝突しない)
#include "froxel_common.hlsli"
#include "acoustic_common.hlsli" // M65e: 残光の式の正本

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
    // ---- M54e: 局所ライトのシャドウアトラス (末尾 append)。forward_lit.hlsl と同一 ----
    int      gShadowAtlasEnabled;
    float    gShadowAtlasTexel; // 1/アトラス解像度
    float2   _atlasPad;
    ShadowTile gShadowTiles[MYE_MAX_SHADOW_TILES];
    // ---- M57e: フロクセル・ボリュメトリック (末尾 append)。forward_lit.hlsl と同一 ----
    int      gFroxelEnabled;
    float    gFroxelNearZ;
    float    gFroxelFarZ;
    float    gFroxelSlices;
    float4   gFroxelViewZRow;
    float2   gFroxelScreenSize;
    float2   _froxelPad;
    // ---- M65e: 音響の残光ボリューム (末尾 append)。
    //      **w = 0 で従来と完全に同一の式**。★形は DeferredPath / ForwardPath の
    //      PerFrameCB と共有 (RenderTypes.h の AcousticCB) — 片方だけ足すと
    //      Deferred の透明後段だけがゴミを読む (M54e の轍) ----
    float4   gAcousticGridMin; // xyz = セル(0,0,0) の最小角のワールド座標
    float4   gAcousticInvSize; // xyz = 1/(dim*cellSize)
    float4   gAcousticParams;  // x=強さ y=法線押し出し[m] z=予約 w=有効
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld; // インスタンス版では未使用 (レイアウト互換)
    float4   gBaseColor;
    // ---- インスタンシング (M38f、末尾 append) ----
    int      gInstanceBase; // gInstances 内の run 開始位置
    float3   _instPad;
};

cbuffer MaterialParams : register(b2)
{
    float  gMetallic;
    float  gRoughness;
    int    gHasNormal; // 0=ノーマルマップ無し (幾何法線をそのまま使う)
    float  gEmissive; // M46i: 自己発光の強さ (0 = 発光なし)
};

// CPU 側は XMFLOAT4X4 (行優先) をそのまま書くため row_major で受ける
struct MeshInstance
{
    row_major float4x4 world;
};
StructuredBuffer<MeshInstance> gInstances : register(t0); // VS 側 (PS の t0 とは独立)

Texture2D                gAlbedo        : register(t0);
Texture2DArray           gShadowMap     : register(t1); // M38d: CSM カスケード配列
Texture2D                gNormalTex     : register(t2);
TextureCube              gIblIrradiance : register(t3); // M38c
TextureCube              gIblPrefiltered: register(t4);
Texture2D                gIblBrdfLut    : register(t5);
Texture2D                gShadowAtlas   : register(t6); // M54e (局所ライトの深度アトラス)
Texture3D                gFroxelVolume  : register(t7); // M57e (rgb=積算内向き散乱 / a=透過率)
// M65e: 残光ボリューム。番号の正本は acoustic_common.hlsli の MYE_ACOUSTIC_FWD_SRV_SLOT。
// ★張る側 (ForwardPath / DeferredPath の透明後段) の本数を 7 -> 8 にすること。
//   **null を張り直す側も 8**。剥がし忘れると次フレームまで生き残る (M57e の罠)
Texture3D                gAcousticGlow  : MYE_ACOUSTIC_REG(MYE_ACOUSTIC_FWD_SRV_SLOT);
SamplerState             gSampler       : register(s0);
SamplerComparisonState   gShadowSampler : register(s1);
SamplerState             gIblSampler    : register(s2); // LINEAR/CLAMP (M38c)

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
    uint   instId : SV_InstanceID;
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
    const float4x4 world = gInstances[gInstanceBase + v.instId].world;
    const float4 posW = mul(float4(v.pos, 1.0f), world);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)world));
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
    // M54e: 局所ライトの影 (Deferred と同一の関数 = 両経路の絵が一致する)
    float localShadow[MAX_LIGHTS];
    ResolveLocalShadows(gShadowAtlas, gShadowSampler, gShadowTiles, gLights, gLightCount,
                        gShadowAtlasEnabled, i.posW, gShadowAtlasTexel, localShadow);
    float3 color = ApplyLighting(albedo.rgb, n, i.posW, gCameraPos, gMetallic, gRoughness,
                                 gAmbient, gLights, gLightCount, dirShadow, localShadow,
                                 gIblEnabled, gIblSpecMips, gIblIrradiance, gIblPrefiltered,
                                 gIblBrdfLut, gIblSampler, 1.0f); // SSAO は Deferred のみ
    // M46i: 自己発光。ライティングに依らず放射する分を足す (フォグより前 =
    // 遠くの発光もフォグに減衰される)。gEmissive=0 なら加算項がちょうど 0
    color += albedo.rgb * gEmissive;
    // ---- M65e: 音響の残光 (gAcousticParams.w == 0 で従来とビット恒等) ----
    // ★足すのは**フォグより前**。残光は面から出ていく放射なので霧が掛かる側に居るのが正しい。
    // ★サンプラは s2 (IBL 用 LINEAR/CLAMP) を流用 = サンプラは 1 つも増えない
    if (gAcousticParams.w != 0.0f) {
        const float glow = AcousticSample(gAcousticGlow, gIblSampler, i.posW, n,
                                          gAcousticGridMin.xyz, gAcousticInvSize.xyz,
                                          gAcousticParams.y);
        color += AcousticRadiance(glow, gAcousticParams.x);
    }
    // 大気散乱 (M29d + M43a、M57e でフロクセルと分担)。forward_lit.hlsl と同一の分岐
    if (gFroxelEnabled != 0) {
        const float fviewZ = dot(float4(i.posW, 1.0f), gFroxelViewZRow);
        color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                         FroxelFogOrigin(gCameraPos, i.posW, fviewZ, gFroxelFarZ), i.posW,
                         gFogHeightFalloff, gFogBaseHeight, gSunDirection, gSunColor,
                         gFogInscatterIntensity, gFogInscatterPower);
        color = FroxelComposite(gFroxelVolume, gIblSampler, i.pos.xy, gFroxelScreenSize, fviewZ,
                                gFroxelSlices, gFroxelNearZ, gFroxelFarZ, color);
    } else {
        color = ApplyFog(color, gFogColor, gFogMode, gFogDensity, gFogStart, gFogEnd,
                         gCameraPos, i.posW, gFogHeightFalloff, gFogBaseHeight, gSunDirection,
                         gSunColor, gFogInscatterIntensity, gFogInscatterPower); // M29d+M43a
    }
    return float4(color, albedo.a);
}
