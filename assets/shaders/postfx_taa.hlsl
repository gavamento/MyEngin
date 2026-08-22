// M55d: テンポラル AA (TAA)。ポストプロセスチェーンの**先頭** (DoF より前) で
// 「今フレームの HDR シーン」と「前フレームの TAA 結果」を混ぜる。
//
// 成立の条件は 3 つで、どれが欠けても走らない (= 絵は 1 ビットも変わらない):
//   ① カメラが frame index 由来の Halton でサブピクセルジッタされていること (M55b)
//   ② 画面速度バッファ (GBuffer RT4、M55c) があること = **Deferred のみ**
//   ③ 前フレームも同じビューが同じ解像度で描かれていること (通番の連続性)
//
// 再投影の規約は M55c と同じ: **prevUv = uv - velocity**。
// velocity 側でジッタは既に引き戻されているので、ここでは何も足し引きしない。
//
// ★履歴のクランプ: 再投影した履歴をそのまま混ぜるとゴースト (前フレームの像が尾を引く)
//   になる。今フレームの 3x3 近傍が作る色の箱へ押し込むのが定番の抑制で、これで
//   「近傍のどれとも似ていない履歴」= 遮蔽が解けた画素 が自動的に捨てられる。
//   代償はこの min/max 分岐が **機種差を増幅する**こと — FXAA と同じ性質なので
//   demo_render_taa の golden は tol=0 のローカル限定にしてある (MYE_SHOT_SKIP_TAA)。
//
// **CPU ミラー: PostFxMath.h の mye::taa::Resolve — 変更時は両方更新**
// (RenderSelfTest の TestTaaResolve が検証)。

cbuffer TaaCB : register(b0)
{
    float2 gTaaSize;    // 描画先の解像度 (px)
    float gTaaFeedback; // 履歴の残し率 [0,1)。0 = 履歴を混ぜない (= 恒等)
    int gTaaHistValid;  // 0 = 履歴なし (初回 / リサイズ / 通番が飛んだ) → cur をそのまま出す
};

Texture2D gTaaCur : register(t0);            // 今フレームの HDR シーン
Texture2D gTaaHist : register(t1);           // 前フレームの TAA 結果
Texture2D<float2> gTaaVelocity : register(t2); // GBuffer RT4 (今 UV − 前 UV)
SamplerState gLinear : register(s0);

struct VSOut
{
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
    const int2 maxPx = int2(gTaaSize) - int2(1, 1);
    const int2 px = clamp(int2(i.pos.xy), int2(0, 0), maxPx);
    // シーン / 速度は描画先と同解像度なので Load (サンプラの補間で嘘をつかない)
    const float4 curSample = gTaaCur.Load(int3(px, 0));
    const float3 cur = curSample.rgb;
    if (gTaaHistValid == 0) {
        return curSample; // 履歴なし = 今フレームそのまま (初回フレームの縮退)
    }

    // 3x3 近傍の色の箱 (履歴のクランプ範囲)
    float3 nmin = cur;
    float3 nmax = cur;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = clamp(px + int2(x, y), int2(0, 0), maxPx);
            const float3 c = gTaaCur.Load(int3(q, 0)).rgb;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
        }
    }

    const float2 uv = (float2(px) + 0.5f) / gTaaSize;
    const float2 prevUv = uv - gTaaVelocity.Load(int3(px, 0));
    if (any(prevUv < 0.0f) || any(prevUv >= 1.0f)) {
        return curSample; // 前フレームの画面の外 = 履歴が存在しない
    }

    // 履歴は非整数 UV になるのでバイリニア。クランプは箱への押し込み
    const float3 hist = clamp(gTaaHist.SampleLevel(gLinear, prevUv, 0).rgb, nmin, nmax);
    return float4(lerp(cur, hist, gTaaFeedback), curSample.a);
}
