// Deferred ジオメトリパス: GBuffer (albedo + ワールド法線 + ワールド座標) へ出力。
// ライティングは deferred_light パスで行うため、ここでは gViewProj のみ使う
// (CB 実体は Forward と同じ PerFrameCB。先頭の viewProj/cameraPos だけ読む)。

#include "common.hlsli" // PerturbNormal (M17.3)

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
    float  gEmissive; // M46i: 自己発光の強さ (0 = 発光なし)
};

// M55c: 画面速度 (GBuffer RT4) 用。b4 は GBuffer パス専用 — b0-b3 は PerFrame /
// PerObject / MaterialParams / BonePalette が使っており、Forward 系の CB には触らない。
// gPrevWorld は「**前フレームに実際に描いた** world」(前 tick ではない — 詳細は
// RenderSystem.h の PrevRenderWorldStore)。gVelocityValid==0 (初フレーム/リサイズ) は 0 を書く
cbuffer VelocityParams : register(b4)
{
    float4x4 gPrevViewProj;  // 非ジッタ (RenderView::prevViewProj)
    float4x4 gPrevWorld;     // インスタンス版では未使用 (StructuredBuffer から引く)
    float2   gJitterNdc;     // 今フレームの proj に載っているジッタ量
    int      gVelocityValid; // 0 = 履歴なし → velocity 0
    float    _velPad;
};

Texture2D    gAlbedoTex : register(t0);
Texture2D    gNormalTex : register(t1);
SamplerState gSampler   : register(s0);

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
    // M55c: SV_Position は PS ではピクセル座標に化けるので、クリップ座標の複製を渡す
    float4 curClip  : TEXCOORD2;
    float4 prevClip : TEXCOORD3;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 posW = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)gWorld));
    o.uv = v.uv;
    o.posW = posW.xyz;
    o.curClip = o.pos;
    o.prevClip = mul(mul(float4(v.pos, 1.0f), gPrevWorld), gPrevViewProj);
    return o;
}

struct PSOut
{
    float4 albedo   : SV_Target0; // rgb = albedo, a = 1 (ジオメトリ有り)
    float4 normal   : SV_Target1; // ワールド法線 *0.5+0.5 (R10G10B10A2)
    float4 position : SV_Target2; // ワールド座標 (R16G16B16A16_FLOAT)
    float4 material : SV_Target3; // r=metallic g=roughness b=emissive/MYE_EMISSIVE_MAX
    float2 velocity : SV_Target4; // M55c: 画面速度 UV (R16G16_FLOAT)
};

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
    o.material = float4(gMetallic, gRoughness, EncodeEmissive(gEmissive), 1.0f);
    o.velocity = (gVelocityValid != 0) ? ComputeVelocityUv(i.curClip, i.prevClip, gJitterNdc)
                                       : float2(0.0f, 0.0f);
    return o;
}
