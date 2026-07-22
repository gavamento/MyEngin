// SSAO の 4x4 box ブラー (M38e)。ノイズ回転で散らした遮蔽を均す (半解像度のまま)。

cbuffer BlurCB : register(b0)
{
    float2 gTexel; // 1/halfW, 1/halfH
    float2 _pad;
};

Texture2D gSrc : register(t0);
SamplerState gLinearClamp : register(s0);

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
    float sum = 0.0f;
    [unroll] for (int y = -2; y < 2; ++y) {
        [unroll] for (int x = -2; x < 2; ++x) {
            sum += gSrc.SampleLevel(gLinearClamp, i.uv + float2(x + 0.5f, y + 0.5f) * gTexel, 0).r;
        }
    }
    return sum / 16.0f;
}
