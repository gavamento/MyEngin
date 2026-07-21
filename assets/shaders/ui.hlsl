// ゲーム内 UI (M21): スクリーン空間の色付き/テクスチャ付きクアッド + ビットマップテキスト。
// 頂点はピクセル座標 (左上原点)。gInvScreen で NDC へ変換。色 * テクスチャを出力。
// パネル/画像は白 or 画像テクスチャ、テキストはフォントアトラス (rgb=1, a=カバレッジ) をバインド。

cbuffer UICB : register(b0)
{
    float2 gInvScreen; // (1/width, 1/height)
    float2 gPad;
};

struct VSIn
{
    float2 pos : POSITION;   // ピクセル座標 (左上原点)
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
    // ピクセル → NDC (y は下向きなので反転)
    float2 ndc = float2(i.pos.x * gInvScreen.x * 2.0 - 1.0, 1.0 - i.pos.y * gInvScreen.y * 2.0);
    o.pos = float4(ndc, 0.0, 1.0);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    float4 t = gTex.Sample(gSamp, i.uv);
    return i.color * t; // 白テクスチャ→単色、フォント(rgb=1,a=cov)→color.rgb * (color.a*cov)
}
