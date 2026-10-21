// Sprite2D の影 (M94p): 影 RT (R8) へ、キャスターの辺をライトから押し出した四辺形を gParam.y (1) で、
// selfShadow == false のキャスターの多角形を 0 で書く。ブレンドはアルファ 1 の上書き。
// 頂点はライト形状と同じ並び (Light2DVertex) で、ワールド XY と Z は gParam.x (ライトの位置)。

cbuffer ShadowCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj)
    float4 gParam;      // x = ワールド Z, y = 書く値
};

struct VSIn
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float edge : TEXCOORD1;
};

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, gParam.x, 1.0), gViewProj);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    return float4(gParam.y, gParam.y, gParam.y, 1.0);
}
