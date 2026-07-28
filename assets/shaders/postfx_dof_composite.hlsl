// M44c: DoF 合成パス — シャープなフル解像度シーンと半解像度ボケを、深度から
// 再計算した CoC で lerp する (blend = saturate(|coc|))。CoC をフル解像度で
// 再計算するのは、半解像度 α のアップサンプルで焦点境界が滲むのを避けるため。
// 出力は sceneB (以降の bloom/トーンマップはこちらを読む)。
// v1 制限: 近景の前景滲み (シャープな背景への bleed) 非対応 / 半解像度境界のハロ。
// CB は PostProcess.cpp の DofCB と同一レイアウト。

cbuffer DofCB : register(b0)
{
    float gFocusDist;
    float gFocusRange;
    float gNearZ;
    float gFarZ;
    float gMaxRadius; // 最大ボケ半径 (フル解像度 px)
    float gTexelX;    // フル解像度の 1/幅
    float gTexelY;    // フル解像度の 1/高さ
    float _dofPad;
};

Texture2D gScene : register(t0);        // シャープな HDR シーン (フル解像度)
Texture2D gBlur : register(t1);         // ギャザー済みボケ (半解像度)
Texture2D<float> gDepth : register(t2); // シーン深度 (フル解像度)
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

float LinearizeDepth(float d)
{
    return gNearZ * gFarZ / max(gFarZ - d * (gFarZ - gNearZ), 1e-4f);
}

float4 PSMain(VSOut i) : SV_Target
{
    const int3 pixel = int3(int2(i.pos.xy), 0);
    const float3 sharp = gScene.Load(pixel).rgb;
    const float z = LinearizeDepth(gDepth.Load(pixel));
    const float coc = clamp((z - gFocusDist) / max(gFocusRange, 1e-4f), -1.0f, 1.0f);
    const float3 blur = gBlur.Sample(gLinear, i.uv).rgb;
    return float4(lerp(sharp, blur, saturate(abs(coc))), 1.0f);
}
