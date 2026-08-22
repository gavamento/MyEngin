// M46d: GI のテンポラル蓄積。1spp のノイズを「前フレームの結果を再投影して混ぜる」ことで
// 落とす。SVGF の第 1 段 — 後段の分散推定 (rt_variance) と A-Trous (rt_atrous) が M46e。
// ここでは色の蓄積に加えて、分散推定用の輝度モーメント (μ, μ²) も同じ重みで積む。
//
// 履歴の持ち方: 色バッファ (rgb = 蓄積 GI, a = 履歴長) と
//               ジオメトリバッファ (xyz = ワールド法線, w = カメラからの距離) の 2 枚を
//               viewKey 別に ping-pong する。深度は view 行列を要らなくするため
//               「カメラ距離」で持つ (カメラの向きが変わっても意味が変わらない)。
//
// 再投影 (M55f で 2 経路になった):
//   ① 画面速度あり (gTempUseVelocity != 0) → prevUv = uv - velocity。GBuffer RT4 は
//      カメラと物体の運動を合成済みなので、**動く物体でも同じ材質点の履歴**に当たる。
//   ② それ以外 (履歴なしフレーム / velocity 未バインド) → M46d のまま、現フレームの
//      可視点 P を前フレームの viewProj で射影する (カメラ運動のみ)。
// どちらの経路でも、得られた画素の記録法線/距離が現在の面と食い違えば履歴を捨てる。
// ★①でも深度判定は「**現**フレームの P を前カメラから測った距離」と比べている
//   (画面速度は 2D なので前フレームのカメラ距離を復元できない)。物体が 1 フレームで
//   カメラ距離を kRtTemporalDepthThreshold 以上動かすと履歴を捨てる = 尾を引く代わりに
//   ノイズへ落ちる。安全側なので v1 はこれで許容する。
// v1 制限: 履歴のタップは最近傍 1 点 (バイリニアの部分棄却はしない)。
//          スキンメッシュは前フレームのボーンパレットが無いので velocity が
//          カメラ + 物体トランスフォームぶんしか出ない (M55c から続く制限)。

// C++ の kRtTemporalMaxHistory と一致検査される (tools/check_rules.ps1 規則 9)
#define MYE_RT_TEMPORAL_MAX_HISTORY 32

cbuffer RtTemporalCB : register(b2)
{
    float4x4 gTempPrevViewProj; // 転置済み (mul(row, M) 規約)
    float2 gTempOutSize;        // GI バッファの解像度 (内部解像度)
    float2 gTempGbSize;         // G-Buffer の解像度 (フル)
    float3 gTempPrevCameraPos;
    int gTempHistValid; // 0 = 前フレームの履歴が無い (初回 / リサイズ / 連続していない)
    float3 gTempCameraPos;
    float gTempDepthThreshold; // カメラ距離の相対許容 (RtTypes.h が出所)
    float gTempNormalThreshold; // 法線 cos の下限
    // M46h: この信号の履歴長上限。GI は MYE_RT_TEMPORAL_MAX_HISTORY と同値、
    // 反射はより短い値 (鏡面はカメラ運動で反射像が大きく動くので長く積むとラグになる)。
    // 下の #define はハードキャップとして残す (C++ 定数との一致を規則 9 が検査する)
    float gTempMaxHistory;
    // M55f: 1 = 履歴 UV を GBuffer RT4 (画面速度) から作る。0 = 前フレーム VP へ射影 (M46d)。
    // RtPasses が「velocity SRV が張れている かつ 履歴が有効」のときだけ 1 にする
    int gTempUseVelocity;
    float gTempPad;
};

Texture2D gTempCur : register(t0);       // このフレームの 1spp GI (内部解像度)
Texture2D gTempHistColor : register(t1); // 前フレームの蓄積結果 (rgb, a = 履歴長)
Texture2D gTempHistGeom : register(t2);  // 前フレームの法線 (xyz) とカメラ距離 (w)
Texture2D gTempGbNormal : register(t3);  // GBuffer 法線 (*0.5+0.5、フル解像度)
Texture2D gTempGbPosition : register(t4); // GBuffer ワールド座標 (フル解像度)
Texture2D gTempGbMark : register(t5);    // GBuffer アルベド (a = ジオメトリ有りマーク)
Texture2D gTempHistMoments : register(t6); // 前フレームの輝度モーメント (x = μ, y = μ²)
// M55f: GBuffer RT4 = 画面速度 (今 UV − 前 UV、フル解像度)。gTempUseVelocity==0 のときは
// null が張られる (Load は 0 を返すが、そもそも読まない)
Texture2D<float2> gTempGbVelocity : register(t7);

RWTexture2D<float4> gTempOutColor : register(u0);
RWTexture2D<float4> gTempOutGeom : register(u1);
// M46e: SVGF の分散推定に使う輝度モーメント (x = μ, y = μ²)。色と同じ重みで積む
RWTexture2D<float4> gTempOutMoments : register(u2);

// ---- RtMath.h と同一式 (変更時は両方更新。selftest が C++ 側を検証する) ----

// 前フレームのクリップ座標 → 履歴 UV。背後 (w<=0) と画面外は false
bool RtClipToPrevUv(float4 clip, out float2 outUv)
{
    outUv = float2(0.0f, 0.0f);
    bool ok = false;
    if (clip.w > 1e-6f) {
        const float2 ndc = clip.xy / clip.w;
        outUv = ndc * float2(0.5f, -0.5f) + 0.5f;
        ok = all(outUv >= 0.0f) && all(outUv < 1.0f);
    }
    return ok;
}

// M55f: 履歴 UV をどちらの経路で作るか。useVelocity != 0 なら画面速度、0 なら前フレーム VP。
// 画面外の棄却は 2 経路で同じ規約 (RtClipToPrevUv と揃えて [0,1) 判定)
bool RtHistoryUv(int useVelocity, float2 uv, float2 velocity, float4 prevClip, out float2 outUv)
{
    if (useVelocity != 0) {
        outUv = uv - velocity;
        return all(outUv >= 0.0f) && all(outUv < 1.0f);
    }
    return RtClipToPrevUv(prevClip, outUv);
}

// 再投影先の履歴が現在の面と同じものか (深度 = カメラ距離の相対差 + 法線 cos)
bool RtReprojectValid(float expectedDepth, float storedDepth, float3 n, float3 prevN,
                      float depthThreshold, float normalThreshold)
{
    bool ok = (storedDepth > 0.0f) && (expectedDepth > 0.0f);
    if (ok) {
        const float d = abs(expectedDepth - storedDepth);
        if (d > depthThreshold * max(expectedDepth, 1e-3f)) {
            ok = false; // 別の面が手前/奥にある
        } else if (dot(n, prevN) < normalThreshold) {
            ok = false; // 面の向きが違う
        }
    }
    return ok;
}

// 履歴長を 1 進める。無効なら 1 に若返る (= 今フレームの 1spp をそのまま採用)
float RtAdvanceHistory(float prevLen, bool valid, float maxLen)
{
    return min((valid ? prevLen : 0.0f) + 1.0f, maxLen);
}

// 移動平均の重み (新サンプルの寄与)。履歴長 1 で 1.0
float RtTemporalAlpha(float histLen)
{
    return 1.0f / max(histLen, 1.0f);
}

// 輝度 (Rec.709)。RtMath.h の RtLuminance と同一式
float RtLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gTempOutSize.x || tid.y >= (uint)gTempOutSize.y) {
        return;
    }
    // 内部解像度のピクセル中心を G-Buffer の座標へ写す (rt_gi.cs.hlsl と同じ写像)
    const float2 uv = (float2(tid.xy) + 0.5f) / gTempOutSize;
    const int3 gp = int3(int2(uv * gTempGbSize), 0);
    if (gTempGbMark.Load(gp).a < 0.5f) {
        // ジオメトリ無し (空) — 履歴も無効化しておく (深度 0 = 未記録)
        gTempOutColor[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gTempOutGeom[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gTempOutMoments[tid.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const float3 N = normalize(gTempGbNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gTempGbPosition.Load(gp).xyz;
    const float3 cur = gTempCur.Load(int3(tid.xy, 0)).rgb;

    // 履歴の探索: P を前フレームのカメラへ射影する
    bool valid = false;
    float4 hist = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 histMoments = float2(0.0f, 0.0f);
    const float expectedDepth = length(P - gTempPrevCameraPos);
    if (gTempHistValid != 0) {
        float2 prevUv;
        // velocity は GBuffer と同解像度なので、P/N と同じ gp で引く
        const float2 vel = gTempGbVelocity.Load(gp);
        if (RtHistoryUv(gTempUseVelocity, uv, vel, mul(float4(P, 1.0f), gTempPrevViewProj),
                        prevUv)) {
            const int3 hp = int3(int2(prevUv * gTempOutSize), 0);
            const float4 geom = gTempHistGeom.Load(hp);
            if (RtReprojectValid(expectedDepth, geom.w, N, geom.xyz, gTempDepthThreshold,
                                 gTempNormalThreshold)) {
                hist = gTempHistColor.Load(hp);
                histMoments = gTempHistMoments.Load(hp).xy;
                valid = true;
            }
        }
    }

    const float histLen =
        RtAdvanceHistory(hist.a, valid, min(gTempMaxHistory, (float)MYE_RT_TEMPORAL_MAX_HISTORY));
    const float alpha = RtTemporalAlpha(histLen);
    const float3 accum = lerp(hist.rgb, cur, alpha); // valid=false なら histLen=1 → cur

    // M46e: 輝度モーメントも同じ重みで積む。μ は accum の輝度と一致するが、
    // 16F の丸めで食い違わないよう独立に持つ (分散 = μ² - μ² が負に落ちないように)
    const float lum = RtLuminance(cur);
    const float2 moments = lerp(histMoments, float2(lum, lum * lum), alpha);

    gTempOutColor[tid.xy] = float4(accum, histLen);
    gTempOutGeom[tid.xy] = float4(N, length(P - gTempCameraPos));
    gTempOutMoments[tid.xy] = float4(moments, 0.0f, 0.0f);
}
