// シャドウマップ深度パス (M17): ライト視点で不透明ジオメトリの深度のみを描く。
// gMVP = transpose(world * lightViewProj) (CPU 側で合成済み)。PS は深度専用のためダミー。

cbuffer ShadowObject : register(b0)
{
    float4x4 gMVP;
};

struct VSIn
{
    float3 pos : POSITION;
};

float4 VSMain(VSIn v) : SV_Position
{
    return mul(float4(v.pos, 1.0f), gMVP);
}

// 深度のみ描画のため実際には bind しない (PSSetShader(nullptr))。コンパイル成立用の最小 PS。
float4 PSMain() : SV_Target
{
    return 0.0f;
}
