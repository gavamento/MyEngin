// M46d: GI のテンポラル蓄積。1spp のノイズを「前フレームの結果を再投影して混ぜる」ことで
// 落とす。SVGF の第 1 段 — 空間フィルタ (A-Trous) は M46e で後段に挿す。
//
// 履歴の持ち方: 色バッファ (rgb = 蓄積 GI, a = 履歴長) と
//               ジオメトリバッファ (xyz = ワールド法線, w = カメラからの距離) の 2 枚を
//               viewKey 別に ping-pong する。深度は view 行列を要らなくするため
//               「カメラ距離」で持つ (カメラの向きが変わっても意味が変わらない)。
//
// 再投影: 現フレームの可視点 P を前フレームの viewProj で射影して履歴 UV を得る。
//         得られた画素の記録法線/距離が現在の面と食い違えば (disocclusion) 履歴を捨てる。
// v1 制限: 履歴のタップは最近傍 1 点 (バイリニアの部分棄却はしない)。
//          物体自身の運動ベクトルを持たないので、動く物体は履歴が残る (ゴースト)。

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
    float3 gTempPad;
};

Texture2D gTempCur : register(t0);       // このフレームの 1spp GI (内部解像度)
Texture2D gTempHistColor : register(t1); // 前フレームの蓄積結果 (rgb, a = 履歴長)
Texture2D gTempHistGeom : register(t2);  // 前フレームの法線 (xyz) とカメラ距離 (w)
Texture2D gTempGbNormal : register(t3);  // GBuffer 法線 (*0.5+0.5、フル解像度)
Texture2D gTempGbPosition : register(t4); // GBuffer ワールド座標 (フル解像度)
Texture2D gTempGbMark : register(t5);    // GBuffer アルベド (a = ジオメトリ有りマーク)

RWTexture2D<float4> gTempOutColor : register(u0);
RWTexture2D<float4> gTempOutGeom : register(u1);

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
        return;
    }
    const float3 N = normalize(gTempGbNormal.Load(gp).xyz * 2.0f - 1.0f);
    const float3 P = gTempGbPosition.Load(gp).xyz;
    const float3 cur = gTempCur.Load(int3(tid.xy, 0)).rgb;

    // 履歴の探索: P を前フレームのカメラへ射影する
    bool valid = false;
    float4 hist = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float expectedDepth = length(P - gTempPrevCameraPos);
    if (gTempHistValid != 0) {
        float2 prevUv;
        if (RtClipToPrevUv(mul(float4(P, 1.0f), gTempPrevViewProj), prevUv)) {
            const int3 hp = int3(int2(prevUv * gTempOutSize), 0);
            const float4 geom = gTempHistGeom.Load(hp);
            if (RtReprojectValid(expectedDepth, geom.w, N, geom.xyz, gTempDepthThreshold,
                                 gTempNormalThreshold)) {
                hist = gTempHistColor.Load(hp);
                valid = true;
            }
        }
    }

    const float histLen =
        RtAdvanceHistory(hist.a, valid, (float)MYE_RT_TEMPORAL_MAX_HISTORY);
    const float alpha = RtTemporalAlpha(histLen);
    const float3 accum = lerp(hist.rgb, cur, alpha); // valid=false なら histLen=1 → cur

    gTempOutColor[tid.xy] = float4(accum, histLen);
    gTempOutGeom[tid.xy] = float4(N, length(P - gTempCameraPos));
}
