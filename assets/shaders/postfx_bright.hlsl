// ブルーム bright-pass (M16): HDR シーンから輝度がしきい値を超えた分のみ抽出する。
// フルスクリーン三角形。半解像度ターゲットへ描くことでダウンサンプルも兼ねる (linear sample)。

cbuffer Bright : register(b0)
{
    float gThreshold;  // 輝度しきい値 (これ未満は 0)。露出後空間で判定する
    float gExposure;   // 手動露出 (PostFx cbuffer の gExposure と同値)
    int gAutoExposure; // 1 = t1 の露出倍率を乗算 (M44b)
    float gPad2;
};

Texture2D gScene : register(t0);
StructuredBuffer<float> gExposureBuf : register(t1); // 自動露出の倍率 (tonemap の t5 と同じバッファ)
SamplerState gLinear : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const float2 corners[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    VSOut o;
    o.pos = float4(corners[vid], 0.0f, 1.0f);
    o.uv = corners[vid] * float2(0.5f, -0.5f) + 0.5f;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    const float3 c = gScene.Sample(gLinear, i.uv).rgb;
    // しきい値判定は露出後空間で行う — 露出前の絶対値だと、自動露出で持ち上げた暗所で
    // 何もしきい値を超えられずブルームが全滅する (露出後なら gThreshold=1.0 が
    // 「表示でほぼ白飛び」という一定の意味を持つ)。出力は露出前のまま = 露出は
    // tonemap 側が加算合成の後に一括で掛ける。exposure==1 では従来とビット同一
    float exposure = gExposure;
    if (gAutoExposure != 0) {
        exposure *= gExposureBuf[0];
    }
    const float luma = dot(c, float3(0.2126f, 0.7152f, 0.0722f)) * exposure;
    // しきい値超過分を色相を保ったまま取り出す (ソフト)
    const float contrib = max(luma - gThreshold, 0.0f) / max(luma, 1e-4f);
    return float4(c * contrib, 1.0f);
}
