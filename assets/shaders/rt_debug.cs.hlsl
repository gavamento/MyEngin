// M46b: BVH の検証用デバッグ表示。カメラからプライマリレイを 1 本撃ち、
// トラバーサルの訪問ノード数 / ヒット法線 / インスタンス ID を可視化する。
// 目的は (1) BLAS/TLAS 構築とレイ変換の正しさを目視で確かめること、
// (2) cs_5_0 ソフトウェアトラバーサルの実コストを測ること。

#include "rt_common.hlsli"

// 表示の番号 (C++ の rtdebug:: と同値。check_rules の規則 9 が照合する)。
// この CS に届くのは一次レイを撃つ表示だけ — 他の番号は RtPasses::RenderDebug が結果バッファを Blit する
#define MYE_RT_DEBUG_BVH_HEAT 1
#define MYE_RT_DEBUG_HIT_NORMAL 2
#define MYE_RT_DEBUG_PRIMARY_CLASS 13

cbuffer RtDebugCB : register(b2) // b0=RtSceneCB / b1=RtEnvCB は rt_common.hlsli が使う
{
    float4x4 gInvViewProj; // transpose(inverse(view*proj))
    float3 gCameraPos;
    float gTMax;
    float2 gScreenSize;
    int gDebugMode;  // MYE_RT_DEBUG_* (どれにも当たらなければインスタンス ID)
    float gHeatScale; // ヒートマップを 1.0 に飽和させる訪問ノード数
};

RWTexture2D<float4> gOut : register(u0);

// 青 → 緑 → 黄 → 赤
float3 HeatColor(float x)
{
    x = saturate(x);
    float3 c = lerp(float3(0.0f, 0.0f, 1.0f), float3(0.0f, 1.0f, 0.0f), saturate(x * 3.0f));
    c = lerp(c, float3(1.0f, 1.0f, 0.0f), saturate(x * 3.0f - 1.0f));
    c = lerp(c, float3(1.0f, 0.0f, 0.0f), saturate(x * 3.0f - 2.0f));
    return c;
}

// index → 見分けのつく色 (Knuth の乗算ハッシュ)
float3 IdColor(int id)
{
    const uint h = uint(id + 1) * 2654435761u;
    return float3(float((h >> 0) & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u))
        / 255.0f * 0.8f + 0.1f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gScreenSize.x || tid.y >= (uint)gScreenSize.y) {
        return;
    }
    // ピクセル中心 → far 平面の点 → レイ方向 (行ベクトル規約)
    const float2 uv = (float2(tid.xy) + 0.5f) / gScreenSize;
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 farH = mul(float4(ndc, 1.0f, 1.0f), gInvViewProj);
    const float3 farW = farH.xyz / farH.w;
    const float3 rd = normalize(farW - gCameraPos);

    RtHit hit;
    const bool got = RtTraceClosest(gCameraPos, rd, gTMax, hit);

    float3 col = float3(0.0f, 0.0f, 0.0f);
    if (gDebugMode == MYE_RT_DEBUG_BVH_HEAT) {
        col = HeatColor(float(hit.visited) / max(gHeatScale, 1.0f));
    } else if (got) {
        if (gDebugMode == MYE_RT_DEBUG_HIT_NORMAL) {
            col = RtHitNormal(hit) * 0.5f + 0.5f;
        } else if (gDebugMode == MYE_RT_DEBUG_PRIMARY_CLASS) {
            // M67: 一次ヒットの ReflectionClass。Material → RtInstance の配管が
            // 通っているかを絵で確かめる唯一の口 (反射像側は 14 = rt_blit.hlsl の mode 4)
            col = RtReflClassColor(RtHitReflectionClass(hit));
        } else {
            col = IdColor(hit.inst);
        }
    }
    gOut[tid.xy] = float4(col, 1.0f);
}
