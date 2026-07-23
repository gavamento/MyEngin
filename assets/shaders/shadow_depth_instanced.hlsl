// シャドウマップ深度パスのインスタンス版 (M38f)。
// 非インスタンス版と違い gMVP には transpose(lightViewProj) 単体が入り (world を含まない)、
// ワールド行列は StructuredBuffer + SV_InstanceID で引く。

cbuffer ShadowObject : register(b0)
{
    float4x4 gMVP; // ここでは transpose(lightViewProj) 単体
    // ---- インスタンシング (M38f、末尾 append) ----
    int      gInstanceBase; // gInstances 内の run 開始位置
    float3   _instPad;
};

// CPU 側は XMFLOAT4X4 (行優先) をそのまま書くため row_major で受ける
struct MeshInstance
{
    row_major float4x4 world;
};
StructuredBuffer<MeshInstance> gInstances : register(t0);

struct VSIn
{
    float3 pos    : POSITION;
    uint   instId : SV_InstanceID;
};

float4 VSMain(VSIn v) : SV_Position
{
    const float4 posW = mul(float4(v.pos, 1.0f), gInstances[gInstanceBase + v.instId].world);
    return mul(posW, gMVP);
}

// 深度のみ描画のため実際には bind しない (PSSetShader(nullptr))。コンパイル成立用の最小 PS。
float4 PSMain() : SV_Target
{
    return 0.0f;
}
