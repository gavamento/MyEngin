// M67d: 再投影 (前フレームの同じ材質点を探す) の共通関数。SVGF の蓄積 (rt_temporal) と
// ReSTIR の temporal 再利用 (rt_refl、M67e) が同じ判定を使う — 2 か所に書くと
// 「片方だけ直して SVGF と ReSTIR の履歴条件がずれる」形で静かに壊れる。
//
// C++ 側 src/Engine/Renderer/RayTracing/RtMath.h の同名関数の写しで、
// **式は 1 文字も変えずに両方更新すること** (RtSelfTest が CPU 側を固定する根拠)。
//
// このヘッダは rt_common.hlsli に依存しない (テクスチャも定数バッファも触らない) ので、
// rt_temporal.cs.hlsl のように rt_common を include しないシェーダからも使える。

#ifndef MYE_RT_REPROJECT_INCLUDED
#define MYE_RT_REPROJECT_INCLUDED

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

// 輝度 (Rec.709)。RtMath.h の RtLuminance と同一式。
// ★rt_restir_common.hlsli も同名関数を持つので**同じガード名**で包む — 両方を
//   include するシェーダ (rt_refl) で二重定義になると、反射シェーダが丸ごと
//   コンパイルできず golden (demo_render_rtrefl) が動く
#ifndef MYE_RT_LUMINANCE_DEFINED
#define MYE_RT_LUMINANCE_DEFINED
float RtLuminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}
#endif

#endif // MYE_RT_REPROJECT_INCLUDED
