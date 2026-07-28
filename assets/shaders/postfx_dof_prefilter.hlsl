// M44c: DoF プリフィルタパス — HDR シーンを半解像度へ落としつつ、深度から
// 符号付き CoC (焦点面 0、手前 -1..奥 +1 に clamp) を α に書く。
// CoC 式は PostFxMath.h::SignedCoC とコメント同期 — 変更時は両方更新。
// CB は PostProcess.cpp の DofCB と同一レイアウト (3 パス共通)。

cbuffer DofCB : register(b0)
{
    float gFocusDist;  // 焦点距離 (ビュー空間 z)
    float gFocusRange; // 焦点面から CoC が 1 に達するまでの距離
    float gNearZ;      // 深度線形化用
    float gFarZ;
    float gMaxRadius;  // 最大ボケ半径 (フル解像度 px)
    float gTexelX;     // 出力ターゲットの 1/幅
    float gTexelY;     // 出力ターゲットの 1/高さ
    float _dofPad;
};

Texture2D gScene : register(t0);        // HDR シーンカラー (フル解像度)
Texture2D<float> gDepth : register(t1); // シーン深度 (フル解像度、R24_UNORM_X8)
SamplerState gLinear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

// 非線形深度 [0,1] -> ビュー空間 z (particle_render.hlsl と同式)
float LinearizeDepth(float d)
{
    return gNearZ * gFarZ / max(gFarZ - d * (gFarZ - gNearZ), 1e-4f);
}

float4 PSMain(VSOut i) : SV_Target
{
    // 色は linear sample がダウンサンプルを兼ねる。深度は最近傍 1 点 (中心) で十分
    const float3 c = gScene.Sample(gLinear, i.uv).rgb;
    const float2 full = float2(1.0f / gTexelX, 1.0f / gTexelY) * 2.0f; // フル解像度
    const float d = gDepth.Load(int3(int2(i.uv * full), 0));
    const float z = LinearizeDepth(d);
    const float coc = clamp((z - gFocusDist) / max(gFocusRange, 1e-4f), -1.0f, 1.0f);
    return float4(c, coc);
}
