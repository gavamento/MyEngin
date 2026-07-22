// ポストプロセス解決 (M16): HDR シーンカラーをトーンマップして LDR ターゲットへ書く。
// フルスクリーン三角形 + .Load による 1:1 解決 (deferred_light と同じパターン)。
//   gTonemap: 0 = passthrough (色を変えず配管の透過性を検証する用途)
//             1 = ACES filmic (Narkowicz 近似, 既定)
//             2 = Reinhard
// bloom テクスチャ (t1) は gBloomIntensity>0 のとき加算合成する (M16 ブルーム)。

cbuffer PostFx : register(b0)
{
    float gExposure;
    int   gTonemap;
    float gBloomIntensity; // 0 でブルーム無効
    int   gApplyGamma;     // 1 で linear→sRGB OETF を適用 (正しい sRGB パイプラインが揃う M17 で ON)
    // ---- M32d ----
    float  gChromAb;         // 色収差 (UV スケール、0=off)
    float  gVignette;        // 周辺減光 (0=off)
    float  gVignetteRadius;  // 減光開始半径
    float  gSaturation;      // 彩度 (1=変化なし)
    float  gContrast;        // コントラスト (1=変化なし)
    float3 _pfxpad;
    float4 gColorFilter;     // 乗算カラーフィルタ
};

Texture2D gScene : register(t0); // HDR シーンカラー (R16G16B16A16F)
Texture2D gBloom : register(t1); // ブルーム (低解像度をアップサンプル済み。未使用時は黒)
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

// Narkowicz ACES filmic 近似 (linear HDR → 表示参照)
float3 ACES(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOut i) : SV_Target
{
    const int3 pixel = int3(int2(i.pos.xy), 0);

    if (gTonemap == 0) {
        return float4(gScene.Load(pixel).rgb, 1.0f); // passthrough: HDR 配管の非破壊検証用
    }

    // 色収差 (M32d): 中心からの放射方向に RGB を分離サンプル (0 なら Load で厳密)
    float3 hdr;
    if (gChromAb > 0.0f) {
        const float2 off = (i.uv - 0.5f) * gChromAb;
        hdr.r = gScene.Sample(gLinear, i.uv + off).r;
        hdr.g = gScene.Sample(gLinear, i.uv).g;
        hdr.b = gScene.Sample(gLinear, i.uv - off).b;
    } else {
        hdr = gScene.Load(pixel).rgb;
    }

    float3 c = hdr * gExposure;
    if (gBloomIntensity > 0.0f) {
        c += gBloom.Sample(gLinear, i.uv).rgb * gBloomIntensity; // ブルーム加算
    }
    if (gTonemap == 2) {
        c = c / (1.0f + c); // Reinhard
    } else {
        c = ACES(c);        // ACES (既定)
    }

    // カラーグレーディング (M32d): 彩度 → コントラスト → カラーフィルタ
    const float lum = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    c = lerp(float3(lum, lum, lum), c, gSaturation);
    c = (c - 0.5f) * gContrast + 0.5f;
    c *= gColorFilter.rgb;

    // ビネット (M32d): 中心から radius を超えた分だけ intensity で減光
    if (gVignette > 0.0f) {
        const float d = length(i.uv - 0.5f) * 1.41421356f; // 0=中心, ~1=四隅
        const float v = 1.0f
                        - gVignette * saturate((d - gVignetteRadius)
                                               / max(1e-4f, 1.0f - gVignetteRadius));
        c *= v;
    }

    if (gApplyGamma != 0) {
        c = pow(max(c, 0.0f), 1.0f / 2.2f); // linear → sRGB (OETF)。M16 は既定 OFF
    }
    return float4(max(c, 0.0f), 1.0f);
}
