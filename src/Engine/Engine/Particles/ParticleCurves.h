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

// ---- M61f: GPU 容量/バースト判定 ----
// GPU エミッタの容量追従とバースト上限の「判定」だけを純関数に分離する (StepGpuIdleSkip と
// 同じ理由 — D3D 実機は selftest で回せないので、判定ロジックをここで検証する)。
// GPU プールは表示用ベストエフォート (ハッシュ非対象) — sim 状態には決して使わないこと。

// desc.maxParticles から GPU プール容量を決める。下限 1024 は極小 APPEND/COUNTER バッファを
// 作らない保険、上限 1M は VRAM 暴走ガード (48B/粒 ≒ 48MB)。SyncEmitters (新規作成) と
// Update (容量追従) の両方がこの 1 本を通る = クランプ基準の一本化。
inline uint32_t GpuEmitterCapacityFor(int32_t maxParticles)
{
    return static_cast<uint32_t>(std::clamp(maxParticles, 1024, 1000000));
}

// 容量追従の要否。クランプ後の希望容量と現容量の単純比較 — 増減どちらでも作り直す
// (縮小に追従しないと「一度 1M にすると戻せない」形で VRAM を占有し続ける)。
// maxParticles がクランプ域の外で動いてもクランプ後が同値なら false = 無駄な再作成をしない
inline bool GpuCapacityNeedsRecreate(uint32_t currentCapacity, uint32_t desiredCapacity)
{
    return currentCapacity != desiredCapacity;
}

// 1 tick の GPU 放出数クランプ。旧実装は capacity/4 の静黙クランプ (1tick 暴発ガード) で、
// 「maxParticles まで積んだはずのバーストが 25% で切られる」罠だったので容量全量まで緩和。
// dead list 枯渇分は particle_emit.cs.hlsl の deadCount ガードが既に安全に捨てる。
// EmitData ステージングが最大 capacity*32B (1M で 32MB) になりうるが、burst した tick
// だけの一過性 (動的バッファは以降の小さい tick でもそのまま再利用されるだけ) と判断
inline int ClampGpuEmitCount(int emitCount, uint32_t capacity)
{
    return std::clamp(emitCount, 0, static_cast<int>(capacity));
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

// ---- M61c: サブフレーム放出の誕生フラクション ----
// この tick に生まれる total 個のうち n 番目 (0 始まり) の誕生時刻 (tick 区間 [0,1] 内)。
// RNG を消費しない決定論的な固定式。+0.5 のオフセットで区間の両端 (0 と 1 ちょうど) を
// 避ける — f=0 は前 tick の放出末尾と重なり、f=1 は「この tick では 1 度も動かない」粒になる
inline float ParticleSubframeFraction(int n, int total)
{
    return (static_cast<float>(n) + 0.5f) / static_cast<float>(total);
}

// ==== M61d: 乱流ノイズ (カールノイズ純関数 + HLSL ミラー) はこの下へ ====

// ---- M61d: カールノイズ乱流場 (turbulenceMode=1) ----
// particle_sim.cs.hlsl の同名関数群とコメント同期のミラー (機械照合なし — 変更は必ず両方同時に)。
// ★決定論の要: pool.rng は 1 回も消費しない — 位置と時間だけの純関数。時間は呼び出し側が
//   sim 状態 (pool.ageTicks * dt) から与える (実時間は絶対に混ぜない)。
// 実装は「整数格子ハッシュ + quintic 補間のバリューノイズ」を 3 相 (オフセット違い) で
// ポテンシャル場 F とし、curl = ∇×F を中心差分で近似する。div(∇×F)=0 の性質は、差分演算子が
// 厳密に可換 (同一 4 点の同一係数和) なため「同じ刻みで測った数値発散」でも丸め誤差レベルで
// 成り立つ — selftest はこれを検証する。

// 中心差分の刻み。格子間隔 1.0 に対して十分小さく、かつ float の桁落ちが出ない値。
// selftest の数値発散検査はこの同じ刻みを使う (刻みが違うと相殺が崩れて O(h^2) 残差が出る)
inline constexpr float kCurlNoiseEps = 0.25f;

// 整数格子ハッシュ (32bit finalizer)。int→uint は 2 の補数 wrap で C++/HLSL 同一。
// 乗算オーバーフローも両言語とも wrap = 同値
inline uint32_t CurlNoiseHash(int32_t ix, int32_t iy, int32_t iz)
{
    uint32_t h = static_cast<uint32_t>(ix) * 0x8da6b343u
               + static_cast<uint32_t>(iy) * 0xd8163841u
               + static_cast<uint32_t>(iz) * 0xcb1ab31fu;
    h ^= h >> 13;
    h *= 0x7feb352du;
    h ^= h >> 16;
    return h;
}

// 格子点の値 [-1, 1)。上位 24bit → [0,1) の量子化は Pcg32::NextFloat01 と同じ手筋
// (float で正確に表現できる範囲に落としてから写像する)
inline float CurlNoiseLattice(int32_t ix, int32_t iy, int32_t iz)
{
    return static_cast<float>(CurlNoiseHash(ix, iy, iz) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// 3D バリューノイズ (トリリニア + quintic フェード)。値域はほぼ [-1, 1]。
// quintic (C2 連続) なのは中心差分で curl を取るため — smoothstep (C1) だと格子境界で
// 2 階微分が跳ね、カール場が縞状のアーティファクトを持つ。
// 座標が float→int32 の変換域を超えるほど飛んだ場合 (|x| > 2^31) は cvttss2si の飽和値に
// 落ちるが、x64 では全ビルド同一命令 = 決定論は保たれる (粒子は寿命で死ぬので実害なし)
inline float CurlValueNoise(float x, float y, float z)
{
    const float fx = floorf(x);
    const float fy = floorf(y);
    const float fz = floorf(z);
    const int32_t ix = static_cast<int32_t>(fx);
    const int32_t iy = static_cast<int32_t>(fy);
    const int32_t iz = static_cast<int32_t>(fz);
    float tx = x - fx;
    float ty = y - fy;
    float tz = z - fz;
    tx = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
    ty = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
    tz = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);
    const float v000 = CurlNoiseLattice(ix, iy, iz);
    const float v100 = CurlNoiseLattice(ix + 1, iy, iz);
    const float v010 = CurlNoiseLattice(ix, iy + 1, iz);
    const float v110 = CurlNoiseLattice(ix + 1, iy + 1, iz);
    const float v001 = CurlNoiseLattice(ix, iy, iz + 1);
    const float v101 = CurlNoiseLattice(ix + 1, iy, iz + 1);
    const float v011 = CurlNoiseLattice(ix, iy + 1, iz + 1);
    const float v111 = CurlNoiseLattice(ix + 1, iy + 1, iz + 1);
    const float x00 = v000 + (v100 - v000) * tx;
    const float x10 = v010 + (v110 - v010) * tx;
    const float x01 = v001 + (v101 - v001) * tx;
    const float x11 = v011 + (v111 - v011) * tx;
    const float y0 = x00 + (x10 - x00) * ty;
    const float y1 = x01 + (x11 - x01) * ty;
    return y0 + (y1 - y0) * tz;
}

// ポテンシャル場の 3 相。同一ノイズのオフセット違い (格子周期と無縁な端数オフセットで
// 相関を切る)。相ごとに別ハッシュ定数を持つより、ミラー時の写し間違いが起きにくい
inline float CurlPotX(float x, float y, float z) { return CurlValueNoise(x, y, z); }
inline float CurlPotY(float x, float y, float z)
{
    return CurlValueNoise(x + 31.416f, y + 17.923f, z + 43.651f);
}
inline float CurlPotZ(float x, float y, float z)
{
    return CurlValueNoise(x - 47.317f, y + 61.139f, z - 21.744f);
}

// カールノイズ場の評価。p は「粒子位置 × noiseFrequency」、t は「ageTicks*dt × noiseSpeed」を
// 呼び出し側が渡す。返り値は加速度の向き場 (呼び出し側が turbulence を掛ける)。
// 時間は斜めドリフト (場全体の平行移動) として注入する — 4D ノイズより安価で、軸沿いだと
// 格子の縞が流れて見えるため非軸整列の方向を使う
inline DirectX::XMFLOAT3 EvalCurlNoise(const DirectX::XMFLOAT3& p, float t)
{
    const float qx = p.x + t * 1.0f;
    const float qy = p.y + t * 0.35f;
    const float qz = p.z + t * 0.71f;
    const float e = kCurlNoiseEps;
    const float inv2e = 1.0f / (2.0f * e);
    // curl = ∇×F に必要な偏微分 6 個 (各 2 点評価 = ノイズ 12 回)
    const float dFzDy = (CurlPotZ(qx, qy + e, qz) - CurlPotZ(qx, qy - e, qz)) * inv2e;
    const float dFyDz = (CurlPotY(qx, qy, qz + e) - CurlPotY(qx, qy, qz - e)) * inv2e;
    const float dFxDz = (CurlPotX(qx, qy, qz + e) - CurlPotX(qx, qy, qz - e)) * inv2e;
    const float dFzDx = (CurlPotZ(qx + e, qy, qz) - CurlPotZ(qx - e, qy, qz)) * inv2e;
    const float dFyDx = (CurlPotY(qx + e, qy, qz) - CurlPotY(qx - e, qy, qz)) * inv2e;
    const float dFxDy = (CurlPotX(qx, qy + e, qz) - CurlPotX(qx, qy - e, qz)) * inv2e;
    return { dFzDy - dFyDz, dFxDz - dFzDx, dFyDx - dFxDy };
}

} // namespace mye
