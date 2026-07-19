// エディタ補助線描画 (M9): グリッド / コライダーワイヤ / 選択アウトライン / アイコン。
// SceneView の RT にのみ描く (backbuffer/リプレイ経路には出さない)。頂点カラーをそのまま出力。

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
};

struct VSIn
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), gViewProj);
    o.color = i.color;
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    return i.color;
}
