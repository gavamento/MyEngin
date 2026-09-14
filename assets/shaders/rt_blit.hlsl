// M46b: RT の中間バッファをシーンの上に貼り付けるフルスクリーンパス。
// 内部解像度 (GI 等) の拡大にも使うので、描画先サイズを CB で受けて線形サンプルする。

cbuffer RtBlitCB : register(b0)
{
    float2 gBlitDstSize; // 描画先の解像度 (px)
    // 0 = rgb / 1 = a を履歴長 (M46d) / 2 = a を分散 (M46e) のヒートマップ /
    // 3 = r をグレースケール (M46g: 影の可視率) /
    // 4 = a を ReflectionClass の色 (M67d: 反射像側。rgb = ns も見て空を黒に落とす)
    int gBlitMode;
    float gBlitParam;    // 正規化スケール (mode 1 = 履歴長の上限 / mode 2 = 標準偏差の倍率)
};

Texture2D gSrc : register(t0);
SamplerState gBlitSamp : register(s0);  // LINEAR / CLAMP
SamplerState gBlitPoint : register(s1); // POINT / CLAMP (M67d: 整数を補間させない)

struct VSOut {
    float4 pos : SV_Position;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float2 uv = i.pos.xy / max(gBlitDstSize, float2(1.0f, 1.0f));
    // M67d: クラス番号 (整数) を線形補間すると境界に「隣り合う 2 クラスの中間の番号」=
    // 実在しないクラスの色が出るので、mode 4 だけ点サンプルで引く (gBlitMode は
    // CB のスカラー = uniform な分岐)
    const float4 s = (gBlitMode == 4) ? gSrc.SampleLevel(gBlitPoint, uv, 0)
                                      : gSrc.SampleLevel(gBlitSamp, uv, 0);
    float3 c = s.rgb;
    if (gBlitMode == 1) {
        // 履歴長 0 → 赤 (履歴なし) / 中間 → 黄 / 上限 → 緑 (十分に蓄積された)
        const float t = saturate(s.a / max(gBlitParam, 1.0f));
        c = float3(saturate(2.0f - 2.0f * t), saturate(2.0f * t), 0.0f);
    } else if (gBlitMode == 2) {
        // M46e: 推定標準偏差 0 → 緑 (収束) / 大 → 赤 (まだノイズが乗っている)
        const float t = saturate(sqrt(max(s.a, 0.0f)) * gBlitParam);
        c = float3(saturate(2.0f * t), saturate(2.0f - 2.0f * t), 0.0f);
    } else if (gBlitMode == 3) {
        // M46g: 1 チャンネル量 (影の可視率) をそのまま白黒で
        c = float3(s.r, s.r, s.r);
    } else if (gBlitMode == 4) {
        // M67d: 反射像側の ReflectionClass (デバッグ 14)。a = クラス番号、rgb = ヒット法線。
        // ★色表は **rt_common.hlsli::RtReflClassColor と一致させること** —
        //   このシェーダは rt_common を include できない (向こうの RtSceneCB が b0 で
        //   RtBlitCB と衝突する) ので写しになっている。デバッグ 13 と 14 で
        //   同じ物体が違う色に見えたらここがずれている
        const float3 kClassColors[5] = {
            float3(1.0f, 0.2f, 0.2f), // 0 Hero      = 赤
            float3(1.0f, 0.6f, 0.1f), // 1 Character = 橙
            float3(0.9f, 0.9f, 0.2f), // 2 Vehicle   = 黄
            float3(0.2f, 0.8f, 1.0f), // 3 Prop      = 水色
            float3(0.5f, 0.5f, 0.5f), // 4 Default   = 灰
        };
        // 空 reservoir は cls = -1 = 範囲外 → 黒。**切り捨てでは -1 が 0 (Hero) に
        // 化ける**ので round で読む (rt_restir_common.hlsli::RtReservoirUnpack と同じ規約)
        const int cls = (int)round(s.a);
        c = (cls >= 0 && cls < 5) ? kClassColors[cls] : float3(0.0f, 0.0f, 0.0f);
        if (dot(s.rgb, s.rgb) <= 0.0f) {
            // スカイヒットは cls = 4 (Default) だが法線が無い。デバッグ 13 の
            // 「一次レイのミス = 黒」と見え方を揃える
            c = float3(0.0f, 0.0f, 0.0f);
        }
    }
    return float4(c, 1.0f);
}
