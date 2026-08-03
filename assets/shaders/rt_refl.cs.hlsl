// M46h: RT 反射。G-Buffer の可視点から GGX の可視法線分布 (VNDF) に沿って反射レイを
// 1 本撃ち、その方向の入射放射輝度をそのまま出力する (GI と同じ demodulated 形式)。
//
// 出力の次元は IBL のプリフィルタ済み放射輝度 (split-sum 第 1 項) と揃えてあるので、
// 合成側では IBL の `pre` をこの値へ差し替えて (F0*brdf.x + brdf.y) を掛けるだけでよい。
// 両者が同じ次元なので roughness による混色 (RtReflWeight) が段差を作らない。
//
// roughness > gRfMaxRoughness ではレイを撃たない — GGX ローブが広がるほど 1spp の
// 分散が跳ね上がる一方、プリフィルタ IBL との見た目の差は縮むため。合成側が
// 同じしきい値でフォールバックする (撃たなかった画素の値は使われない)。

#include "rt_common.hlsli"

cbuffer RtReflCB : register(b2)
{
    float2 gRfOutSize; // 反射バッファの解像度 (内部解像度)
    float2 gRfGbSize;  // G-Buffer の解像度 (フル)
    float3 gRfCameraPos;
    float gRfTMax;
    uint gRfFrameIndex;    // フレーム毎に乱数列をずらす (テンポラル蓄積で平均される)
    int gRfBounces;
    float gRfMaxRoughness; // これを超えたら撃たない (RtTypes.h が出所)
    float gRfEpsMin;       // レイ原点のオフセット (絶対下限)
    float gRfEpsRel;       // 同 (距離への比例係数)
    float3 gRfPad;
};

Texture2D gRfNormal : register(t7);   // GBuffer 法線 (*0.5+0.5 のワールド法線)
Texture2D gRfPosition : register(t8); // GBuffer ワールド座標
Texture2D gRfMark : register(t9);     // GBuffer アルベド (a = ジオメトリ有りマーク)
Texture2D gRfMaterial : register(t10); // GBuffer マテリアル (r = metallic, g = roughness)

RWTexture2D<float4> gRfOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gRfOutSize.x || tid.y >= (uint)gRfOutSize.y) {
        return;
    }
    // 内部解像度のピクセル中心を G-Buffer の座標へ写す (rt_gi.cs.hlsl と同じ写像)
    const float2 uv = (float2(tid.xy) + 0.5f) / gRfOutSize;
    const int3 gp = int3(int2(uv * gRfGbSize), 0);
    if (gRfMark.Load(gp).a < 0.5f) {
        gRfOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f); // ジオメトリ無し (空)
        return;
    }
    const float roughness = gRfMaterial.Load(gp).g;
    if (roughness > gRfMaxRoughness) {
        gRfOut[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f); // 合成側で IBL へフォールバック
        return;
    }
    const float3 N = normalize(gRfNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gRfPosition.Load(gp).xyz;
    const float3 V = normalize(gRfCameraPos - P);

    // GGX VNDF で half vector を 1 本引き、視線をそれで反射させる。
    // alpha = roughness² (common.hlsli::DistributionGGX と同じ規約)
    uint3 seed = uint3(tid.x, tid.y, gRfFrameIndex * 16u + 11u); // GI/影と別の乱数列
    const float alpha = roughness * roughness;
    float3 L;
    if (dot(N, V) <= 1e-4f) {
        // シルエット際で補間法線が視線の裏へ回った画素。VNDF の前提 (ve.z > 0) を
        // 満たさないので鏡面方向で代用する
        L = reflect(-V, N);
    } else {
        L = reflect(-V, RtGgxVndf(N, V, alpha, RtNextRand2(seed)));
    }
    if (dot(L, N) <= 0.0f) {
        // ローブが面の下へ抜けた (粗い面 + 斜め視線)。棄却して黒を返すと 1spp では
        // 黒斑になるので鏡面方向へ丸める (v1 の近似)
        L = reflect(-V, N);
    }

    // 原点の誤差は「ワールド座標の絶対値」と「カメラからの距離」の両方に比例して増える
    // (rt_shadow.cs.hlsl と同一式)
    const float dist = max(length(P - gRfCameraPos), length(P));
    const float eps = max(gRfEpsMin, gRfEpsRel * dist);
    // ミス時のスカイは lod 0 — 鏡面反射に映る空をぼかさない (GI は粗い mip のまま)。
    // envOnLastHit = 1: 映り込んだ面もラスタと同じ明るさ (直接光 + 環境項) にする
    const float3 radiance =
        RtTraceRadianceLod(P + N * eps, L, gRfTMax, max(gRfBounces, 1), seed, 0.0f, 1.0f);
    gRfOut[tid.xy] = float4(radiance, 1.0f);
}
