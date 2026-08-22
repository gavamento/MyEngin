// Deferred ジオメトリパスのインスタンス版 (M38f)。
// ワールド行列を StructuredBuffer + SV_InstanceID で引く以外は deferred_gbuffer.hlsl と同一。

#include "common.hlsli" // PerturbNormal (M17.3)

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    float    _pad0;
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
// M55c: 前フレームに実際に描いた world 行列。gInstances と**同じ添字**で引く
// (DeferredPath が runs_ の順序どおりに同じ長さで積むので base/instId がそのまま通る)
StructuredBuffer<MeshInstance> gPrevInstances : register(t1); // VS 側

// M55c: 画面速度 (GBuffer RT4) 用。詳細は deferred_gbuffer.hlsl と同じ。
// インスタンス版は gPrevWorld を使わない (行列は gPrevInstances から引く)
cbuffer VelocityParams : register(b4)
{
    float4x4 gPrevViewProj;  // 非ジッタ (RenderView::prevViewProj)
    float4x4 gPrevWorld;     // 未使用 (レイアウト互換のために置く)
    float2   gJitterNdc;
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
    uint   instId : SV_InstanceID;
};

struct VSOut
{
    float4 pos     : SV_Position;
    float3 normalW : NORMAL;
    float2 uv      : TEXCOORD0;
    float3 posW    : TEXCOORD1;
    float4 curClip  : TEXCOORD2; // M55c
    float4 prevClip : TEXCOORD3;
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
    o.curClip = o.pos;
    const float4x4 prevWorld = gPrevInstances[gInstanceBase + v.instId].world;
    o.prevClip = mul(mul(float4(v.pos, 1.0f), prevWorld), gPrevViewProj);
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
