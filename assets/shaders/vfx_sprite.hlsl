// VFX (M29c): Sprite / Trail / TextMesh のワールド空間クアッド描画。
// 頂点は CPU 構築済みのワールド座標。色 * テクスチャを出力 (アルファブレンド、深度書き込み無し)。
// Sprite は画像 or 白、Trail は白、TextMesh はフォントアトラス (rgb=1, a=カバレッジ) をバインド。
// M32c: シーンフォグを距離ベースで適用 (アルファブレンドなのでフォグ色へ lerp)。

cbuffer VfxCB : register(b0)
{
    float4x4 gViewProj; // transpose(view*proj) — 既存の「転置してアップロード」規約
    float3   gCameraPos;
    int      gFogMode;    // -1=off / 0=linear 1=exp 2=exp2
    float3   gFogColor;
    float    gFogDensity;
    float    gFogStart;
    float    gFogEnd;
    float2   _pad;
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
    float  dist : TEXCOORD1; // カメラからのワールド距離 (フォグ用)
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), gViewProj);
    o.uv = i.uv;
    o.color = i.color;
    o.dist = length(i.pos - gCameraPos);
    return o;
}

Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);

float4 PSMain(VSOut i) : SV_TARGET
{
    float4 col = i.color * gTex.Sample(gSamp, i.uv);
    if (gFogMode >= 0)
    {
        float f;
        if (gFogMode == 0) { f = saturate((i.dist - gFogStart) / max(gFogEnd - gFogStart, 0.001)); }
        else if (gFogMode == 1) { f = 1.0 - exp(-gFogDensity * i.dist); }
        else { const float e = gFogDensity * i.dist; f = 1.0 - exp(-e * e); }
        col.rgb = lerp(col.rgb, gFogColor, f); // アルファブレンドなのでフォグ色へ補間
    }
    return col;
}
