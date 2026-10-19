// Sprite2D (M94): 2D スプライトのワールド空間クアッド描画。頂点は CPU 構築済みのワールド座標。
// 色 * テクスチャを出力する (アルファブレンド、深度書き込み無し)。フォグ・ライトは掛けない。

cbuffer Sprite2DCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj) — 既存の「転置してアップロード」規約
};

struct VSIn
{
    float3 pos : POSITION; // ワールド座標
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), gViewProj);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

Texture2D    gTex  : register(t0);
SamplerState gSamp : register(s0); // バッチごとに LINEAR / POINT が入れ替わる

float4 PSMain(VSOut i) : SV_TARGET
{
    return i.color * gTex.Sample(gSamp, i.uv);
}
