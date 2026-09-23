// M79: サーフェスシェーダーの読み込み失敗時フォールバック (組込みエラーシェーダ)。
// マゼンタ (1,0,1) 不透明・変位なしで描く。サーフェスと同じ予約 CB で描ける形にしてあるので、
// sub-02/sub-03 は他の *.surface.hlsl と同じ経路 (LoadSurface / 生成エントリ) でそのまま使える。
#include "MyEngineSurface.hlsli"

struct VSIn
{
    float3 pos : POSITION;
};

struct VSOut
{
    float4 pos : SV_Position;
};

VSOut VSMain(VSIn v)
{
    VSOut o;
    const float4 posW = mul(float4(v.pos, 1.0f), gWorld);
    o.pos = mul(posW, gViewProj);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return float4(1.0f, 0.0f, 1.0f, 1.0f);
}
