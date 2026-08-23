// M57c: フロクセルのテンポラル蓄積 (再投影 + 履歴の混合)。
//
// なぜ要るのか: 注入 (M57b) は 1 セル 1 サンプルしか撃たない。スライス深度をフレーム毎に
// ジッタさせると、静止画では「1 サンプルぶんのノイズ」だが、フレームを跨いで混ぜれば
// スライス方向に多重サンプルしたのと同じになる。**ジッタと履歴は必ずセット**で、
// 片方だけ入れると霧が毎フレーム奥行き方向に脈打つだけになる (TAA とジッタの関係と同じ)。
//
// 1 スレッド = 1 セル。今フレームのセル中心をワールドへ戻し、**前フレームの** viewProj で
// 投影し直して前フレームのグリッド座標を出し、そこを線形サンプルして混ぜる。
//   ・再投影に使う行列は **非ジッタ (projNoJitter) 由来** (M55b の規約)。ジッタ込みだと
//     静止したカメラでも毎フレーム半ピクセルぶん履歴がずれる。
//   ・グリッドの外へ落ちたセルは履歴を捨てる (feedback 0 = 今フレームそのまま)。
//     クランプで最遠スライスへ丸めると、カメラが振れた瞬間に画面端の霧が奥の値で汚れる。
//
// ★出力は入力と**別のテクスチャ** (ping-pong)。RWTexture3D の typed ロードは
//   R32 系限定なので、そもそも同じテクスチャを読み書きできない (M57a の制約メモ)。
#include "froxel_common.hlsli"

// C++ の FroxelPostCB (src\Engine\Renderer\FroxelPass.cpp) とレイアウト一致 (176 バイト)。
// froxel_integrate.cs.hlsl と**同じ CB** を共有する (どちらもグリッドの幾何しか要らず、
// 2 本に分けるとアップロードが 2 回になるだけで得が無い)
cbuffer FroxelPostCB : register(b0)
{
    float4x4 gFroxelInvView;      // transpose(inverse(view)): view → ワールド (行ベクトル規約)
    float4x4 gFroxelPrevViewProj; // transpose(前フレームの view * projNoJitter)
    uint3 gFroxelGridSize;
    uint gFroxelHistValid; // 0 = 履歴なし (初フレーム / リサイズ / 通番が飛んだ)
    float gFroxelNearZ;
    float gFroxelFarZ;
    float gFroxelInvProj00;
    float gFroxelInvProj11;
    float gFroxelSliceJitter; // 注入で使ったのと**同じ**オフセット (代表点を合わせる)
    float gFroxelFeedback;    // 履歴の残し率 [0, 0.95]
    float2 gFroxelPostPad;
};

Texture3D<float4> gFroxelCurrent : register(t0); // 今フレームの注入結果
Texture3D<float4> gFroxelHistory : register(t1); // 前フレームのテンポラル出力
SamplerState gFroxelLinear : register(s0);       // LINEAR / CLAMP
RWTexture3D<float4> gFroxelOut : register(u0);

[numthreads(MYE_FROXEL_GROUP, MYE_FROXEL_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= gFroxelGridSize)) {
        return;
    }
    const float4 cur = gFroxelCurrent[id];
    // 履歴が無いフレームは**今フレームをそのまま複製する**。ここで lerp(cur, cur, f) を
    // 通すと丸めで最下位ビットが動き、「テンポラル 1 フレーム目は注入とビット一致」
    // という M57c の検査 (--froxel-dump) が成立しなくなる
    if (gFroxelHistValid == 0 || gFroxelFeedback <= 0.0f) {
        gFroxelOut[id] = cur;
        return;
    }

    // ---- 今フレームのセル中心 → ワールド ----
    const float sliceCount = (float)gFroxelGridSize.z;
    const float viewZ =
        FroxelSliceDepth((float)id.z + gFroxelSliceJitter, sliceCount, gFroxelNearZ, gFroxelFarZ);
    const float2 uv = ((float2)id.xy + 0.5f) / (float2)gFroxelGridSize.xy;
    const float3 viewPos = FroxelViewPos(uv, viewZ, gFroxelInvProj00, gFroxelInvProj11);
    const float3 posW = mul(float4(viewPos, 1.0f), gFroxelInvView).xyz;

    // ---- 前フレームのグリッド座標 ----
    const float4 prevClip = mul(float4(posW, 1.0f), gFroxelPrevViewProj);
    if (prevClip.w <= 1e-4f) {
        gFroxelOut[id] = cur; // 前フレームはカメラの背面 = 履歴なし
        return;
    }
    const float2 prevNdc = prevClip.xy / prevClip.w;
    const float2 prevUv = float2(prevNdc.x * 0.5f + 0.5f, 0.5f - prevNdc.y * 0.5f);
    const float prevSlice =
        FroxelDepthToSlice(prevClip.w, sliceCount, gFroxelNearZ, gFroxelFarZ);
    // グリッドの外は履歴を捨てる。**クランプしない** — 最遠スライスへ丸めると、
    // カメラが振れた瞬間に画面端の霧が「奥の値」で塗られて尾を引く
    if (any(prevUv < 0.0f) || any(prevUv > 1.0f) || prevSlice < 0.0f || prevSlice > sliceCount) {
        gFroxelOut[id] = cur;
        return;
    }
    // 履歴テクスチャは注入と同じ格納規約 (テクセル z の中心 = スライス z の代表点) なので
    // w は素直に prevSlice / count。積分結果の半テクセルずらし (IntegratedSampleW) とは別物
    const float3 uvw = float3(prevUv, prevSlice / sliceCount);
    const float4 hist = gFroxelHistory.SampleLevel(gFroxelLinear, uvw, 0.0f);

    // C++ の froxel::TemporalBlend と同じ展開順 (cur + (hist - cur) * f)
    gFroxelOut[id] = lerp(cur, hist, gFroxelFeedback);
}
