// M43b: ゴッドレイの空マスクパス — 深度が far (>=0.9999) の「空」ピクセルだけ
// 太陽色を出力し、それ以外は黒 (遮蔽)。半解像度ターゲットへ描く。
// v1 制限: 遮蔽マスクは空のみ (発光体のレイ非対応)。
// CB は PostProcess.cpp の GodrayMaskCB と同一レイアウト。

cbuffer GodrayMask : register(b0)
{
    float2 gScreenSize;   // フル解像度 (深度 Load 用)
    float2 _gmPad;
    float3 gSunColorFade; // sunColor (リニア・強度込み) × intensity × 画面端フェード
    float  _gmPad2;
};

Texture2D<float> gDepth : register(t0); // シーン深度 (フル解像度、R24_UNORM_X8)

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
    const int2 p = int2(i.uv * gScreenSize);
    const float d = gDepth.Load(int3(p, 0));
    return (d >= 0.9999f) ? float4(gSunColorFade, 1.0f) : float4(0.0f, 0.0f, 0.0f, 1.0f);
}
