// Deferred ジオメトリパス: GBuffer (albedo + ワールド法線) へ出力

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    float    _pad0;
    float3   gLightDir;
    float    _pad1;
    float3   gLightColor;
    float    gLightIntensity;
    float3   gAmbient;
    float    _pad2;
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    float4   gBaseColor;
};

Texture2D    gAlbedoTex : register(t0);
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
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 posW = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    o.normalW = normalize(mul(v.normal, (float3x3)gWorld));
    o.uv = v.uv;
    return o;
}

struct PSOut
{
    float4 albedo : SV_Target0; // rgb = albedo, a = 1 (ジオメトリ有り)
    float4 normal : SV_Target1; // ワールド法線 *0.5+0.5 (R10G10B10A2)
};

PSOut PSMain(VSOut i)
{
    PSOut o;
    const float4 albedo = gAlbedoTex.Sample(gSampler, i.uv) * gBaseColor;
    o.albedo = float4(albedo.rgb, 1.0f);
    o.normal = float4(normalize(i.normalW) * 0.5f + 0.5f, 1.0f);
    return o;
}
