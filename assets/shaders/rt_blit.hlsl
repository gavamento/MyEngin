// M46b: RT の中間バッファをシーンの上に貼り付けるフルスクリーンパス。
// 内部解像度 (GI 等) の拡大にも使うので、描画先サイズを CB で受けて線形サンプルする。

cbuffer RtBlitCB : register(b0)
{
    float2 gBlitDstSize; // 描画先の解像度 (px)
    // 0 = rgb / 1 = a を履歴長 (M46d) / 2 = a を分散 (M46e) のヒートマップ /
    // 3 = r をグレースケール (M46g: 影の可視率)
    int gBlitMode;
    float gBlitParam;    // 正規化スケール (mode 1 = 履歴長の上限 / mode 2 = 標準偏差の倍率)
};

Texture2D gSrc : register(t0);
SamplerState gBlitSamp : register(s0); // LINEAR / CLAMP

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
    const float4 s = gSrc.SampleLevel(gBlitSamp, uv, 0);
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
    }
    return float4(c, 1.0f);
}
