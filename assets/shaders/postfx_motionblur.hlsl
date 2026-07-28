// M44d: カメラモーションブラー (深度再投影方式)。現フレームの UV+深度からワールド座標を
// 復元し、前フレームの viewProj で再投影した UV との差を速度ベクトルとして 8 タップ平均。
// カメラの動きのみが対象 (オブジェクト velocity は対象外 = v1 制限)。
// 再投影は PostFxMath.h::ReprojectUv とコメント同期 — 変更時は両方更新。
// CB は PostProcess.cpp の MotionBlurCB と同一レイアウト。

cbuffer MotionBlurCB : register(b0)
{
    float4x4 gInvViewProj;  // transpose(inverse(view*proj))
    float4x4 gPrevViewProj; // transpose(前フレームの view*proj)
    float    gIntensity;    // ブラー量スケール (0..1)
    float    gMaxPixels;    // 速度クランプ (px)
    float2   gScreenSize;   // フル解像度
};

Texture2D gScene : register(t0);        // 入力シーン (scene または DoF 後の sceneB)
Texture2D<float> gDepth : register(t1); // シーン深度 (フル解像度)
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

float4 PSMain(VSOut i) : SV_Target
{
    const int3 pixel = int3(int2(i.pos.xy), 0);
    float3 c = gScene.Load(pixel).rgb;
    const float d = gDepth.Load(pixel);

    // uv+depth → NDC → ワールド (透視除算) → 前フレームのクリップ
    const float2 ndc = float2(i.uv.x * 2.0f - 1.0f, 1.0f - i.uv.y * 2.0f);
    float4 world = mul(float4(ndc, d, 1.0f), gInvViewProj);
    world.xyz /= world.w; // 透視除算 (深度 [0,1] の範囲で w>0)
    world.w = 1.0f;
    const float4 prevClip = mul(world, gPrevViewProj);
    if (prevClip.w <= 1e-4f) {
        return float4(c, 1.0f); // 前フレームで背面 — ブラーしない
    }
    const float2 prevUv = float2(prevClip.x / prevClip.w * 0.5f + 0.5f,
                                 0.5f - prevClip.y / prevClip.w * 0.5f);
    float2 vel = (i.uv - prevUv) * gIntensity;
    const float lenPx = length(vel * gScreenSize);
    if (lenPx > gMaxPixels) {
        vel *= gMaxPixels / lenPx; // スミアの暴走防止
    }
    // 8 タップ平均 (中心 + 進行方向の後方 7 点)
    float3 acc = c;
    [unroll]
    for (int k = 1; k < 8; ++k) {
        acc += gScene.Sample(gLinear, i.uv - vel * ((float)k / 8.0f)).rgb;
    }
    return float4(acc / 8.0f, 1.0f);
}
