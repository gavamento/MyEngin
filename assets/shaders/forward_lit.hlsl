// Forward パス標準ライティング (平行光 + アンビエント)
// エントリ: VSMain / PSMain (ShaderManager の規約)

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
    float3   gCameraPos;
    float    _pad0;
    float3   gLightDir;      // 正規化済み・光の進行方向
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

Texture2D    gAlbedo  : register(t0);
SamplerState gSampler : register(s0);

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
    const float3 n = normalize(i.normalW);
    const float ndl = saturate(dot(n, -gLightDir));
    const float4 albedo = gAlbedo.Sample(gSampler, i.uv) * gBaseColor;
    const float3 color = albedo.rgb * (gAmbient + gLightColor * gLightIntensity * ndl);
    return float4(color, albedo.a);
}
