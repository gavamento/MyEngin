// M46g: RT 影。G-Buffer の可視点から太陽 (最初の平行光) へ影レイを 1 本撃ち、
// 可視率 (1 = 照らされる / 0 = 影) をフル解像度の R8 に書く。
// シャドウマップと違い「テクセル解像度」「深度バイアス」「カスケード分割」が無いので、
// アクネ / ピーターパン / カスケード境界の継ぎ目が原理的に発生しない。
//
// 太陽は点ではなく視半径 0.265° の円盤なので、レイ方向はその円錐内から 1 本選ぶ
// (= 距離に応じて半影が広がる)。残る 1spp のノイズは rt_shadow_filter.cs.hlsl で均す。

#include "rt_common.hlsli"

cbuffer RtShadowCB : register(b2)
{
    float2 gShSize;       // 出力解像度 (= G-Buffer と同じフル解像度)
    float gShCosThetaMax; // 太陽コーンの cos (1 = 点光源 = 完全に硬い影)
    uint gShFrameIndex;   // 乱数列をフレームでずらす (freeze 時は 0 固定)
    float3 gShCameraPos;
    float gShEpsMin; // 影レイ原点のオフセット (絶対下限)
    float gShEpsRel; // 同 (距離への比例係数。RtTypes.h が出所)
    float3 gShPad;
};

Texture2D gShNormal : register(t7);   // GBuffer 法線 (*0.5+0.5 のワールド法線)
Texture2D gShPosition : register(t8); // GBuffer ワールド座標
Texture2D gShMark : register(t9);     // GBuffer アルベド (a = ジオメトリ有りマーク)

RWTexture2D<float> gShOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gShSize.x || tid.y >= (uint)gShSize.y) {
        return;
    }
    const int3 gp = int3(int2(tid.xy), 0);
    if (gShMark.Load(gp).a < 0.5f) {
        gShOut[tid.xy] = 1.0f; // ジオメトリ無し (空) — ライトパスはこの画素を読まない
        return;
    }
    // 影を落とすのは最初の平行光だけ (ラスタ CSM と同じ規約。common.hlsli::ApplyLighting は
    // dirShadow を全ての平行光へ一律に掛けるので、置き換えても挙動が一致する)
    int sun = -1;
    for (int i = 0; i < gRtLightCount; ++i) {
        if (sun < 0 && gRtLights[i].type == 0) {
            sun = i;
        }
    }
    if (sun < 0) {
        gShOut[tid.xy] = 1.0f; // 平行光なし = 影を落とす光源が無い
        return;
    }
    const float3 N = normalize(gShNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gShPosition.Load(gp).xyz;
    const float3 toL = -gRtLights[sun].direction;
    if (dot(N, toL) <= 0.0f) {
        gShOut[tid.xy] = 0.0f; // 太陽に背を向けた面 (レイを撃つまでもなく影)
        return;
    }
    uint3 seed = uint3(tid.x, tid.y, gShFrameIndex * 16u + 7u); // GI と別の乱数列
    const float3 dir = RtSampleCone(toL, gShCosThetaMax, RtNextRand2(seed));
    // 原点の誤差は「ワールド座標の絶対値」と「カメラからの距離」の両方に比例して増える
    const float dist = max(length(P - gShCameraPos), length(P));
    const float eps = max(gShEpsMin, gShEpsRel * dist);
    gShOut[tid.xy] = RtTraceAnyHit(P + N * eps, dir, 1e16f) ? 0.0f : 1.0f;
}
