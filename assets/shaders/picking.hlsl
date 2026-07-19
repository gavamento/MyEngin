// エディタ ピッキング (M9): メッシュをエンティティ ID (uint) で R32_UINT ターゲットへ描く。
// 深度テスト有効なので、最前面のメッシュの ID が残る。ID=0 は「何も無し」。
// CB は forward_lit と同じ column_major (CPU 側で転置してアップロード) 規約。

cbuffer PerFrame : register(b0)
{
    float4x4 gViewProj;
};

cbuffer PerObject : register(b1)
{
    float4x4 gWorld;
    uint gId;
    uint3 gPad;
};

struct VSIn
{
    float3 pos : POSITION;
};

float4 VSMain(VSIn i) : SV_POSITION
{
    return mul(mul(float4(i.pos, 1.0), gWorld), gViewProj);
}

uint PSMain() : SV_TARGET
{
    return gId;
}
