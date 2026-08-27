#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Random.h"          // M61b: 形状サンプリング (Pcg32 消費列の正本)
#include "Engine/Renderer/FrustumCull.h" // プール単位カリング (描画専用 — ハッシュ非関与)
#include "Engine/Renderer/PostFxMath.h"  // M55a: LinearizeDepth (CPU ミラーの正本)

// パーティクルの放出計画 (burst/duration/loop/playing) と多点グラデーション評価を
// CPU/GPU 両バックエンドと selftest で共有する純関数群 (D3D 非依存)。
// - 放出計画は決定論の核: ageTicks/emitAccum を副作用として進め、この tick の放出希望数を返す。
//   cap (maxParticles / GPU 容量) は呼び出し側が適用する。
// - グラデーションは描画専用 (中間キーが無ければ従来の 2 点線形とビット同一)。
namespace mye {

// この tick の放出希望数 (cap 前) を返し、ageTicks/emitAccum を進める。
// 放出順の意味: (1) ウィンドウ先頭 burst → (2) スクリプト起因 pendingBurst → (3) rate 蓄積。
// 返り値は総数のみ (全粒子は同一の per-particle RNG 消費列で生成されるため、
// burst/rate の区別は「先頭優先で cap する」ためだけに使う = 総数 clamp と等価)。
inline int PlanParticleEmission(const ParticleEmitterComponent& d, int32_t& ageTicks,
                                float& emitAccum, float dt)
{
    // 明示バースト (スクリプト/エディタ) は playing に関わらず 1 発通す
    int burst = (d.pendingBurst > 0) ? d.pendingBurst : 0;
    if (!d.playing) {
        return burst; // 連続放出は停止 (ageTicks/emitAccum は凍結)
    }
    bool windowStart = (ageTicks == 0);
    bool emitting = true;
    if (d.durationTicks > 0 && ageTicks >= d.durationTicks) {
        if (d.looping) {
            ageTicks = 0; // ウィンドウを巻き戻して再放出
            windowStart = true;
        } else {
            emitting = false; // ウィンドウ終了 (rate 蓄積もしない)
        }
    }
    if (windowStart && d.burstCount > 0) {
        burst += d.burstCount;
    }
    int rateEmit = 0;
    if (emitting) {
        emitAccum += d.rate * dt;
        rateEmit = static_cast<int>(emitAccum);
        if (rateEmit > 0) {
            emitAccum -= static_cast<float>(rateEmit);
        }
    }
    ++ageTicks;
    return burst + rateEmit;
}

// 寿命係数 age∈[0,1] での色。キー: begin(0) / [colorMid1@T1] / [colorMid2@T2] / end(1)。
// 中間キーは T∈(0,1) のときのみ有効 (0 は無効)。無効時は begin→end の線形とビット同一。
inline DirectX::XMFLOAT4 EvalParticleColor(const ParticleEmitterComponent& d, float age)
{
    age = std::clamp(age, 0.0f, 1.0f);
    struct Key {
        float t;
        DirectX::XMFLOAT4 c;
    };
    Key keys[4];
    int n = 0;
    keys[n++] = { 0.0f, d.colorBegin };
    if (d.colorMidT1 > 0.0f && d.colorMidT1 < 1.0f) {
        keys[n++] = { d.colorMidT1, d.colorMid1 };
    }
    if (d.colorMidT2 > 0.0f && d.colorMidT2 < 1.0f) {
        keys[n++] = { d.colorMidT2, d.colorMid2 };
    }
    keys[n++] = { 1.0f, d.colorEnd };
    // T 昇順に挿入ソート (n<=4、安定)
    for (int i = 1; i < n; ++i) {
        for (int j = i; j > 0 && keys[j].t < keys[j - 1].t; --j) {
            std::swap(keys[j], keys[j - 1]);
        }
    }
    for (int i = 0; i + 1 < n; ++i) {
        if (age <= keys[i + 1].t) {
            const float span = keys[i + 1].t - keys[i].t;
            const float f = (span > 1e-6f) ? (age - keys[i].t) / span : 0.0f;
            const DirectX::XMFLOAT4& a = keys[i].c;
            const DirectX::XMFLOAT4& b = keys[i + 1].c;
            return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f,
                     a.w + (b.w - a.w) * f };
        }
    }
    return keys[n - 1].c;
}

// M42b: ソフトパーティクル。非線形深度 [0,1] -> ビュー空間 z (透視投影)。
// M55a: 式そのものは Renderer/PostFxMath.h::LinearizeDepth へ移した (HLSL 側も
// common.hlsli の共有版に一本化済み)。ここは既存の呼び出し元と selftest のための別名。
// PostFxMath.h は D3D 非依存の純数式ヘッダなので、Engine → Renderer の依存方向に沿う。
inline float LinearizeParticleDepth(float d, float nearZ, float farZ)
{
    return LinearizeDepth(d, nearZ, farZ);
}

// 深度フェード係数 [0,1]。fadeDist<=0 = 無効 (1.0 = 従来とビット同一)。
// 粒子がシーン深度より奥 (sceneLinZ < particleLinZ) なら 0 = 完全に消える
inline float SoftFadeFactor(float sceneLinZ, float particleLinZ, float fadeDist)
{
    if (fadeDist <= 0.0f) {
        return 1.0f;
    }
    return std::clamp((sceneLinZ - particleLinZ) / std::max(fadeDist, 1e-4f), 0.0f, 1.0f);
}

// M42e: GPU 深度衝突の座標変換/反射。particle_sim.cs.hlsl とコメント同期のミラー —
// selftest はこちらを検証する。GPU バックエンド限定の見た目効果 (spec 7.5 例外)。
// クリップ座標 -> スクリーン UV。背面 (w<=0) は false (衝突判定しない)
inline bool ParticleClipToUv(float clipX, float clipY, float clipW, float& u, float& v)
{
    if (clipW <= 0.0f) {
        return false;
    }
    u = (clipX / clipW) * 0.5f + 0.5f;
    v = 0.5f - (clipY / clipW) * 0.5f;
    return true;
}

// 反射 + 反発係数: v' = (v - 2(v·n)n) * restitution
inline DirectX::XMFLOAT3 ReflectWithRestitution(const DirectX::XMFLOAT3& vel,
                                                const DirectX::XMFLOAT3& n, float restitution)
{
    const float d = vel.x * n.x + vel.y * n.y + vel.z * n.z;
    return { (vel.x - 2.0f * d * n.x) * restitution, (vel.y - 2.0f * d * n.y) * restitution,
             (vel.z - 2.0f * d * n.z) * restitution };
}

// 寿命係数 age∈[0,1] でのサイズ倍率。キー: 1.0(0) / [sizeMidScale@sizeMidT] / sizeEndScale(1)。
// sizeMidT∈(0,1) のときのみ中間キー有効。無効時は 1.0→sizeEndScale の線形とビット同一。
inline float EvalParticleSizeScale(const ParticleEmitterComponent& d, float age)
{
    age = std::clamp(age, 0.0f, 1.0f);
    if (d.sizeMidT > 0.0f && d.sizeMidT < 1.0f) {
        if (age <= d.sizeMidT) {
            const float f = age / d.sizeMidT;
            return 1.0f + (d.sizeMidScale - 1.0f) * f;
        }
        const float f = (age - d.sizeMidT) / (1.0f - d.sizeMidT);
        return d.sizeMidScale + (d.sizeEndScale - d.sizeMidScale) * f;
    }
    return 1.0f + (d.sizeEndScale - 1.0f) * age;
}

// ---- プール単位フラスタムカリング (描画専用) ----
// シム更新のカリングは**やらない** — CPU プールはワールドハッシュ対象なので、可視性で
// 更新を止めた瞬間に「カメラの向きがシム結果を変える」= 決定論違反になる。合法なのは
// 描画スキップだけ (FrustumCull.h 冒頭と同じ理屈)。
// AABB はビルボードの張り出し (expand) と 2P 描画オフセットで広げてから p-vertex 保守
// テストへ渡す。跨ぎ/内側は必ず true = 可視な粒子を決して落とさない (落とすと golden が割れる)。
inline bool ParticlePoolVisible(const Frustum& f, const DirectX::XMFLOAT3& bmin,
                                const DirectX::XMFLOAT3& bmax, float expand, float renderOffsetX)
{
    const DirectX::XMFLOAT3 wmin = { bmin.x - expand + renderOffsetX, bmin.y - expand,
                                     bmin.z - expand };
    const DirectX::XMFLOAT3 wmax = { bmax.x + expand + renderOffsetX, bmax.y + expand,
                                     bmax.z + expand };
    return WorldAabbInFrustum(f, wmin, wmax);
}

// ビルボードの AABB 拡張量。size はビルボード半幅 (particle_render.hlsl: corner*size) で
// 対角方向は √2 倍まで届く。サイズカーブは中間/終端キーで初期値を超えうるので最大倍率を
// 掛ける (sizeMidT が無効でも max に含める = 保守側で単純)。1.5 は √2 の保守的丸め。
inline float ParticleBillboardExpand(const ParticleEmitterComponent& d, float maxSize0)
{
    const float maxScale =
        std::max({ 1.0f, std::fabs(d.sizeMidScale), std::fabs(d.sizeEndScale) });
    return maxSize0 * maxScale * 1.5f;
}

// ---- GPU 空 Dispatch 回避の遅延判定 ----
// 「放出 0 かつ推定生存 0」が graceTicks 連続したら true (= その tick の GPU 作業を丸ごと
// 省いてよい)。推定は境界値で ±1 tick ずれる (GpuAliveEstimator 冒頭) ため、猶予中は
// Dispatch を続けて GPU 側の残存粒子を死なせ切る — 最後の実 Dispatch が InstanceCount=0 を
// 確定させてから眠るのが不変量。表示専用の判定 — sim 状態には決して使わないこと。
inline bool StepGpuIdleSkip(int emitCount, uint32_t aliveEstimate, int32_t graceTicks,
                            int32_t& idleTicks)
{
    if (emitCount > 0 || aliveEstimate > 0) {
        idleTicks = 0;
        return false;
    }
    if (idleTicks < graceTicks) {
        ++idleTicks;
        return false;
    }
    return true;
}

// ==== M61b/M61c: 放出系ヘルパ (回転 / 形状サンプリング / サブフレーム補間 / 速度継承) はこの下へ ====

// 放出式で使う π (CpuParticleBackend.cpp の旧 kPi と同じ float 値)。
// ★「u * 2.0f * kParticleEmitPi」のように旧コードと同じ演算列で使うこと — 2π を
//   事前に畳んだ定数へ変えると丸めが 1 ulp 変わり、既存 .rep/golden のビット保存が崩れる
inline constexpr float kParticleEmitPi = 3.14159265358979323846f;

// ---- M61b: エミッタ基底 (ワールド行列の上 3x3 = 回転*スケール) ----
// identity は 9 成分の float 完全一致判定 (完全一致比較は決定論的に安全)。恒等のとき
// 呼び出し側は基底適用を丸ごと飛ばす = 演算列が従来とビット同一 — 既存コンテンツ
// (デモの Fire 含む) のビット保存はこの高速パスで担保する。判定は毎 tick・プール毎で軽量
struct ParticleEmitBasis {
    float m[3][3]; // 行ベクトル規約: v' = v * M (XMVector3TransformNormal と同順)
    bool identity;
};

inline ParticleEmitBasis MakeParticleEmitBasis(const DirectX::XMFLOAT4X4& w)
{
    ParticleEmitBasis b;
    b.m[0][0] = w._11; b.m[0][1] = w._12; b.m[0][2] = w._13;
    b.m[1][0] = w._21; b.m[1][1] = w._22; b.m[1][2] = w._23;
    b.m[2][0] = w._31; b.m[2][1] = w._32; b.m[2][2] = w._33;
    b.identity = (w._11 == 1.0f && w._12 == 0.0f && w._13 == 0.0f
                  && w._21 == 0.0f && w._22 == 1.0f && w._23 == 0.0f
                  && w._31 == 0.0f && w._32 == 0.0f && w._33 == 1.0f);
    return b;
}

// 放出方向へ基底を適用して正規化する。スケール混入で長さが変わるため正規化必須。
// 長さがほぼ 0 (退化基底) は (0,1,0) へフォールバック — RNG は消費しない (決定論)
inline void ParticleBasisRotateDir(const ParticleEmitBasis& b, float& x, float& y, float& z)
{
    const float tx = x * b.m[0][0] + y * b.m[1][0] + z * b.m[2][0];
    const float ty = x * b.m[0][1] + y * b.m[1][1] + z * b.m[2][1];
    const float tz = x * b.m[0][2] + y * b.m[1][2] + z * b.m[2][2];
    const float len = sqrtf(tx * tx + ty * ty + tz * tz);
    if (len > 1e-6f) {
        x = tx / len;
        y = ty / len;
        z = tz / len;
    } else {
        x = 0.0f;
        y = 1.0f;
        z = 0.0f;
    }
}

// origin からの局所オフセットへ基底を適用する (スケール込みの張り出し — 正規化しない)
inline void ParticleBasisTransformOffset(const ParticleEmitBasis& b, float& x, float& y, float& z)
{
    const float tx = x * b.m[0][0] + y * b.m[1][0] + z * b.m[2][0];
    const float ty = x * b.m[0][1] + y * b.m[1][1] + z * b.m[2][1];
    const float tz = x * b.m[0][2] + y * b.m[1][2] + z * b.m[2][2];
    x = tx;
    y = ty;
    z = tz;
}

// ---- M61b: 形状サンプリング (emitFrom = 0:従来 / 1:体積 / 2:表面) ----
// CPU/GPU 両バックエンドの放出ループが共有する (旧: 両者に手写しの switch が重複していた)。
// RNG 消費数の契約 ((shape, emitFrom) の組ごとに個数と順序が固定 — 決定論。変更禁止):
//   point(0):  0 回 (emitFrom 無視)
//   sphere(1): emitFrom=1 (体積) は 3 回 (z, phi, u)。それ以外は 2 回 (z, phi) —
//              表面 (2) は従来 (0) と同一挙動・同一消費 (球の既定が元々表面放出のため)
//   cone(2):   emitFrom=0 は 2 回 (cosT, phi — 従来の apex 点放出)。
//              1/2 は 4 回 (cosT, phi, u, v — 底半径 shapeRadius の円盤からの放出)
//   box(3):    emitFrom=2 (表面) は 3 回 (face, a, b)。それ以外は 3 回 (x, y, z) —
//              体積 (1) は従来 (0) と同一挙動・同一消費 (箱の既定が元々体積放出のため)
// 未知の emitFrom 値は従来 (0) へ倒す = 消費列が既定と一致する安全側。
// hasOffset=false のとき呼び出し側は加算演算そのものを行わないこと —
// origin.x + 0.0f でも -0.0 が +0.0 に化ける (ビット保存の契約)
struct ParticleShapeSample {
    float dirX, dirY, dirZ; // 正規化済みの放出方向 (基底適用前)
    float offX, offY, offZ; // origin からの局所オフセット (基底適用前)
    bool hasOffset;         // false = オフセットなし (呼び出し側は加算しない)
};

inline ParticleShapeSample SampleParticleShape(const ParticleEmitterComponent& d, Pcg32& rng)
{
    ParticleShapeSample s = { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, false };
    switch (d.shape) {
    case 1: { // sphere (外向き。emitFrom=1 で半径方向を体積分布に)
        const float z = 1.0f - 2.0f * rng.NextFloat01();
        const float phi = rng.NextFloat01() * 2.0f * kParticleEmitPi;
        const float sn = sqrtf(std::max(0.0f, 1.0f - z * z));
        s.dirX = sn * cosf(phi);
        s.dirY = z;
        s.dirZ = sn * sinf(phi);
        float r = d.shapeRadius;
        if (d.emitFrom == 1) {
            // 体積一様は r ∝ cbrt(u) (半径 r 内の体積が r^3 に比例するため)。u を 1 回追加消費
            r = d.shapeRadius * cbrtf(rng.NextFloat01());
        }
        s.offX = s.dirX * r;
        s.offY = s.dirY * r;
        s.offZ = s.dirZ * r;
        s.hasOffset = true;
        break;
    }
    case 2: { // cone (+Y 中心。emitFrom=1/2 で底円盤から放出)
        const float cosMax = cosf(d.coneAngleDeg * kParticleEmitPi / 180.0f);
        const float cosT = 1.0f - rng.NextFloat01() * (1.0f - cosMax);
        const float sinT = sqrtf(std::max(0.0f, 1.0f - cosT * cosT));
        const float phi = rng.NextFloat01() * 2.0f * kParticleEmitPi;
        s.dirX = sinT * cosf(phi);
        s.dirY = cosT;
        s.dirZ = sinT * sinf(phi);
        if (d.emitFrom == 1 || d.emitFrom == 2) {
            // 半径 shapeRadius の円盤 (y=0 面)。面一様は r ∝ sqrt(u)。u, v を追加消費。
            // 円盤に「体積/表面」の区別は無いので 1/2 は同義 (どちらも同じ消費列)
            const float rd = d.shapeRadius * sqrtf(rng.NextFloat01());
            const float theta = rng.NextFloat01() * 2.0f * kParticleEmitPi;
            s.offX = rd * cosf(theta);
            s.offZ = rd * sinf(theta);
            s.hasOffset = true;
        }
        break;
    }
    case 3: { // box (上向き固定。emitFrom=2 で表面 6 面から放出)
        if (d.emitFrom == 2) {
            // 表面: 面選択 1 回 + 面内 2 軸 2 回。固定軸は ±extent に張り付ける。
            // NextFloat01 は [0,1) なので fu*6 < 6 だが、丸めの保険で 5 に clamp
            const float fu = rng.NextFloat01();
            const int face = std::min(static_cast<int>(fu * 6.0f), 5);
            const float a = rng.NextFloat01() * 2.0f - 1.0f;
            const float b = rng.NextFloat01() * 2.0f - 1.0f;
            const int axis = face / 2;                      // 0=x 1=y 2=z
            const float sign = (face & 1) ? -1.0f : 1.0f;
            if (axis == 0) {
                s.offX = sign * d.boxExtents.x;
                s.offY = a * d.boxExtents.y;
                s.offZ = b * d.boxExtents.z;
            } else if (axis == 1) {
                s.offX = a * d.boxExtents.x;
                s.offY = sign * d.boxExtents.y;
                s.offZ = b * d.boxExtents.z;
            } else {
                s.offX = a * d.boxExtents.x;
                s.offY = b * d.boxExtents.y;
                s.offZ = sign * d.boxExtents.z;
            }
        } else {
            // 体積 (0/1 とも従来と同一挙動・同一消費): 各軸独立の一様
            s.offX = (rng.NextFloat01() * 2.0f - 1.0f) * d.boxExtents.x;
            s.offY = (rng.NextFloat01() * 2.0f - 1.0f) * d.boxExtents.y;
            s.offZ = (rng.NextFloat01() * 2.0f - 1.0f) * d.boxExtents.z;
        }
        s.hasOffset = true;
        break;
    }
    default: // point (emitFrom 無視。オフセットも乱数消費も無し)
        break;
    }
    return s;
}

// ==== M61d: 乱流ノイズ (カールノイズ純関数 + HLSL ミラー) はこの下へ ====

} // namespace mye
