#pragma once
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Renderer/RayTracing/RtTypes.h"

// レイトレーシングの数式を C++ に複製した純関数群 (D3D 非依存)。
// HLSL 側 assets/shaders/rt_common.hlsli とコメント同期で複製し、selftest がこちらを検証する。
// 描画専用 (sim / WorldHash 非関与)。**変更時は HLSL と両方更新すること**。
namespace mye {

// 0 除算を避けた逆数。rd の成分が 0 のとき (bmin-ro)*inf が NaN になるのを防ぐ。
// HLSL の RtSafeInv と同一式
inline float RtSafeInv(float d)
{
    constexpr float kEps = 1e-8f;
    if (d > kEps || d < -kEps) {
        return 1.0f / d;
    }
    return (d >= 0.0f) ? (1.0f / kEps) : (-1.0f / kEps);
}

// AABB スラブテスト。[0, tMax] の範囲で交差があれば true。
// HLSL の RtSlabTest と同一式
inline bool RtSlabTest(const DirectX::XMFLOAT3& bmin, const DirectX::XMFLOAT3& bmax,
                       const DirectX::XMFLOAT3& ro, const DirectX::XMFLOAT3& invD, float tMax)
{
    const float t0x = (bmin.x - ro.x) * invD.x, t1x = (bmax.x - ro.x) * invD.x;
    const float t0y = (bmin.y - ro.y) * invD.y, t1y = (bmax.y - ro.y) * invD.y;
    const float t0z = (bmin.z - ro.z) * invD.z, t1z = (bmax.z - ro.z) * invD.z;
    const float nx = (t0x < t1x) ? t0x : t1x, fx = (t0x < t1x) ? t1x : t0x;
    const float ny = (t0y < t1y) ? t0y : t1y, fy = (t0y < t1y) ? t1y : t0y;
    const float nz = (t0z < t1z) ? t0z : t1z, fz = (t0z < t1z) ? t1z : t0z;
    float tNear = (nx > ny) ? nx : ny;
    tNear = (tNear > nz) ? tNear : nz;
    tNear = (tNear > 0.0f) ? tNear : 0.0f;
    float tFar = (fx < fy) ? fx : fy;
    tFar = (tFar < fz) ? tFar : fz;
    tFar = (tFar < tMax) ? tFar : tMax;
    return tNear <= tFar;
}

// Möller-Trumbore (両面)。Physics の shapes::RayTri と同じ式を辺の前計算済み形で。
// HLSL の RtRayTri と同一式
inline bool RtRayTri(const DirectX::XMFLOAT3& ro, const DirectX::XMFLOAT3& rd, const RtTri& tri,
                     float& outT, float& outU, float& outV)
{
    using namespace DirectX;
    const XMVECTOR o = XMLoadFloat3(&ro), d = XMLoadFloat3(&rd);
    const XMVECTOR p0 = XMLoadFloat3(&tri.p0);
    const XMVECTOR e1 = XMLoadFloat3(&tri.e1), e2 = XMLoadFloat3(&tri.e2);
    const XMVECTOR pv = XMVector3Cross(d, e2);
    const float det = XMVectorGetX(XMVector3Dot(e1, pv));
    if (det > -1e-12f && det < 1e-12f) {
        return false; // レイと三角形が平行
    }
    const float inv = 1.0f / det;
    const XMVECTOR tv = XMVectorSubtract(o, p0);
    const float u = XMVectorGetX(XMVector3Dot(tv, pv)) * inv;
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const XMVECTOR qv = XMVector3Cross(tv, e1);
    const float v = XMVectorGetX(XMVector3Dot(d, qv)) * inv;
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = XMVectorGetX(XMVector3Dot(e2, qv)) * inv;
    if (t <= 0.0f) {
        return false;
    }
    outT = t;
    outU = u;
    outV = v;
    return true;
}

// ---- サンプリング (HLSL の同名関数と一致。変更時は両方更新) ----

// PCG3D ハッシュ (状態レス)。同じ入力からは常に同じ値 = スクリーンショットの決定性が保てる
struct RtSeed {
    uint32_t x = 0, y = 0, z = 0;
};

inline RtSeed RtPcg3d(RtSeed v)
{
    v.x = v.x * 1664525u + 1013904223u;
    v.y = v.y * 1664525u + 1013904223u;
    v.z = v.z * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x ^= v.x >> 16u;
    v.y ^= v.y >> 16u;
    v.z ^= v.z >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

// 呼ぶたびに seed.z を進める (HLSL の RtNextRand2 と同一)
inline DirectX::XMFLOAT2 RtNextRand2(RtSeed& seed)
{
    seed.z += 1u;
    const RtSeed h = RtPcg3d(seed);
    constexpr float kInv2p32 = 2.3283064365386963e-10f; // 1 / 2^32
    return { static_cast<float>(h.x) * kInv2p32, static_cast<float>(h.y) * kInv2p32 };
}

// コサイン重点サンプリング (法線半球、pdf = cos/PI)。
// 基底は Duff らの分岐なし ONB。HLSL の RtCosineHemisphere と同一式
inline DirectX::XMFLOAT3 RtCosineHemisphere(const DirectX::XMFLOAT3& n,
                                            const DirectX::XMFLOAT2& u)
{
    using namespace DirectX;
    const float r = std::sqrt(u.x);
    const float phi = 6.28318530718f * u.y;
    const float sgn = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + n.z);
    const float b = n.x * n.y * a;
    const XMFLOAT3 t1 = { 1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x };
    const XMFLOAT3 t2 = { b, sgn + n.y * n.y * a, -n.y };
    const float c1 = r * std::cos(phi);
    const float c2 = r * std::sin(phi);
    const float c3 = std::sqrt((std::max)(0.0f, 1.0f - u.x));
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(XMVectorSet(
                            t1.x * c1 + t2.x * c2 + n.x * c3, t1.y * c1 + t2.y * c2 + n.y * c3,
                            t1.z * c1 + t2.z * c2 + n.z * c3, 0.0f)));
    return out;
}

// ---- M46g: RT 影 (HLSL の rt_common.hlsli / rt_shadow.cs.hlsl と一致。両方更新) ----

// 半頂角 (度) → cos。1 = 点光源 (完全に硬い影)。CPU 側だけで使い CB へ渡す
inline float RtConeCosMax(float angleDeg)
{
    constexpr float kDegToRad = 0.01745329252f;
    constexpr float kPi = 3.14159265358979f;
    float a = angleDeg * kDegToRad;
    if (a < 0.0f) {
        a = 0.0f;
    }
    if (a > kPi) {
        a = kPi;
    }
    return std::cos(a);
}

// 円錐 (半頂角 acos(cosMax)) の内側を立体角に対して一様にサンプルする。
// cosMax = 1 で dir そのもの。基底は RtCosineHemisphere と同じ Duff らの分岐なし ONB。
// HLSL の RtSampleCone と同一式
inline DirectX::XMFLOAT3 RtSampleCone(const DirectX::XMFLOAT3& dir, float cosMax,
                                      const DirectX::XMFLOAT2& u)
{
    using namespace DirectX;
    const float cosT = cosMax + u.x * (1.0f - cosMax);
    const float sinT = std::sqrt((std::max)(0.0f, 1.0f - cosT * cosT));
    const float phi = 6.28318530718f * u.y;
    const float sgn = (dir.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + dir.z);
    const float b = dir.x * dir.y * a;
    const XMFLOAT3 t1 = { 1.0f + sgn * dir.x * dir.x * a, sgn * b, -sgn * dir.x };
    const XMFLOAT3 t2 = { b, sgn + dir.y * dir.y * a, -dir.y };
    const float c1 = sinT * std::cos(phi);
    const float c2 = sinT * std::sin(phi);
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(XMVectorSet(
                            t1.x * c1 + t2.x * c2 + dir.x * cosT,
                            t1.y * c1 + t2.y * c2 + dir.y * cosT,
                            t1.z * c1 + t2.z * c2 + dir.z * cosT, 0.0f)));
    return out;
}

// 可視点から二次光線を撃つときの原点オフセット量 (影 M46g / 反射 M46h 共通)。
// G-Buffer のワールド座標が半精度なので距離に比例させる
// (定数だけだと遠景でアクネ、大きすぎるとピーターパン)。HLSL の eps 計算と同一式
inline float RtSurfaceRayEps(float dist)
{
    const float e = kRtSurfaceEpsRel * dist;
    return (e > kRtSurfaceEpsMin) ? e : kRtSurfaceEpsMin;
}

// ---- M46h: RT 反射 (HLSL の rt_common.hlsli / rt_refl.cs.hlsl と一致。両方更新) ----

// GGX の可視法線分布 (VNDF) サンプリング (Heitz 2018, JCGT)。
// n = ワールド法線 / v = 面 → カメラ / alpha = roughness² / 戻り値 = half vector。
// alpha = 0 で n そのもの (完全鏡面) へ退化する。HLSL の RtGgxVndf と同一式
inline DirectX::XMFLOAT3 RtGgxVndf(const DirectX::XMFLOAT3& n, const DirectX::XMFLOAT3& v,
                                   float alpha, const DirectX::XMFLOAT2& u)
{
    using namespace DirectX;
    // n を z 軸とする正規直交基底 (Duff らの分岐なし ONB)
    const float sgn = (n.z >= 0.0f) ? 1.0f : -1.0f;
    const float a = -1.0f / (sgn + n.z);
    const float b = n.x * n.y * a;
    const XMFLOAT3 t1 = { 1.0f + sgn * n.x * n.x * a, sgn * b, -sgn * n.x };
    const XMFLOAT3 t2 = { b, sgn + n.y * n.y * a, -n.y };

    // 視線を接空間へ → 楕円体を半球へ変形
    const float vex = v.x * t1.x + v.y * t1.y + v.z * t1.z;
    const float vey = v.x * t2.x + v.y * t2.y + v.z * t2.z;
    const float vez = v.x * n.x + v.y * n.y + v.z * n.z;
    XMFLOAT3 vh;
    XMStoreFloat3(&vh, XMVector3Normalize(XMVectorSet(alpha * vex, alpha * vey, vez, 0.0f)));
    // 投影面積の正規直交基底 (vh が z 軸に一致するときの特異点を分岐で回避)
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    XMFLOAT3 h1 = { 1.0f, 0.0f, 0.0f };
    if (lensq > 1e-12f) {
        const float inv = 1.0f / std::sqrt(lensq);
        h1 = { -vh.y * inv, vh.x * inv, 0.0f };
    }
    const XMFLOAT3 h2 = { vh.y * h1.z - vh.z * h1.y, vh.z * h1.x - vh.x * h1.z,
                          vh.x * h1.y - vh.y * h1.x };
    // 単位円板の一様サンプルを可視半球の投影 (楕円) へ切り詰める
    const float r = std::sqrt(u.x);
    const float phi = 6.28318530718f * u.y;
    const float p1 = r * std::cos(phi);
    float p2 = r * std::sin(phi);
    const float s = 0.5f * (1.0f + vh.z);
    p2 = (1.0f - s) * std::sqrt((std::max)(0.0f, 1.0f - p1 * p1)) + s * p2;
    // 半球へ持ち上げ → 楕円体へ戻す
    const float p3 = std::sqrt((std::max)(0.0f, 1.0f - p1 * p1 - p2 * p2));
    const XMFLOAT3 nh = { p1 * h1.x + p2 * h2.x + p3 * vh.x, p1 * h1.y + p2 * h2.y + p3 * vh.y,
                          p1 * h1.z + p2 * h2.z + p3 * vh.z };
    XMFLOAT3 ne;
    XMStoreFloat3(&ne, XMVector3Normalize(XMVectorSet(alpha * nh.x, alpha * nh.y,
                                                      (std::max)(1e-6f, nh.z), 0.0f)));
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Normalize(XMVectorSet(
                            ne.x * t1.x + ne.y * t2.x + ne.z * n.x,
                            ne.x * t1.y + ne.y * t2.y + ne.z * n.y,
                            ne.x * t1.z + ne.y * t2.z + ne.z * n.z, 0.0f)));
    return out;
}

// レイトレ反射を IBL スペキュラへ混ぜる重み (1 = 反射 100% / 0 = IBL 100%)。
// kRtReflFadeStart から kRtReflMaxRoughness まで smoothstep で落とす。
// common.hlsli::RtReflWeight と同一式 (合成の段差が出ないことをここで担保する)
inline float RtReflWeight(float roughness)
{
    const float lo = kRtReflFadeStart;
    const float hi = kRtReflMaxRoughness;
    float t = (roughness - lo) / ((hi > lo) ? (hi - lo) : 1e-4f);
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return 1.0f - t * t * (3.0f - 2.0f * t); // 1 - smoothstep
}

// ---- M46d: テンポラル蓄積 (HLSL の rt_temporal.cs.hlsl と一致。変更時は両方更新) ----

// 前フレームのクリップ座標 → 履歴バッファの UV。
// 背後 (w<=0) と画面外は false = 履歴なし。HLSL の RtClipToPrevUv と同一式
inline bool RtClipToPrevUv(const DirectX::XMFLOAT4& clip, DirectX::XMFLOAT2& outUv)
{
    outUv = { 0.0f, 0.0f };
    if (clip.w <= 1e-6f) {
        return false; // 前フレームのカメラの背後 (または退化)
    }
    const float nx = clip.x / clip.w;
    const float ny = clip.y / clip.w;
    outUv = { nx * 0.5f + 0.5f, ny * -0.5f + 0.5f };
    return outUv.x >= 0.0f && outUv.x < 1.0f && outUv.y >= 0.0f && outUv.y < 1.0f;
}

// M55f: 履歴 UV をどちらの経路で作るか。
//   useVelocity = true  → prevUv = uv - velocity (GBuffer RT4。カメラ + 物体の運動が入っている)
//   useVelocity = false → prevClip (= 現在の可視点を前フレーム VP で射影) へ縮退 (M46d のまま)
// 画面外の棄却は 2 経路で同じ規約。HLSL の RtHistoryUv と同一式
inline bool RtHistoryUv(bool useVelocity, const DirectX::XMFLOAT2& uv,
                        const DirectX::XMFLOAT2& velocity, const DirectX::XMFLOAT4& prevClip,
                        DirectX::XMFLOAT2& outUv)
{
    if (useVelocity) {
        outUv = { uv.x - velocity.x, uv.y - velocity.y };
        return outUv.x >= 0.0f && outUv.x < 1.0f && outUv.y >= 0.0f && outUv.y < 1.0f;
    }
    return RtClipToPrevUv(prevClip, outUv);
}

// 再投影先の履歴が現在の面と同じものか。
// expectedDepth = 現在の可視点を前フレームのカメラから測った距離、
// storedDepth   = 履歴バッファがそのピクセルに記録している距離 (0 以下 = 未記録)。
// HLSL の RtReprojectValid と同一式
inline bool RtReprojectValid(float expectedDepth, float storedDepth,
                             const DirectX::XMFLOAT3& n, const DirectX::XMFLOAT3& prevN,
                             float depthThreshold, float normalThreshold)
{
    if (!(storedDepth > 0.0f) || !(expectedDepth > 0.0f)) {
        return false;
    }
    const float d = std::fabs(expectedDepth - storedDepth);
    if (d > depthThreshold * (std::max)(expectedDepth, 1e-3f)) {
        return false; // 別の面が手前/奥にある (disocclusion)
    }
    const float c = n.x * prevN.x + n.y * prevN.y + n.z * prevN.z;
    return c >= normalThreshold;
}

// 履歴長を 1 進める。無効なら 1 (= 今フレームの 1spp をそのまま採用) に若返る。
// HLSL の RtAdvanceHistory と同一式
inline float RtAdvanceHistory(float prevLen, bool valid, float maxLen)
{
    const float base = valid ? prevLen : 0.0f;
    return (std::min)(base + 1.0f, maxLen);
}

// 移動平均の重み (新サンプルの寄与)。履歴長 1 で 1.0 = 履歴を使わない。
// HLSL の RtTemporalAlpha と同一式
inline float RtTemporalAlpha(float histLen)
{
    return 1.0f / (std::max)(histLen, 1.0f);
}

// ---- M46e: SVGF (HLSL の rt_variance.cs.hlsl / rt_atrous.cs.hlsl と一致。両方更新) ----

// 輝度 (Rec.709)。ポスプロ (postfx_tonemap.hlsl 等) と同じ係数
inline float RtLuminance(const DirectX::XMFLOAT3& c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

// 1 サンプルあたりの分散 (μ² から μ の 2 乗を引く)。丸めで負に落ちるので 0 で止める。
// HLSL の RtVarianceFromMoments と同一式
inline float RtVarianceFromMoments(float m1, float m2)
{
    const float v = m2 - m1 * m1;
    return (v > 0.0f) ? v : 0.0f;
}

// サンプル分散 → 蓄積後の推定値の分散。N 個の平均の分散は 1/N になるので履歴長で割る。
// SVGF 原論文はサンプル分散をそのまま使うが、それだと収束後もぼけ続けて GI の
// コンタクト陰影が溶ける。forceSpatial (シード凍結 = 毎フレーム同じ 1 サンプル) は
// いくら履歴が伸びても実効サンプル数が 1 なので割らない。
// HLSL の RtVarianceEstimate と同一式
inline float RtVarianceEstimate(float sampleVar, float histLen, bool forceSpatial)
{
    if (forceSpatial) {
        return sampleVar;
    }
    return sampleVar / ((histLen > 1.0f) ? histLen : 1.0f);
}

// 深度 (カメラ距離) のエッジ停止重み。真の深度勾配を持たないので
// 「タップが遠いほど許容を広げる」相対差で近似する (平面上で重みが落ちないように)。
// HLSL の RtAtrousDepthWeight と同一式
inline float RtAtrousDepthWeight(float zc, float zq, float tapDist, float sigma)
{
    const float tol = sigma * ((zc > 1e-3f) ? zc : 1e-3f) * ((tapDist > 1.0f) ? tapDist : 1.0f);
    return std::exp(-std::fabs(zc - zq) / ((tol > 1e-6f) ? tol : 1e-6f));
}

// 法線のエッジ停止重み (cos の冪)。裏向きは 0。HLSL の RtAtrousNormalWeight と同一式
inline float RtAtrousNormalWeight(const DirectX::XMFLOAT3& nc, const DirectX::XMFLOAT3& nq,
                                  float power)
{
    const float c = nc.x * nq.x + nc.y * nq.y + nc.z * nq.z;
    return std::pow((c > 0.0f) ? c : 0.0f, power);
}

// 輝度のエッジ停止重み。推定標準偏差でスケールするので、ノイズが乗っている間は
// 緩く (よくぼける)、収束すると厳しく (エッジが残る)。HLSL の RtAtrousLumaWeight と同一式
inline float RtAtrousLumaWeight(float lc, float lq, float variance, float sigma)
{
    const float sd = std::sqrt((variance > 0.0f) ? variance : 0.0f);
    return std::exp(-std::fabs(lc - lq) / (sigma * sd + 1e-4f));
}

// A-Trous の 1 次元カーネル (B3 スプライン (1,4,6,4,1)/16)。d は中心からのタップ番号。
// HLSL の RtAtrousKernel と同一値
inline float RtAtrousKernel(int d)
{
    const int i = (d < 0) ? -d : d;
    switch (i) {
    case 0:
        return 6.0f / 16.0f;
    case 1:
        return 4.0f / 16.0f;
    case 2:
        return 1.0f / 16.0f;
    default:
        return 0.0f;
    }
}

// ---- M67: ReSTIR 反射 (HLSL の rt_restir_common.hlsli と一致。**変更時は両方更新**) ----
//
// 推定対象は現行 (M46h) と同じ「VNDF 方向の入射放射輝度の期待値」なので、
// 出力の次元は変わらない = 合成側 (common.hlsli::RtReflWeight の混色) は不変。
// M = 1 (再利用なし) のとき RtRestirResolve が Ls をそのまま返すことが、
// 「ReSTIR off = 現行とビット一致」の数学的な根拠になっている。
// **この節は RtLuminance に依存するので SVGF 節より後ろに置いてある**
// (前に置くと前方参照になる)。

// reservoir が保持する 1 サンプルと統計。GPU 側は 3 枚のテクスチャに詰めて運ぶ
// (rt_restir_common.hlsli の struct RtReservoir と同じ並び)。
// 受け側の情報 (geom / rpos) は reservoir ではなく別テクスチャで持つ —
// 「どこから借りたか」は再利用の判定にしか使わず、サンプルそのものではないため
struct RtReservoirCpu {
    DirectX::XMFLOAT3 xs = { 0, 0, 0 }; // first hit のワールド座標 (スカイは方向ベクトル)
    float W = 0.0f;                     // = wSum / (M * pHat(y))
    DirectX::XMFLOAT3 Ls = { 0, 0, 0 }; // ヒット点から出た放射輝度 (バウンス込み)
    float M = 0.0f;                     // 統合したサンプル数 (0 = 空)
    DirectX::XMFLOAT3 ns = { 0, 0, 0 }; // ヒット点の法線 (**ゼロ = スカイのセンチネル**)
    int32_t cls = -1;                   // ReflectionClass (空 = -1 = 範囲外 = 表示は黒)
};

// 空の reservoir。cls は **-1 (範囲外)** — 4 (Default) にすると
// デバッグ表示で「何も入っていない画素」と「クラス 4 の物体」が同じ灰色になる
inline RtReservoirCpu RtReservoirEmpty()
{
    return RtReservoirCpu{};
}

// GGX VNDF サンプリング (RtGgxVndf) の pdf。**立体角 (方向 l) に対する密度**で、
// Heitz 2018 の可視法線分布 D_vis(h) に半ベクトル → 反射方向のヤコビアン
// 1/(4 (v·h)) を掛けたもの: G1(v) * D(h) / (4 (n·v))。
//   n = 面法線 / v = 面 → カメラ / l = 面 → 反射先 / alpha = roughness²
// alpha は kRtRestirAlphaMin で下から留める (alpha=0 のデルタ分布は pdf が発散する)。
// ★半球で積分すると 1 ではなく「1 − 下半球へ抜けた分」になる (alpha=0.36 で約 0.885) —
//   VNDF は半ベクトル側で正規化されており、反射後に地平線の下へ回った分は
//   pdf の定義域から外れるため。selftest はこの関係そのものを検査する。
// HLSL の RtGgxVndfPdf と同一式
inline float RtGgxVndfPdf(const DirectX::XMFLOAT3& n, const DirectX::XMFLOAT3& v,
                          const DirectX::XMFLOAT3& l, float alpha)
{
    constexpr float kPi = 3.14159265358979f;
    const float a = (alpha > kRtRestirAlphaMin) ? alpha : kRtRestirAlphaMin;
    const float ndotv = n.x * v.x + n.y * v.y + n.z * v.z;
    const float ndotl = n.x * l.x + n.y * l.y + n.z * l.z;
    if (ndotl <= 0.0f || ndotv <= 1e-6f) {
        return 0.0f; // 面の裏 / 視線が面と平行 (G1/(4 n·v) が 0/0 になる)
    }
    const DirectX::XMFLOAT3 hv = { v.x + l.x, v.y + l.y, v.z + l.z };
    const float hlen = std::sqrt(hv.x * hv.x + hv.y * hv.y + hv.z * hv.z);
    if (hlen <= 1e-8f) {
        return 0.0f; // v と l が正反対 (半ベクトルが定義できない)
    }
    float ndoth = (n.x * hv.x + n.y * hv.y + n.z * hv.z) / hlen;
    ndoth = (ndoth < 0.0f) ? 0.0f : ((ndoth > 1.0f) ? 1.0f : ndoth);
    const float a2 = a * a;
    const float dd = ndoth * ndoth * (a2 - 1.0f) + 1.0f;
    const float ggxD = a2 / (kPi * dd * dd);
    const float g1 = 2.0f * ndotv / (ndotv + std::sqrt(a2 + (1.0f - a2) * ndotv * ndotv));
    return g1 * ggxD / (4.0f * ndotv);
}

// ReSTIR の target function p̂_q(y)。「この受け側画素にとってこのサンプルがどれだけ
// 効くか」を 1 本のスカラーで表す = 再利用の重み付けの基準。
// 輝度 × VNDF pdf にしてあるので、初期サンプル (ソース pdf = VNDF) では
// w = p̂/p = lum(Ls) に約分される = M=1 で現行と一致する形になる。
// HLSL の RtRestirTargetPdf と同一式
inline float RtRestirTargetPdf(const DirectX::XMFLOAT3& Ls, const DirectX::XMFLOAT3& L,
                               const DirectX::XMFLOAT3& V, const DirectX::XMFLOAT3& N,
                               float alpha)
{
    const float lum = RtLuminance(Ls);
    if (lum <= 0.0f) {
        return 0.0f; // 真っ黒なサンプルは誰の役にも立たない (借りても絵が変わらない)
    }
    return lum * RtGgxVndfPdf(N, V, L, alpha);
}

// unbiased contribution weight W = wSum / (M * p̂(y))。**テクスチャへ書き戻すのはこの W**
// (wSum ではなく) — 統合の式 (RtReservoirMerge) が教科書形 `p̂ * W * M * J` のままになる。
// HLSL の RtRestirWeight と同一式
inline float RtRestirWeight(float wSum, float M, float pHat)
{
    if (M <= 0.0f || pHat <= 0.0f) {
        return 0.0f;
    }
    return wSum / (M * pHat);
}

// streaming RIS の 1 手。重み w の候補を確率 w/wSum で採用し、M は**重みに関わらず**進める。
// ★ここは**自画素のサンプル専用**の入口 — 真っ黒 (lum = 0 → w = 0) でも「1 本撃った」事実は
//   変わらないので M = 1 になる。**ここに w > 0 のゲートを足さないこと**
//   (足すと黒い画素の M が 0 に落ち、resolve が 0 を返し続ける)。
//   借りてきた候補を「候補から外す」判定は RtReservoirMerge の仕事。
// rnd は [0,1)。HLSL の RtReservoirUpdate と同一式
inline bool RtReservoirUpdate(RtReservoirCpu& r, float& wSum, const DirectX::XMFLOAT3& xs,
                              const DirectX::XMFLOAT3& ns, const DirectX::XMFLOAT3& Ls,
                              int32_t cls, float mInc, float w, float rnd)
{
    r.M += mInc;
    if (!(w > 0.0f)) {
        return false; // 0 と NaN をまとめて弾く (NaN を足すと wSum が二度と戻らない)
    }
    wSum += w;
    if (rnd < w / wSum) {
        r.xs = xs;
        r.ns = ns;
        r.Ls = Ls;
        r.cls = cls;
        return true;
    }
    return false;
}

// 候補 reservoir を 1 つ統合する (temporal / spatial 共通)。
//   w = p̂_q(y') * W' * min(M', mCap) * J
// mCap はクラス別の M 上限 (kRtReflClassTable)、J は受け側が変わったぶんの Jacobian、
// jMax は棄却の閾値 (CB の gRsJacobianMax。関数内の定数にすると temporal だけ緩められない)。
// ★**候補から外したものは M にも数えない** (spec §4.2「M を数える規則」)。外すのは
//   空 reservoir / J が範囲外 / **有効重み w が 0 または非有限** (p̂=0 = ローブ外・受け側の
//   半球外・真っ黒、W'=0) の 3 経路。教科書の biased 変種は p̂=0 でも M を足すが、それだと
//   W = wSum/(M·p̂) が縮んで**暗化**する — 粗さ 0.10 の鏡面 (α=0.01、ローブ幅 ≈ 1°) では
//   半径 8px の候補の大半が p̂ ≈ 0 なので、鏡面パッチが目に見えて暗くなる。数えない側の
//   偏りは「わずかに明るい / 分散が減らない」= 鏡面のディテールを守る向き。
//   **自画素の初期サンプルは別扱い** (RtReservoirUpdate を直接呼ぶので w=0 でも M=1)。
// HLSL の RtReservoirMerge と同一式
inline bool RtReservoirMerge(RtReservoirCpu& r, float& wSum, const RtReservoirCpu& cand,
                             float pHatAtQ, float mCap, float J, float jMax, float rnd)
{
    if (!(cand.M > 0.0f)) {
        return false; // 空 reservoir は候補にならない
    }
    if (!(J >= 1.0f / jMax && J <= jMax)) {
        return false; // NaN もここで落ちる (比較が両方 false になる)
    }
    const float mInc = (cand.M < mCap) ? cand.M : mCap;
    const float w = pHatAtQ * cand.W * mInc * J;
    // 有効重みが 0 / 非有限なら候補から外す (M も wSum も動かさない)。
    // ★`isfinite()` ではなく上限との比較で書く — fxc は /Gis 抜きだと
    //   「値が無限になることは無い」前提で isfinite() を消しうる (警告 X3577) ので、
    //   **GPU でも実際に実行される形**にそろえる。NaN も両方の比較に落ちる
    const float kWeightMax = 1e30f;
    if (!(w > 0.0f) || !(w < kWeightMax)) {
        return false;
    }
    return RtReservoirUpdate(r, wSum, cand.xs, cand.ns, cand.Ls, cand.cls, mInc, w, rnd);
}

// 受け側が P_from から P_to へ変わったときの立体角の伸縮 (Jacobian)。
//   J = (cosθ_to / cosθ_from) * (d_from² / d_to²)、d = |xs − P|、cosθ = |ns·(P−xs)/d|
// temporal (P_from = 前フレームの受け側) と spatial (P_from = 近傍画素) で共通。
// スカイ (ns = 0) は無限遠の方向サンプルなので伸縮しない = 1。
// HLSL の RtRestirJacobian と同一式
inline float RtRestirJacobian(const DirectX::XMFLOAT3& xs, const DirectX::XMFLOAT3& ns,
                              const DirectX::XMFLOAT3& pFrom, const DirectX::XMFLOAT3& pTo)
{
    constexpr float kEps = 1e-6f;
    if (ns.x * ns.x + ns.y * ns.y + ns.z * ns.z <= 0.0f) {
        return 1.0f; // スカイのセンチネル
    }
    const DirectX::XMFLOAT3 df = { pFrom.x - xs.x, pFrom.y - xs.y, pFrom.z - xs.z };
    const DirectX::XMFLOAT3 dt = { pTo.x - xs.x, pTo.y - xs.y, pTo.z - xs.z };
    const float lenF = std::sqrt(df.x * df.x + df.y * df.y + df.z * df.z);
    const float lenT = std::sqrt(dt.x * dt.x + dt.y * dt.y + dt.z * dt.z);
    if (lenF <= kEps || lenT <= kEps) {
        return 1.0f; // 受け側がヒット点に重なった (退化。棄却せず素通しする)
    }
    float cosF = std::fabs(ns.x * df.x + ns.y * df.y + ns.z * df.z) / lenF;
    const float cosT = std::fabs(ns.x * dt.x + ns.y * dt.y + ns.z * dt.z) / lenT;
    cosF = (cosF > kEps) ? cosF : kEps; // 0 除算を避ける (真横から見た面 → J が巨大 → 棄却)
    return (cosT / cosF) * ((lenF * lenF) / (lenT * lenT));
}

// 書き戻し前に M をクラスの上限へ切り詰める。wSum を同じ比率で縮めるので
// **W = wSum/(M·p̂) は変わらない = 出力される絵は変わらない**。
// 変わるのは「次のフレームがこの reservoir をどれだけ重く扱うか」だけ。
// HLSL の RtRestirClampM と同一式
inline void RtRestirClampM(RtReservoirCpu& r, float& wSum, float mCap)
{
    if (r.M > mCap && r.M > 0.0f) {
        wSum *= mCap / r.M;
        r.M = mCap;
    }
}

// M67f: 空間再利用の半径を受け側の α で縮める係数 (0〜1)。実効半径 = radius[cls] * これ。
// p̂ (VNDF pdf) のローブ幅は α に比例するので、滑らかな面ほど円板を小さくしないと
// 「ローブの外のタップが増えるだけ」になり、暗化とフリッカーを増やす。
// α <= 0 / ref <= 0 / NaN は 0 (= タップしない)。
// HLSL の RtRestirRadiusScale と同一式
inline float RtRestirRadiusScale(float alpha, float ref)
{
    if (!(alpha > 0.0f) || !(ref > 0.0f)) {
        return 0.0f;
    }
    return (std::min)(1.0f, alpha / ref);
}

// M67f: 空間再利用のタップ位置 (半径 radius の円板上の Vogel 螺旋、i 番目 / 全 count 点)。
//   r = radius * sqrt((i + 0.5) / count)  … 面積が均等になる半径の配り方
//   θ = i * 黄金角 + rotation             … 隣り合う点が同じ方角に並ばない回し方
// **rotation を画素ごとに変える**のが要で、全画素が同じ配置だとタップの偏りが
// 「格子状のまだら」として絵に固定される (時間再利用と違い空間再利用は平均されない)。
// count = 0 で呼ばれても 0 除算しないよう max(1) を噛ませる。
// HLSL の RtRestirVogelTap と同一式
inline DirectX::XMFLOAT2 RtRestirVogelTap(int i, int count, float radius, float rotation)
{
    const float kGoldenAngle = 2.39996323f; // π(3 − √5)
    const float r = radius
        * std::sqrt((static_cast<float>(i) + 0.5f)
                    / (std::max)(static_cast<float>(count), 1.0f));
    const float a = static_cast<float>(i) * kGoldenAngle + rotation;
    return { r * std::cos(a), r * std::sin(a) };
}

// reservoir → 出力放射輝度。out = Ls * wSum / (M * lum(Ls))。
// ★**Ls を先に掛けない** — scale を先に求めることで M=1 (wSum = lum) のとき
//   scale が厳密に 1.0f になり、Ls がビット単位でそのまま出る (A5 の根拠)。
//   (Ls*wSum)/(M*lum) の順だと丸めが 2 回入って 1 ulp ずれうる。
// HLSL の RtRestirResolve と同一式
inline DirectX::XMFLOAT3 RtRestirResolve(const RtReservoirCpu& r, float wSum)
{
    if (!(r.M > 0.0f)) {
        return { 0.0f, 0.0f, 0.0f };
    }
    const float lum = RtLuminance(r.Ls);
    if (!(lum > 0.0f)) {
        return { 0.0f, 0.0f, 0.0f };
    }
    const float scale = wSum / (r.M * lum);
    return { r.Ls.x * scale, r.Ls.y * scale, r.Ls.z * scale };
}

// BLAS (単一メッシュ) のヒット結果。tri は連結三角形配列の絶対 index
struct RtBlasHit {
    float t = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    int32_t tri = -1;
    int32_t visited = 0; // 訪問ノード数 (ヒートマップ / 打ち切り判定)
};

// 1 つの BLAS をスタックで走査して最近ヒットを返す (ローカル空間)。
// nodes/tris は連結配列、root はその中の BLAS ルート index。
// HLSL の RtTraceBlas と同一ロジック — 走査順・打ち切り条件を変えないこと
inline bool RtTraceBlasCpu(const std::vector<RtBvhNode>& nodes, const std::vector<RtTri>& tris,
                           int32_t root, const DirectX::XMFLOAT3& ro,
                           const DirectX::XMFLOAT3& rd, RtBlasHit& hit)
{
    if (nodes.empty() || root < 0 || root >= static_cast<int32_t>(nodes.size())) {
        return false;
    }
    const DirectX::XMFLOAT3 invD = { RtSafeInv(rd.x), RtSafeInv(rd.y), RtSafeInv(rd.z) };
    int32_t stack[kRtStackDepth];
    int top = 0;
    stack[top++] = root;
    bool found = false;
    while (top > 0) {
        if (hit.visited >= kRtMaxVisit) {
            break; // TDR 保険 (HLSL と同条件)
        }
        const RtBvhNode& node = nodes[static_cast<size_t>(stack[--top])];
        ++hit.visited;
        if (!RtSlabTest(node.aabbMin, node.aabbMax, ro, invD, hit.t)) {
            continue;
        }
        if (node.left < 0) { // 葉: 三角形の連続範囲
            const int32_t start = -node.left - 1;
            for (int32_t i = 0; i < node.right; ++i) {
                const int32_t ti = start + i;
                float t = 0.0f, u = 0.0f, v = 0.0f;
                if (RtRayTri(ro, rd, tris[static_cast<size_t>(ti)], t, u, v) && t < hit.t) {
                    hit.t = t;
                    hit.u = u;
                    hit.v = v;
                    hit.tri = ti;
                    found = true;
                }
            }
            continue;
        }
        if (top + 2 <= kRtStackDepth) {
            stack[top++] = node.right; // 固定順 (right を先に積む = left 先行の DFS)
            stack[top++] = node.left;
        }
    }
    return found;
}

} // namespace mye
