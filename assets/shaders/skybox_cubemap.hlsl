// スカイボックス (M38b、cubemap)。skybox.hlsl (gradient) と同じフルスクリーン三角形 +
// invViewProj の視線復元で、TextureCube をワールド方向でサンプルする。
// CB は b3 / テクスチャは t0 / サンプラは s0 (SkyboxPass が専用にバインドする)。

cbuffer SkyCB : register(b3)
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float4 gTopColor;      // cubemap モードでは未使用 (レイアウト共有)
    float4 gHorizonColor;
    float4 gBottomColor;
};

TextureCube gSky : register(t0);
SamplerState gSampler : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 1.0f, 1.0f);
    o.ndc = corners[vid];
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    float4 pf = mul(float4(i.ndc, 1.0f, 1.0f), gInvViewProj);
    float4 pn = mul(float4(i.ndc, 0.0f, 1.0f), gInvViewProj);
    const float3 dir = normalize(pf.xyz / pf.w - pn.xyz / pn.w);
    return float4(gSky.Sample(gSampler, dir).rgb, 1.0f);
}
