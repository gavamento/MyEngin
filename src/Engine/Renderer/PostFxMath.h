#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <DirectXMath.h>

// ポストプロセス/大気系シェーダの数式を C++ に複製した純関数群 (D3D 非依存)。
// HLSL 側 (common.hlsli / postfx_*.hlsl) とコメント同期で複製し、selftest がこちらを検証する。
// 描画専用 (sim/hash 非関与)。
// M55a 以降、複数パスで共有する描画数式の CPU ミラーはポストプロセス由来でなくても
// ここへ置く (LinearizeDepth は DoF / パーティクル / 今後の HZB・SSR・froxel が共有する)。
namespace mye {

// M55a: 透視投影の非線形深度 [0,1] → ビュー空間 z。
// common.hlsli::LinearizeDepth と同一式 — 変更時は両方更新 (RenderSelfTest が検証)。
// 分母の 1e-4 クランプは d==1 かつ near==0 のゼロ除算よけで、HLSL 側と揃えてある。
// Particles/ParticleCurves.h::LinearizeParticleDepth はこの関数への別名 (呼び出し元と
// 既存 selftest の名前を残すためだけの薄いラッパ) — CPU 側の式はここ 1 つだけ。
inline float LinearizeDepth(float d, float nearZ, float farZ)
{
    return nearZ * farZ / (std::max)(farZ - d * (farZ - nearZ), 1e-4f);
}

// M43a: ハイトフォグ。高度で exp 減衰する密度 ρ(y)=e^{-k(y-base)} の視線積分を
// 「実効距離」に畳む: ∫ρ = e^{-k(camY-base)} · (1-e^{-kΔy})/(kΔy) · dist (Δy=posY-camY)。
// common.hlsli::ApplyFog と同一式 — 変更時は両方更新。
// heightFalloff<=0 は dist をそのまま返す (従来とビット同一)。指数は overflow 回避で ±60 clamp
inline float HeightFogEffectiveDistance(float dist, float camY, float posY, float heightFalloff,
                                        float baseHeight)
{
    if (heightFalloff <= 0.0f) {
        return dist;
    }
    const float kd = heightFalloff * (posY - camY);
    // 分母は ±1e-4 でクランプ — C4723 対策 (定数畳み込みが 0 除算を検出して警告する)。
    // |kd|<=1e-4 は分岐で 1 を返すため結果は HLSL 側の素の除算と同じ
    const float safeKd = (kd >= 0.0f) ? (std::max)(kd, 1e-4f) : (std::min)(kd, -1e-4f);
    const float slope = (std::abs(kd) > 1e-4f) ? (1.0f - std::exp(-safeKd)) / safeKd : 1.0f;
    return dist * std::exp(std::clamp(-heightFalloff * (camY - baseHeight), -60.0f, 60.0f))
           * slope;
}

// M43a: 太陽インスキャッタ係数 [0,1]。視線 rayDir が太陽 (光の進行方向 sunDir の逆) へ
// 向くほど 1 に近づく。common.hlsli::ApplyFog の sunAmount と同一式
inline float SunInscatterFactor(const DirectX::XMFLOAT3& rayDir, const DirectX::XMFLOAT3& sunDir,
                                float power)
{
    const float d = -(rayDir.x * sunDir.x + rayDir.y * sunDir.y + rayDir.z * sunDir.z);
    return std::pow(std::clamp(d, 0.0f, 1.0f), std::max(power, 1e-2f));
}

// M44a: 256x16 ストリップ LUT のサンプル UV。blue でスライス 2 枚 (u0/u1) と補間係数 frac を
// 選び、呼び出し側が 2 サンプルを lerp する = 実質トリリニア。
// postfx_tonemap.hlsl::SampleLutStrip とコメント同期 — 変更時は両方更新
inline void LutStripUv(float r, float g, float b, float& u0, float& u1, float& v, float& frac)
{
    const float bb = std::clamp(b, 0.0f, 1.0f) * 15.0f;
    const float slice0 = std::floor(bb);
    frac = bb - slice0;
    const float slice1 = (std::min)(slice0 + 1.0f, 15.0f);
    const float rr = std::clamp(r, 0.0f, 1.0f);
    u0 = (slice0 * 16.0f + rr * 15.0f + 0.5f) / 256.0f;
    u1 = (slice1 * 16.0f + rr * 15.0f + 0.5f) / 256.0f;
    v = (std::clamp(g, 0.0f, 1.0f) * 15.0f + 0.5f) / 16.0f;
}

// M44b: 自動露出のヒストグラム bin。log2 輝度域 [-10,+6] を bin 1..255 に量子化、
// bin 0 = ほぼ黒 (レンジ外) 専用 (平均から除外される)。
// postfx_hist.cs.hlsl / postfx_hist_reduce.cs.hlsl とコメント同期 — 変更時は全て更新
inline int BinForLuminance(float lum)
{
    if (lum <= 1e-6f) {
        return 0;
    }
    const float t = std::clamp((std::log2(lum) + 10.0f) / 16.0f, 0.0f, 1.0f);
    return static_cast<int>(t * 254.0f + 1.5f); // 1..255
}

// bin → 代表輝度 (BinForLuminance の逆量子化)
inline float LumForBin(int bin)
{
    return std::exp2((static_cast<float>(bin) - 1.0f) / 254.0f * 16.0f - 10.0f);
}

// M44c: 符号付き CoC [-1,1]。焦点面 = 0、手前が負・奥が正、focusRange で ±1 に達し clamp。
// postfx_dof_prefilter/composite.hlsl とコメント同期 — 変更時は全て更新
inline float SignedCoC(float viewZ, float focusDist, float focusRange)
{
    return std::clamp((viewZ - focusDist) / (std::max)(focusRange, 1e-4f), -1.0f, 1.0f);
}

// M43b: 太陽のスクリーン位置。sunDir (光の進行方向) の逆 = 太陽方向を方向ベクトル (w=0)
// として view*proj で射影し、UV [0,1] とフェード係数 [0,1] を返す。
// 背面 (clip.w<=0) は 0。画面内 = 1、画面外は UV が [0,1] を出た距離 0.25 で線形減衰
// (ゴッドレイのソースが視界から離れるほど自然に消える)。postfx_godray_*.hlsl の CB 供給元
inline float ComputeSunScreenPos(const DirectX::XMFLOAT4X4& view,
                                 const DirectX::XMFLOAT4X4& proj,
                                 const DirectX::XMFLOAT3& sunDir, float& outU, float& outV)
{
    using namespace DirectX;
    const XMMATRIX vp = XMLoadFloat4x4(&view) * XMLoadFloat4x4(&proj);
    const XMVECTOR clip =
        XMVector4Transform(XMVectorSet(-sunDir.x, -sunDir.y, -sunDir.z, 0.0f), vp);
    const float w = XMVectorGetW(clip);
    outU = 0.5f;
    outV = 0.5f;
    if (w <= 1e-4f) {
        return 0.0f; // 太陽が背面
    }
    outU = 0.5f + 0.5f * (XMVectorGetX(clip) / w);
    outV = 0.5f - 0.5f * (XMVectorGetY(clip) / w);
    const float ox = (std::max)({ 0.0f, -outU, outU - 1.0f });
    const float oy = (std::max)({ 0.0f, -outV, outV - 1.0f });
    return std::clamp(1.0f - (std::max)(ox, oy) / 0.25f, 0.0f, 1.0f);
}

// M44d: 深度再投影 — 現フレームの UV+深度 → ワールド → 前フレームの viewProj で UV へ。
// postfx_motionblur.hlsl の**背景フォールバック側**とコメント同期 — 変更時は両方更新。
// 行列は未転置 (row ベクトル規約)。false = 復元不能または前フレームで背面 (ブラー 0)
inline bool ReprojectUv(const DirectX::XMFLOAT4X4& invViewProj,
                        const DirectX::XMFLOAT4X4& prevViewProj, float u, float v, float depth,
                        float& prevU, float& prevV)
{
    using namespace DirectX;
    const float ndcX = u * 2.0f - 1.0f;
    const float ndcY = 1.0f - v * 2.0f;
    XMVECTOR world = XMVector4Transform(XMVectorSet(ndcX, ndcY, depth, 1.0f),
                                        XMLoadFloat4x4(&invViewProj));
    const float w = XMVectorGetW(world);
    if (std::abs(w) < 1e-6f) {
        return false;
    }
    world = XMVectorSetW(XMVectorScale(world, 1.0f / w), 1.0f);
    const XMVECTOR clip = XMVector4Transform(world, XMLoadFloat4x4(&prevViewProj));
    const float cw = XMVectorGetW(clip);
    if (cw <= 1e-4f) {
        return false;
    }
    prevU = XMVectorGetX(clip) / cw * 0.5f + 0.5f;
    prevV = 0.5f - XMVectorGetY(clip) / cw * 0.5f;
    return true;
}

// M55e: モーションブラーの速度ベクトル (UV)。**postfx_motionblur.hlsl の PSMain と同手順** —
// 片方だけ直すと「ブラーの向きだけ静かに違う」形で壊れるので変更時は両方更新
// (RenderSelfTest の TestMotionBlurVelocity が CPU 側を固定する)。
//
// ★速度源は画素ごとに選ぶ。ここが v1 (M44d、カメラのみ) から変わった唯一の点:
//   ① hasVelocity かつ **その画素にジオメトリがある** (depth < 1) → GBuffer RT4 の
//      画面速度をそのまま使う。カメラ + オブジェクトが合成済みなので、静止カメラでも
//      回る物体がブレる。
//   ② それ以外 → 深度再投影 (v1 と完全に同じ式)。Forward パスには velocity が無く、
//      背景 / スカイは GBuffer を書かないので RT4 が 0 のまま残る。ここを分けないと
//      「カメラを振っても空だけ止まって見える」= v1 より悪い絵になる。
// 戻り値 false = 速度 0 (ブラーしない)
namespace motionblur {

inline bool BlurVector(bool hasVelocity, float velU, float velV,
                       const DirectX::XMFLOAT4X4& invViewProj,
                       const DirectX::XMFLOAT4X4& prevViewProj, float u, float v, float depth,
                       float intensity, float maxPixels, float screenW, float screenH,
                       float& outU, float& outV)
{
    outU = 0.0f;
    outV = 0.0f;
    float du = 0.0f;
    float dv = 0.0f;
    if (hasVelocity && depth < 1.0f) {
        du = velU;
        dv = velV;
    } else {
        float prevU = 0.0f;
        float prevV = 0.0f;
        if (!ReprojectUv(invViewProj, prevViewProj, u, v, depth, prevU, prevV)) {
            return false;
        }
        du = u - prevU;
        dv = v - prevV;
    }
    du *= intensity;
    dv *= intensity;
    // クランプは **px 長** で行う (UV 長だと縦横比で暴走量が変わる)。HLSL 側の
    // length(vel * gScreenSize) と同じ
    const float px = du * screenW;
    const float py = dv * screenH;
    const float lenPx = std::sqrt(px * px + py * py);
    if (lenPx > maxPixels) {
        const float k = maxPixels / lenPx;
        du *= k;
        dv *= k;
    }
    outU = du;
    outV = dv;
    return true;
}

} // namespace motionblur

// M55b: TAA 用カメラジッタ。HLSL ミラーは無い (CPU で射影行列に畳んでしまうので
// シェーダ側は自分がジッタされていることを知らない) が、複数パスが共有する描画数式
// なのでこのヘッダの方針に従ってここへ置く。
//
// **設計の要**: 揺らした射影を受け取るのはラスタライズ経路だけ (Deferred GBuffer /
// Forward / スカイ / パーティクル / デバッグ線)。再投影 (prevViewProj・モーションブラー・
// RT テンポラル)、シャドウのカスケードフィット、視錐台カリング、太陽の画面位置、
// エディタのギズモ/ピッキングは **ジッタ前**の射影 (RenderView::projNoJitter) を読む。
// ジッタ付きを履歴側へ混ぜると「カメラが毎フレーム半ピクセル動いた」ことになり、
// RT テンポラルとモーションブラーが同時に壊れる。
//
// 列は frame index 由来の Halton なので実時間も rand() も混ざらない。決定的撮影モード
// (frame 番号 == tick 番号) ではジッタ列そのものが自動的に決定論になる。
namespace camerajitter {

// ジッタ列の周期。TAA の履歴が 1 巡する長さと揃えてある
constexpr uint32_t kSequenceLength = 8;

// 基数 base の radical inverse (van der Corput)。Halton 列の 1 次元分
inline float RadicalInverse(uint32_t index, uint32_t base)
{
    const float invBase = 1.0f / static_cast<float>(base);
    float result = 0.0f;
    float f = invBase;
    while (index > 0) {
        result += static_cast<float>(index % base) * f;
        index /= base;
        f *= invBase;
    }
    return result;
}

// frameIndex → サブピクセルオフセット [-0.5, 0.5] (ピクセル単位、Halton(2,3))。
// index に +1 しているのは Halton(0) が (0,0) = 「1 巡に 1 回だけ揺れないフレーム」に
// なるのを避けるため (そのフレームだけ実効サンプル位置が重複し、周期的なちらつきになる)
inline void Sample(uint32_t frameIndex, float& outX, float& outY)
{
    const uint32_t i = (frameIndex % kSequenceLength) + 1u;
    outX = RadicalInverse(i, 2) - 0.5f;
    outY = RadicalInverse(i, 3) - 0.5f;
}

// ピクセル単位のオフセット → NDC オフセット。
// y を反転するのは NDC が上向き・ピクセル座標が下向きだから
inline void PixelsToNdc(float px, float py, int width, int height, float& outX, float& outY)
{
    outX = (width > 0) ? (px * 2.0f / static_cast<float>(width)) : 0.0f;
    outY = (height > 0) ? (-py * 2.0f / static_cast<float>(height)) : 0.0f;
}

// 射影行列に NDC オフセットを載せる (行ベクトル規約)。
// 透視 (_34 == 1、w = viewZ) は _31/_32 への加算がそのまま NDC の平行移動になる。
// 正射影 (_34 == 0、w = 1) は _41/_42 側 — エディタの Ortho ビューが実際にここへ来る
// (ComputeCascadeVPs も同じ _34 判定で経路を分けている)。
// オフセットが 0 なら 1 ビットも変わらない = 振幅 0 の既定で従来と完全一致。
inline DirectX::XMFLOAT4X4 ApplyToProj(const DirectX::XMFLOAT4X4& proj, float ndcX, float ndcY)
{
    DirectX::XMFLOAT4X4 m = proj;
    if (std::fabs(proj._34) > 1e-6f) {
        m._31 += ndcX;
        m._32 += ndcY;
    } else {
        m._41 += ndcX;
        m._42 += ndcY;
    }
    return m;
}

} // namespace camerajitter

// M55c: GBuffer RT4 (R16G16_FLOAT) へ書く画面速度。
// **deferred_gbuffer{,_instanced,_skinned}.hlsl の ComputeVelocity と同じ式** —
// 片方だけ直すと「velocity が静かに間違っている」形で壊れる (誰も読んでいない間は
// 絵にも出ない) ので、変更時は 4 箇所すべてを更新すること。
//
// 規約: **velocity = 今フレームの UV − 前フレームの UV**。読む側は prevUv = uv - velocity。
//   ・今フレームのクリップ座標は**ジッタ込み**の proj で作られている (ラスタライズと
//     同じ行列で無いとピクセル中心とずれる) ので、NDC にしてからジッタを引き戻す。
//   ・前フレーム側は RenderView::prevViewProj = **非ジッタ**なので何も引かない。
//     ここを揃えないと「静止物が毎フレーム半ピクセル動く」velocity になる。
namespace velocity {

// clip 空間の 2 点 (今 / 前) → UV 速度。false = どちらかがカメラ背面 (速度 0 とみなす)
inline bool FromClip(const DirectX::XMFLOAT4& curClip, const DirectX::XMFLOAT4& prevClip,
                     float jitterNdcX, float jitterNdcY, float& outU, float& outV)
{
    outU = 0.0f;
    outV = 0.0f;
    if (curClip.w <= 1e-6f || prevClip.w <= 1e-6f) {
        return false;
    }
    const float curX = curClip.x / curClip.w - jitterNdcX;
    const float curY = curClip.y / curClip.w - jitterNdcY;
    const float prevX = prevClip.x / prevClip.w;
    const float prevY = prevClip.y / prevClip.w;
    outU = (curX - prevX) * 0.5f;
    outV = (curY - prevY) * -0.5f; // NDC は上向き / UV は下向き
    return true;
}

// ワールド座標 2 点 (今フレームの位置 / 前フレームに実際に描かれた位置) からの一括計算。
// curViewProj はジッタ込み・prevViewProj は非ジッタ (どちらも未転置 = 行ベクトル規約)
inline bool FromWorld(const DirectX::XMFLOAT4X4& curViewProjJittered,
                      const DirectX::XMFLOAT4X4& prevViewProj,
                      const DirectX::XMFLOAT3& curWorldPos,
                      const DirectX::XMFLOAT3& prevWorldPos, float jitterNdcX, float jitterNdcY,
                      float& outU, float& outV)
{
    using namespace DirectX;
    DirectX::XMFLOAT4 cur;
    DirectX::XMFLOAT4 prev;
    XMStoreFloat4(&cur,
                  XMVector4Transform(XMVectorSet(curWorldPos.x, curWorldPos.y, curWorldPos.z, 1.0f),
                                     XMLoadFloat4x4(&curViewProjJittered)));
    XMStoreFloat4(&prev, XMVector4Transform(XMVectorSet(prevWorldPos.x, prevWorldPos.y,
                                                        prevWorldPos.z, 1.0f),
                                            XMLoadFloat4x4(&prevViewProj)));
    return FromClip(cur, prev, jitterNdcX, jitterNdcY, outU, outV);
}

} // namespace velocity

// M55d: TAA の解決式。**postfx_taa.hlsl の PSMain と同じ手順** — 片方だけ直すと
// 「ゴーストが取れない / 静止画が動く」形で静かに壊れる (RenderSelfTest の
// TestTaaResolve が CPU 側を検証し、HLSL との一致はコメント同期で担保する)。
//
// 規約: 再投影は **prevUv = uv - velocity** (M55c と同じ)。velocity 側でジッタは
// 既に引き戻されているので、ここでは何も足し引きしない。
namespace taa {

// 履歴 UV が画面内か。外れていたら前フレームにその画素は無い = 履歴を捨てる
inline bool HistoryUvValid(float u, float v)
{
    return u >= 0.0f && v >= 0.0f && u < 1.0f && v < 1.0f;
}

// 今フレームの近傍が作る色の箱へ履歴を押し込む (ゴースト抑制)。
// 遮蔽が解けた画素では履歴が近傍のどれとも似ていないので、この 1 行で自動的に捨てられる
inline DirectX::XMFLOAT3 ClampToNeighborhood(const DirectX::XMFLOAT3& hist,
                                             const DirectX::XMFLOAT3& nmin,
                                             const DirectX::XMFLOAT3& nmax)
{
    return { std::clamp(hist.x, nmin.x, nmax.x), std::clamp(hist.y, nmin.y, nmax.y),
             std::clamp(hist.z, nmin.z, nmax.z) };
}

// 最終色。histValid=false / 履歴 UV が画面外 / feedback=0 のいずれでも
// **cur をビット単位でそのまま返す** — これが「TAA off で絵が 1 ビットも変わらない」の根拠
inline DirectX::XMFLOAT3 Resolve(const DirectX::XMFLOAT3& cur, const DirectX::XMFLOAT3& hist,
                                 const DirectX::XMFLOAT3& nmin, const DirectX::XMFLOAT3& nmax,
                                 float prevU, float prevV, bool histValid, float feedback)
{
    if (!histValid || !HistoryUvValid(prevU, prevV)) {
        return cur;
    }
    const DirectX::XMFLOAT3 c = ClampToNeighborhood(hist, nmin, nmax);
    return { cur.x + (c.x - cur.x) * feedback, cur.y + (c.y - cur.y) * feedback,
             cur.z + (c.z - cur.z) * feedback };
}

} // namespace taa

} // namespace mye
