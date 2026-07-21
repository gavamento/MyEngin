// 分離ガウスブラー (M16 ブルーム): gTexelDir に沿った 9-tap。
// H (gTexelDir=(1/w,0)) と V (gTexelDir=(0,1/h)) を交互に適用する。

cbuffer Blur : register(b0)
{
    float2 gTexelDir; // 1 tap あたりの UV オフセット方向 (テクセルサイズ)
    float2 gPad;
};

Texture2D gTex : register(t0);
SamplerState gLinear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float w[5] = { 0.227027f, 0.1945946f, 0.1216216f, 0.054054f, 0.016216f };
    float3 c = gTex.Sample(gLinear, i.uv).rgb * w[0];
    [unroll] for (int k = 1; k < 5; ++k) {
        const float2 off = gTexelDir * (float)k;
        c += gTex.Sample(gLinear, i.uv + off).rgb * w[k];
        c += gTex.Sample(gLinear, i.uv - off).rgb * w[k];
    }
    return float4(c, 1.0f);
}
