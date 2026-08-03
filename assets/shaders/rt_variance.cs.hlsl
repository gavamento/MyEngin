// M46e: SVGF 第 2 段 — 分散推定。テンポラル蓄積 (rt_temporal) が積んだ輝度モーメント
// (μ, μ²) から「今の推定値がどれだけノイズを含んでいるか」を出し、A-Trous の
// 輝度エッジ停止に渡す。出力は rgb = 蓄積色そのまま / a = 分散。
//
// 履歴が浅い (< gVarHistoryMin) と μ,μ² が信用できないので 5x5 の空間推定に落とす。
// 空間タップは深度 (カメラ距離の相対差) と法線で棄却する = 別の面を巻き込まない。
//
// シード凍結 (--rt-freeze-seed / スクリーンショット既定) では毎フレーム同じ 1 サンプルを
// 積むだけなのでテンポラル分散が 0 に潰れる。gVarForceSpatial=1 で空間推定を強制し、
// 実効サンプル数 1 として扱う (凍結画でも A-Trous が効く)。

// C++ の kRtAtrousRadius と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_ATROUS_RADIUS 2

cbuffer RtVarianceCB : register(b2)
{
    float2 gVarSize;          // 内部解像度 (GI バッファのサイズ)
    float gVarHistoryMin;     // これ未満の履歴長は空間推定へ (RtTypes.h が出所)
    int gVarForceSpatial;     // 1 = 常に空間推定 + 履歴長で割らない (シード凍結時)
    float gVarDepthThreshold; // 空間タップの棄却しきい値 (カメラ距離の相対差)
    float gVarNormalThreshold; // 同 (法線 cos の下限)
    float2 gVarPad;
};

Texture2D gVarColor : register(t0);   // 蓄積 GI (rgb) + 履歴長 (a)
Texture2D gVarMoments : register(t1); // 輝度モーメント (x = μ, y = μ²)
Texture2D gVarGeom : register(t2);    // 法線 (xyz) + カメラ距離 (w、0 = ジオメトリ無し)

RWTexture2D<float4> gVarOut : register(u0); // rgb = 色, a = 分散

// ---- RtMath.h と同一式 (変更時は両方更新。selftest が C++ 側を検証する) ----

float RtLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

// 1 サンプルあたりの分散。丸めで負に落ちるので 0 で止める
float RtVarianceFromMoments(float m1, float m2)
{
    return max(m2 - m1 * m1, 0.0f);
}

// サンプル分散 → 蓄積後の推定値の分散 (N 個の平均の分散は 1/N)。
// 凍結時は実効サンプル数 1 なので割らない
float RtVarianceEstimate(float sampleVar, float histLen, bool forceSpatial)
{
    return forceSpatial ? sampleVar : (sampleVar / max(histLen, 1.0f));
}

// 空間タップが同じ面かどうか (rt_temporal の再投影判定と同じ考え方)
bool RtVarianceTapValid(float zc, float zq, float3 nc, float3 nq)
{
    return (zq > 0.0f) && (abs(zc - zq) <= gVarDepthThreshold * max(zc, 1e-3f))
           && (dot(nc, nq) >= gVarNormalThreshold);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gVarSize.x || tid.y >= (uint)gVarSize.y) {
        return;
    }
    const int3 cp = int3(tid.xy, 0);
    const float4 color = gVarColor.Load(cp);
    const float4 geom = gVarGeom.Load(cp);
    if (geom.w <= 0.0f) {
        gVarOut[tid.xy] = float4(color.rgb, 0.0f); // ジオメトリ無し (空) はフィルタしない
        return;
    }

    const float histLen = max(color.a, 1.0f);
    const bool forceSpatial = (gVarForceSpatial != 0);
    float sampleVar;
    if (!forceSpatial && histLen >= gVarHistoryMin) {
        const float2 m = gVarMoments.Load(cp).xy;
        sampleVar = RtVarianceFromMoments(m.x, m.y);
    } else {
        // 5x5 の輝度モーメントをその場で取る (同じ面のタップだけ)
        float sum = 0.0f, sum2 = 0.0f, count = 0.0f;
        [unroll] for (int dy = -MYE_RT_ATROUS_RADIUS; dy <= MYE_RT_ATROUS_RADIUS; ++dy) {
            [unroll] for (int dx = -MYE_RT_ATROUS_RADIUS; dx <= MYE_RT_ATROUS_RADIUS; ++dx) {
                const int2 p = int2(tid.xy) + int2(dx, dy);
                if (any(p < 0) || any(p >= (int2)gVarSize)) {
                    continue;
                }
                const float4 g = gVarGeom.Load(int3(p, 0));
                if (!RtVarianceTapValid(geom.w, g.w, geom.xyz, g.xyz)) {
                    continue;
                }
                const float l = RtLuminance(gVarColor.Load(int3(p, 0)).rgb);
                sum += l;
                sum2 += l * l;
                count += 1.0f;
            }
        }
        const float inv = 1.0f / max(count, 1.0f);
        sampleVar = RtVarianceFromMoments(sum * inv, sum2 * inv);
    }
    gVarOut[tid.xy] = float4(color.rgb, RtVarianceEstimate(sampleVar, histLen, forceSpatial));
}
