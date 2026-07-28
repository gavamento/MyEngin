#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"

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

// M42b: ソフトパーティクル。HLSL 側 (particle_render.hlsl / particle_render_gpu.hlsl) と
// 同一式のミラー — selftest はこちらを検証する。描画専用 (sim/hash 非関与)。
// 非線形深度 [0,1] -> ビュー空間 z (透視投影)
inline float LinearizeParticleDepth(float d, float nearZ, float farZ)
{
    return nearZ * farZ / std::max(farZ - d * (farZ - nearZ), 1e-4f);
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

} // namespace mye
