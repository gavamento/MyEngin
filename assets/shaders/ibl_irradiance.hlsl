// 拡散 irradiance マップのベイク (M38c)。法線方向 N の半球をコサイン重み付きで積分し、
// 「平均入射色」として出力する — エンジンの直接光は 1/PI を省く規約なので、
// irradiance も平均 (Σ L·cosθ·sinθ / Σ cosθ·sinθ) に正規化して整合させる。

#include "ibl_common.hlsli"

BakeVSOut VSMain(uint vid : SV_VertexID)
{
    return BakeVS(vid);
}

float4 PSMain(BakeVSOut i) : SV_Target
{
    const float3 N = BakeDir(i.ndc);
    const float3 up = (abs(N.z) < 0.999f) ? float3(0, 0, 1) : float3(1, 0, 0);
    const float3 tx = normalize(cross(up, N));
    const float3 ty = cross(N, tx);

    const uint kPhi = 32u;
    const uint kTheta = 8u;
    float3 sum = float3(0, 0, 0);
    float weight = 0.0f;
    for (uint p = 0u; p < kPhi; ++p) {
        const float phi = (float(p) + 0.5f) * (2.0f * IBL_PI / float(kPhi));
        for (uint t = 0u; t < kTheta; ++t) {
            const float theta = (float(t) + 0.5f) * (0.5f * IBL_PI / float(kTheta));
            const float sinT = sin(theta);
            const float cosT = cos(theta);
            const float3 dir = tx * (sinT * cos(phi)) + ty * (sinT * sin(phi)) + N * cosT;
            const float w = cosT * sinT;
            sum += SampleEnv(dir) * w;
            weight += w;
        }
    }
    return float4(sum / max(weight, 1e-4f), 1.0f);
}
