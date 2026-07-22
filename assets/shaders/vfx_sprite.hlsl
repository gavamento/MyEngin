// VFX (M29c): Sprite / Trail / TextMesh のワールド空間クアッド描画。
// 頂点は CPU 構築済みのワールド座標。色 * テクスチャを出力 (アルファブレンド、深度書き込み無し)。
// Sprite は画像 or 白、Trail は白、TextMesh はフォントアトラス (rgb=1, a=カバレッジ) をバインド。

cbuffer VfxCB : register(b0)
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
    float2 uv : TEXCOORD;
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

Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    return i.color * gTex.Sample(gSamp, i.uv);
}
