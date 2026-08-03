// M46e: SVGF 第 3 段 — エッジ停止 A-Trous ウェーブレットフィルタ。
// 5x5 の B3 スプライン (1,4,6,4,1)/16 を刻み幅 1,2,4 と倍化しながら複数回掛けることで、
// タップ数を増やさずに広い範囲を均す。エッジ停止は深度 (カメラ距離) / 法線 / 輝度の 3 つ。
//
// 入力・出力とも rgb = GI 色 / a = 分散。分散は重みの 2 乗で畳んで (w² を掛けて sumW² で割る)
// 「均したぶんノイズが減った」ことを次の反復へ伝える = 反復ごとにエッジ停止が厳しくなる。
// 輝度重みに使う分散は 3x3 ガウシアンで前処理する (分散自体のノイズで重みが暴れないように)。

// C++ の kRtAtrousRadius と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_ATROUS_RADIUS 2

cbuffer RtAtrousCB : register(b2)
{
    float2 gAtrousSize;        // 内部解像度
    int gAtrousStep;           // タップの刻み幅 (1, 2, 4, ...)
    float gAtrousSigmaDepth;   // タップ 1 画素あたりの相対深度許容 (RtTypes.h が出所)
    float gAtrousSigmaNormal;  // cos の指数
    float gAtrousSigmaLuma;    // 推定標準偏差の何倍までを均すか
    float2 gAtrousPad;
};

Texture2D gAtrousIn : register(t0);   // rgb = GI 色, a = 分散
Texture2D gAtrousGeom : register(t1); // 法線 (xyz) + カメラ距離 (w、0 = ジオメトリ無し)

RWTexture2D<float4> gAtrousOut : register(u0);

// ---- RtMath.h と同一式 (変更時は両方更新。selftest が C++ 側を検証する) ----

float RtLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// 深度 (カメラ距離) の重み。真の深度勾配を持たないので「タップが遠いほど許容を広げる」
// 相対差で近似する (でないと斜めの床が刻み幅を上げた瞬間にぼけなくなる)
float RtAtrousDepthWeight(float zc, float zq, float tapDist, float sigma)
{
    const float tol = sigma * max(zc, 1e-3f) * max(tapDist, 1.0f);
    return exp(-abs(zc - zq) / max(tol, 1e-6f));
}

// 法線の重み (cos の冪)。裏向きは 0
float RtAtrousNormalWeight(float3 nc, float3 nq, float power)
{
    return pow(max(dot(nc, nq), 0.0f), power);
}

// 輝度の重み。推定標準偏差でスケールするので、ノイズが乗っている間は緩く
// (よくぼける)、収束すると厳しく (エッジが残る)
float RtAtrousLumaWeight(float lc, float lq, float variance, float sigma)
{
    return exp(-abs(lc - lq) / (sigma * sqrt(max(variance, 0.0f)) + 1e-4f));
}

// A-Trous の 1 次元カーネル (B3 スプライン)
float RtAtrousKernel(int d)
{
    const float k[3] = { 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
    return k[min(abs(d), 2)];
}

// 輝度重み用に分散を 3x3 ガウシアン (1,2,1)/4 で均す (刻み幅は掛けない = 常に隣接)
float PrefilterVariance(int2 c)
{
    const float w1[3] = { 0.25f, 0.5f, 0.25f };
    float sum = 0.0f, wsum = 0.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            const int2 p = c + int2(dx, dy);
            if (any(p < 0) || any(p >= (int2)gAtrousSize)) {
                continue;
            }
            if (gAtrousGeom.Load(int3(p, 0)).w <= 0.0f) {
                continue;
            }
            const float w = w1[dx + 1] * w1[dy + 1];
            sum += w * gAtrousIn.Load(int3(p, 0)).a;
            wsum += w;
        }
    }
    return (wsum > 0.0f) ? (sum / wsum) : 0.0f;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gAtrousSize.x || tid.y >= (uint)gAtrousSize.y) {
        return;
    }
    const int2 c = int2(tid.xy);
    const float4 center = gAtrousIn.Load(int3(c, 0));
    const float4 geom = gAtrousGeom.Load(int3(c, 0));
    if (geom.w <= 0.0f) {
        gAtrousOut[tid.xy] = center; // ジオメトリ無し (空) は素通し
        return;
    }
    const float lc = RtLuminance(center.rgb);
    const float varC = PrefilterVariance(c);
    const int step = max(gAtrousStep, 1);

    float3 sumColor = float3(0.0f, 0.0f, 0.0f);
    float sumVar = 0.0f;
    float sumW = 0.0f;
    [unroll] for (int dy = -MYE_RT_ATROUS_RADIUS; dy <= MYE_RT_ATROUS_RADIUS; ++dy) {
        [unroll] for (int dx = -MYE_RT_ATROUS_RADIUS; dx <= MYE_RT_ATROUS_RADIUS; ++dx) {
            const int2 p = c + int2(dx, dy) * step;
            if (any(p < 0) || any(p >= (int2)gAtrousSize)) {
                continue;
            }
            const float4 g = gAtrousGeom.Load(int3(p, 0));
            if (g.w <= 0.0f) {
                continue; // 空 = 別の面
            }
            const float4 s = gAtrousIn.Load(int3(p, 0));
            const float tapDist = length(float2(dx, dy) * (float)step);
            float w = RtAtrousKernel(dx) * RtAtrousKernel(dy);
            w *= RtAtrousDepthWeight(geom.w, g.w, tapDist, gAtrousSigmaDepth);
            w *= RtAtrousNormalWeight(geom.xyz, g.xyz, gAtrousSigmaNormal);
            w *= RtAtrousLumaWeight(lc, RtLuminance(s.rgb), varC, gAtrousSigmaLuma);
            sumColor += w * s.rgb;
            sumVar += w * w * s.a;
            sumW += w;
        }
    }
    if (sumW > 1e-6f) {
        gAtrousOut[tid.xy] = float4(sumColor / sumW, sumVar / (sumW * sumW));
    } else {
        gAtrousOut[tid.xy] = center;
    }
}
