// ブルーム bright-pass (M16): HDR シーンから輝度がしきい値を超えた分のみ抽出する。
// フルスクリーン三角形。半解像度ターゲットへ描くことでダウンサンプルも兼ねる (linear sample)。

cbuffer Bright : register(b0)
{
    float gThreshold; // 輝度しきい値 (これ未満は 0)
    float gPad0;
    float gPad1;
    float gPad2;
};

Texture2D gScene : register(t0);
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
    const float3 c = gScene.Sample(gLinear, i.uv).rgb;
    const float luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    // しきい値超過分を色相を保ったまま取り出す (ソフト)
    const float contrib = max(luma - gThreshold, 0.0f) / max(luma, 1e-4f);
    return float4(c * contrib, 1.0f);
}
