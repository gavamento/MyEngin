// M46b: RT の中間バッファをシーンの上に貼り付けるフルスクリーンパス。
// 内部解像度 (GI 等) の拡大にも使うので、描画先サイズを CB で受けて線形サンプルする。

cbuffer RtBlitCB : register(b0)
{
    float2 gBlitDstSize; // 描画先の解像度 (px)
    int gBlitMode;       // 0 = rgb をそのまま / 1 = a を履歴長ヒートマップとして表示 (M46d)
    float gBlitParam;    // mode 1 = 履歴長の上限 (正規化に使う)
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
    }
    return float4(c, 1.0f);
}
