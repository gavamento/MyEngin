// M44d/M55e: モーションブラー。速度ベクトルの方向へ 8 タップ平均を取る。
//
// 速度の出所は 2 つあり、**画素ごとに**選ぶ (M55e):
//   ① GBuffer RT4 (画面速度、M55c) — その画素にジオメトリがある場合。
//      カメラの動きと**オブジェクト自身の動き**が合成済みなので、静止カメラでも
//      回転する物体がブレる。Deferred パスでしか存在しない。
//   ② 深度再投影 (M44d の v1 方式) — velocity が無い場合 (Forward パス) と、
//      **背景 / スカイ** (depth==1.0 = GBuffer を書いていないので RT4 は 0 のまま)。
//      ここを分けないと「カメラを振っても空だけ止まって見える」ことになる。
//
// 再投影と選択規則は PostFxMath.h::ReprojectUv / motionblur::BlurVector とコメント同期 —
// 変更時は両方更新 (RenderSelfTest の TestMotionBlurVelocity が CPU 側を検証)。
// velocity の規約は M55c と同じ **velocity = 今 UV − 前 UV** で、ジッタは書き込み側で
// 引き戻し済み — ここでは何も足し引きしない。
// CB は PostProcess.cpp の MotionBlurCB と同一レイアウト。

cbuffer MotionBlurCB : register(b0)
{
    float4x4 gInvViewProj;  // transpose(inverse(view*proj))
    float4x4 gPrevViewProj; // transpose(前フレームの view*proj)
    float    gIntensity;    // ブラー量スケール (0..1)
    float    gMaxPixels;    // 速度クランプ (px)
    float2   gScreenSize;   // フル解像度
    int      gUseVelocity;  // M55e: 1 = GBuffer RT4 を速度源に使う (Deferred のみ)
    float3   gMbPad;        // 16 バイト境界合わせ (CPU 側の pad と対)
};

Texture2D gScene : register(t0);            // 入力シーン (scene または DoF 後の sceneB)
Texture2D<float> gDepth : register(t1);     // シーン深度 (フル解像度)
Texture2D<float2> gVelocity : register(t2); // M55e: GBuffer RT4 (未使用時は null)
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

    float2 vel;
    if (gUseVelocity != 0 && d < 1.0f) {
        // ① ジオメトリ画素: 書き込み済みの画面速度をそのまま使う
        vel = gVelocity.Load(pixel);
    } else {
        // ② 背景 / Forward: uv+depth → NDC → ワールド (透視除算) → 前フレームのクリップ
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
        vel = i.uv - prevUv;
    }

    vel *= gIntensity;
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
