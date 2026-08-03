// M46g: 影の空間フィルタ (SVGF のスカラー軽量版)。可視率という 1 チャンネル量に
// B3 スプラインを法線 + カメラ距離のエッジ停止付きで掛ける。
// GI (rt_atrous.cs.hlsl) と違い分散も履歴も持たない — 太陽コーンが狭く、
// 1spp のノイズが半影の数画素に限られるため 1 反復で足りる。
// 副作用として影の輪郭がアンチエイリアスされる (1spp の二値エッジが階調になる)。
//
// **分離型** (水平 5 タップ → 垂直 5 タップ) にしてある。エッジ停止重みは厳密には
// 分離できないが、スカラー量では差が視認できず、タップ数が 25 → 10 に落ちる
// (フル解像度で撃つので、ここのタップ数がそのまま実測 ms に効く)。
// ジオメトリ判定は G-Buffer ワールド座標の w (幾何あり = 1 / クリア = 0) を使う
// = アルベドを別に読まずに済む。

// C++ の kRtAtrousRadius と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_ATROUS_RADIUS 2

cbuffer RtShadowFilterCB : register(b2)
{
    float2 gShFSize;       // フル解像度
    int gShFStep;          // タップの刻み幅 (1, 2, ...)
    float gShFSigmaDepth;  // タップ 1 画素あたりの相対深度許容 (RtTypes.h が出所)
    float3 gShFCameraPos;  // 深度 = カメラからの距離を作るのに使う
    float gShFSigmaNormal; // cos の指数
    int2 gShFAxis;         // タップ方向 (1,0) = 水平 / (0,1) = 垂直
    float2 gShFPad;
};

Texture2D gShFIn : register(t0);       // r = 可視率
Texture2D gShFNormal : register(t1);   // GBuffer 法線
Texture2D gShFPosition : register(t2); // GBuffer ワールド座標 (w = ジオメトリ有りマーク)

RWTexture2D<float> gShFOut : register(u0);

// ---- rt_atrous.cs.hlsl / RtMath.h と同一式 (変更時は 3 箇所とも更新) ----

float RtAtrousDepthWeight(float zc, float zq, float tapDist, float sigma)
{
    const float tol = sigma * max(zc, 1e-3f) * max(tapDist, 1.0f);
    return exp(-abs(zc - zq) / max(tol, 1e-6f));
}

float RtAtrousNormalWeight(float3 nc, float3 nq, float power)
{
    return pow(max(dot(nc, nq), 0.0f), power);
}

float RtAtrousKernel(int d)
{
    const float k[3] = { 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
    return k[min(abs(d), 2)];
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gShFSize.x || tid.y >= (uint)gShFSize.y) {
        return;
    }
    const int2 c = int2(tid.xy);
    const float center = gShFIn.Load(int3(c, 0)).r;
    const float4 pc = gShFPosition.Load(int3(c, 0));
    if (pc.w <= 0.0f) {
        gShFOut[tid.xy] = center; // ジオメトリ無し (空) は素通し
        return;
    }
    const float3 nc = normalize(gShFNormal.Load(int3(c, 0)).xyz * 2.0f - 1.0f);
    const float zc = length(pc.xyz - gShFCameraPos);
    const int step = max(gShFStep, 1);

    float sum = 0.0f;
    float sumW = 0.0f;
    [unroll] for (int d = -MYE_RT_ATROUS_RADIUS; d <= MYE_RT_ATROUS_RADIUS; ++d) {
        const int2 p = c + gShFAxis * (d * step);
        if (any(p < 0) || any(p >= (int2)gShFSize)) {
            continue;
        }
        const float4 pq = gShFPosition.Load(int3(p, 0));
        if (pq.w <= 0.0f) {
            continue; // 空 = 別の面
        }
        const float3 nq = normalize(gShFNormal.Load(int3(p, 0)).xyz * 2.0f - 1.0f);
        const float zq = length(pq.xyz - gShFCameraPos);
        const float tapDist = abs((float)(d * step));
        float w = RtAtrousKernel(d);
        w *= RtAtrousDepthWeight(zc, zq, tapDist, gShFSigmaDepth);
        w *= RtAtrousNormalWeight(nc, nq, gShFSigmaNormal);
        sum += w * gShFIn.Load(int3(p, 0)).r;
        sumW += w;
    }
    gShFOut[tid.xy] = (sumW > 1e-6f) ? (sum / sumW) : center;
}
