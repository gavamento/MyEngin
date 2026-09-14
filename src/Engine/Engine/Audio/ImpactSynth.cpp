//====================================================================================
//                          ImpactSynth.cpp
//  MyEngine/ 秋田蓮音                                                      09/12/2026
//                                          Transient + Resonator Bank + Shard Events
//====================================================================================
#include "Engine/Engine/Audio/ImpactSynth.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Random.h"

namespace mye {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// 整数倍を避けたモード比 (計画 §8)。楽器的 (倍音列) になりすぎるのを防ぐ
constexpr float kModeRatios[6] = { 1.00f, 1.37f, 1.91f, 2.63f, 3.42f, 4.71f };
constexpr int kModeCount = 6;

float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// 材質プリセット。順序は AcousticMaterial の列挙値
// (baseFrequency / resonanceDecay / resonanceAmount / noiseAmount / brightness / shardAmount /
//  transientDecay / bodyAmount)
// ★計画 §9 の初期値より一段暗い (基本周波数を 2/3 前後・brightness を少し絞る・bodyAmount で
//   低域の胴鳴り)。初期値のままだと全体的に高音に聞こえる
constexpr ImpactMaterialPreset kPresets[kAcousticMaterialCount] = {
    { 260.0f, 24.0f, 0.80f, 0.60f, 0.25f, 0.10f, 120.0f, 0.60f },  // Wood: 低中域の共鳴、早く減衰
    { 650.0f, 6.0f, 1.00f, 0.15f, 0.75f, 0.20f, 220.0f, 0.30f },   // Metal: 明るく長く鳴る
    { 170.0f, 40.0f, 0.35f, 0.90f, 0.35f, 0.00f, 180.0f, 0.55f },  // Concrete: 乾いたノイズ、共鳴弱
    // Glass: 甲高い、破片あり。★resonanceDecay 14 だと 0.3 秒鳴り続けてグラスを弾いた「ピーン」に
    //   なる (純音 3 本だけで hi>4k のノイズが 0 だった)。
    //   瓶や板ガラスの衝突は「カツン」= 短い共鳴 + はっきりした transient なので 45 / noise 0.55
    { 1200.0f, 45.0f, 1.00f, 0.55f, 0.90f, 1.00f, 200.0f, 0.15f },
    { 220.0f, 45.0f, 0.60f, 0.50f, 0.20f, 0.00f, 150.0f, 0.50f },  // Plastic: 鈍く短い
    { 130.0f, 60.0f, 0.10f, 1.00f, 0.10f, 0.00f, 55.0f, 0.70f },   // Grass: ノイズ主体、共鳴ほぼ無し
    { 420.0f, 50.0f, 0.30f, 0.90f, 0.50f, 0.00f, 320.0f, 0.40f },  // Gravel: 短いクリックの集まり
};

// brightness (0..1.3) → transient LPF のカットオフ [Hz]。対数補間: 0 = 400 Hz、1 = 10 kHz。
// ★1 極 (6 dB/oct) で alpha 0.9 だとほぼ白色ノイズになり、何を踏んでも「シャッ」に
//   なる。2 極 (12 dB/oct) にして肩を下げる
float TransientCutoffHz(float brightness, uint32_t rate)
{
    const float fc = 400.0f * std::pow(25.0f, Clamp(brightness, 0.0f, 1.3f));
    return std::min(fc, static_cast<float>(rate) * 0.45f);
}

constexpr const char* kKindNames[3] = { "Footstep", "Impact", "GlassBreak" };
constexpr const char* kMaterialNames[kAcousticMaterialCount] = {
    "Wood", "Metal", "Concrete", "Glass", "Plastic", "Grass", "Gravel",
};

// 相手材質による補正 (計画 §15 の最小実装)
struct PairCorrection {
    float transientMul = 1.0f;
    float brightnessAdd = 0.0f;
    float highFreqMul = 1.0f;
};
PairCorrection CorrectionFor(AcousticMaterial partner)
{
    PairCorrection c;
    switch (partner) {
    case AcousticMaterial::Concrete: c.transientMul = 1.3f; break;
    case AcousticMaterial::Metal: c.brightnessAdd = 0.2f; break;
    case AcousticMaterial::Wood: c.highFreqMul = 0.7f; break;
    case AcousticMaterial::Grass: c.transientMul = 0.6f; break;
    default: break;
    }
    return c;
}

// 減衰正弦を 1 本加算する。起点に 1ms の立ち上がりを入れてクリックを避ける
void AddDampedSine(std::vector<float>& mix, uint32_t rate, float startSec, float freq, float amp,
                   float decay, float phase)
{
    if (amp <= 0.0f || freq <= 0.0f || freq >= rate * 0.45f) {
        return; // ナイキスト付近は折り返すので捨てる
    }
    const size_t frames = mix.size();
    const size_t start = static_cast<size_t>(std::max(0.0f, startSec) * rate);
    if (start >= frames) {
        return;
    }
    const double inv = 1.0 / static_cast<double>(rate);
    const double w = kTwoPi * static_cast<double>(freq);
    const double attack = 0.001; // 1ms
    for (size_t i = start; i < frames; ++i) {
        const double t = static_cast<double>(i - start) * inv;
        const double env = std::exp(-static_cast<double>(decay) * t);
        if (env < 1e-4) {
            break; // 以降は聞こえない
        }
        const double att = t < attack ? (t / attack) : 1.0;
        mix[i] += static_cast<float>(std::sin(w * t + phase) * env * att * amp);
    }
}

// ノイズバースト (transient) を 1 本加算する。2 極 LPF (12 dB/oct) のカットオフ = brightness
void AddNoiseBurst(std::vector<float>& mix, uint32_t rate, Pcg32& rng, float startSec, float amp,
                   float decay, float brightness)
{
    if (amp <= 0.0f) {
        return;
    }
    const size_t frames = mix.size();
    const size_t start = static_cast<size_t>(std::max(0.0f, startSec) * rate);
    if (start >= frames) {
        return;
    }
    const double inv = 1.0 / static_cast<double>(rate);
    // 1 極の係数 alpha = 1 - exp(-2π fc / fs) を 2 段重ねる
    const float fc = TransientCutoffHz(brightness, rate);
    const float alpha = 1.0f - static_cast<float>(std::exp(-kTwoPi * fc / static_cast<double>(rate)));
    float lp1 = 0.0f;
    float lp2 = 0.0f;
    for (size_t i = start; i < frames; ++i) {
        const double t = static_cast<double>(i - start) * inv;
        const double env = std::exp(-static_cast<double>(decay) * t);
        if (env < 1e-4) {
            break;
        }
        const float n = rng.Range(-1.0f, 1.0f);
        lp1 += alpha * (n - lp1);
        lp2 += alpha * (lp1 - lp2);
        mix[i] += static_cast<float>(lp2 * env * amp);
    }
}

} // namespace

const ImpactMaterialPreset& MaterialPreset(AcousticMaterial m)
{
    const int32_t i = static_cast<int32_t>(m);
    return kPresets[(i >= 0 && i < kAcousticMaterialCount) ? i : 0];
}

const char* ImpactSoundKindName(ImpactSoundKind k)
{
    const int32_t i = static_cast<int32_t>(k);
    return kKindNames[(i >= 0 && i < 3) ? i : 1];
}

const char* AcousticMaterialName(AcousticMaterial m)
{
    const int32_t i = static_cast<int32_t>(m);
    return kMaterialNames[(i >= 0 && i < kAcousticMaterialCount) ? i : 0];
}

bool ParseImpactSoundKind(std::string_view s, ImpactSoundKind& out)
{
    for (int32_t i = 0; i < 3; ++i) {
        if (s == kKindNames[i]) {
            out = static_cast<ImpactSoundKind>(i);
            return true;
        }
    }
    return false;
}

bool ParseAcousticMaterial(std::string_view s, AcousticMaterial& out)
{
    for (int32_t i = 0; i < kAcousticMaterialCount; ++i) {
        if (s == kMaterialNames[i]) {
            out = static_cast<AcousticMaterial>(i);
            return true;
        }
    }
    return false;
}

void ImpactSynthRender(const ImpactSynthParams& params, AudioClip& out)
{
    out = AudioClip{};
    const uint32_t rate = params.sampleRate != 0 ? params.sampleRate : 44100u;
    const double total = params.durationSec > 0.0f ? static_cast<double>(params.durationSec) : 0.0;
    const size_t frames = static_cast<size_t>(total * rate + 0.5);
    if (frames == 0) {
        return;
    }

    // 主体材質: 足音は地面 (materialB)、それ以外は materialA。補正は相手側から (計画 §12 / §15)
    const bool footstep = params.kind == ImpactSoundKind::Footstep;
    const AcousticMaterial primary = footstep ? params.materialB : params.materialA;
    const ImpactMaterialPreset& pre = MaterialPreset(primary);
    const PairCorrection corr = footstep ? PairCorrection{} : CorrectionFor(params.materialB);

    const float size = Clamp(params.size, 0.25f, 4.0f);
    const float strength = Clamp(params.strength, 0.05f, 4.0f);
    const float amp = std::sqrt(strength); // 計画 §11 の非線形
    // 明るさ = プリセット × 耳用の倍率 + 相手材質の補正 + 強打ほど明るい。上限 1.3 は
    // 「倍率 2 を掛けた Glass がまだ動く」ため (TransientCutoffHz / hf が受ける範囲)
    const float brightScale = Clamp(params.brightness, 0.25f, 2.0f);
    const float brightness = Clamp(pre.brightness * brightScale + corr.brightnessAdd
                                       + 0.15f * (strength - 1.0f),
                                   0.0f, 1.3f);

    // seed の下位に kind を混ぜない — 「同じ seed で kind だけ違う」も当然別の音になる
    // (描く成分が違う) ので、列は params.seed だけで決める = 再現手順が 1 行で書ける
    Pcg32 rng;
    rng.Seed(params.seed);

    std::vector<float> mix(frames, 0.0f);

    // ---- 1. Transient (計画 §7) ----
    {
        const float tAmp = pre.noiseAmount * (0.6f + 0.4f * strength) * corr.transientMul;
        switch (params.kind) {
        case ImpactSoundKind::Footstep:
            if (primary == AcousticMaterial::Gravel) {
                // 砂利: 40ms の中に 3-6 個の短いクリック (計画 §12)
                const int clicks = 3 + rng.RangeInt(0, 4);
                for (int c = 0; c < clicks; ++c) {
                    const float t0 = rng.Range(0.0f, 0.04f);
                    const float a = tAmp * rng.Range(0.5f, 1.0f);
                    AddNoiseBurst(mix, rate, rng, t0, a, pre.transientDecay, brightness);
                }
            } else {
                // 「コッ」5-30ms。材質の transientDecay をそのまま使う
                AddNoiseBurst(mix, rate, rng, 0.0f, tAmp, pre.transientDecay * 0.8f, brightness);
                if (primary == AcousticMaterial::Grass) {
                    // 草 / 柔らかい面: ノイズが主成分なので、暗い尾を長めに足す
                    AddNoiseBurst(mix, rate, rng, 0.005f, tAmp * 0.5f, pre.transientDecay * 0.35f,
                                  brightness * 0.5f);
                }
            }
            break;
        case ImpactSoundKind::Impact:
            // 「カッ」3-20ms
            AddNoiseBurst(mix, rate, rng, 0.0f, tAmp, pre.transientDecay, brightness);
            break;
        case ImpactSoundKind::GlassBreak:
        default:
            // Crack: 計画 §14 の exp(-150 t)。明るいノイズで「パキッ」
            AddNoiseBurst(mix, rate, rng, 0.0f, tAmp * 1.2f, 150.0f, Clamp(brightness + 0.2f, 0.0f, 1.3f));
            break;
        }
    }

    // ---- 1b. Body: 低域の胴鳴り「ドン」 ----
    // 足や石の質量が床を押す成分。70-110 Hz を size で緩く下げ (√size)、約 100ms で消える。
    // 純正弦だと「ブー」になるので 2.1 倍の成分を少し重ねる。GlassBreak は瓶が床に当たる分だけ
    {
        float bodyAmt = pre.bodyAmount * (footstep ? 1.0f : 0.8f);
        if (params.kind == ImpactSoundKind::GlassBreak) {
            bodyAmt *= 0.5f;
        }
        if (bodyAmt > 0.0f) {
            const float fb = Clamp(rng.Range(70.0f, 110.0f) / std::sqrt(size), 40.0f, 160.0f);
            const float decay = footstep ? 32.0f : 24.0f;
            const float ph1 = rng.Range(0.0f, static_cast<float>(kTwoPi));
            const float ph2 = rng.Range(0.0f, static_cast<float>(kTwoPi));
            AddDampedSine(mix, rate, 0.0f, fb, bodyAmt * amp * 0.9f, decay, ph1);
            AddDampedSine(mix, rate, 0.0f, fb * 2.1f, bodyAmt * amp * 0.25f, decay * 1.6f, ph2);
        }
    }

    // ---- 2. Resonator Bank (計画 §8) ----
    {
        int modes = kModeCount;
        float resAmp = pre.resonanceAmount * amp * 0.6f;
        float decayMul = 1.0f;
        if (footstep) {
            modes = (primary == AcousticMaterial::Grass) ? 1 : 3; // 2-4 モード (計画 §12)
        } else if (params.kind == ImpactSoundKind::GlassBreak) {
            decayMul = 2.0f; // 瓶の共鳴は破壊直後に短く鳴らす (計画 §14。Glass 45 × 2 = 約 20ms)
            resAmp *= 0.7f;
        }
        // 周波数は 1/size。seed で ±3% 揺らす (計画 §10 / §13)
        const float f0 = (pre.baseFrequency / size) * (1.0f + rng.Range(-0.03f, 0.03f));
        // 高次モードの振幅比。brightness が高いほど高域が残る。相手が木なら高域を削る。
        // 上限 0.85 = 最も明るい材質でも第 6 モード (4.71 f0) は第 1 モードの 44% まで
        const float hf = Clamp((0.35f + 0.45f * brightness) * corr.highFreqMul, 0.1f, 0.85f);
        for (int m = 0; m < modes; ++m) {
            const float f = f0 * kModeRatios[m] * (1.0f + rng.Range(-0.01f, 0.01f));
            const float a = resAmp * std::pow(hf, static_cast<float>(m));
            const float d = pre.resonanceDecay * decayMul * (1.0f + 0.18f * static_cast<float>(m));
            const float ph = rng.Range(0.0f, static_cast<float>(kTwoPi));
            AddDampedSine(mix, rate, 0.0f, f, a, d, ph);
        }
    }

    // ---- 3. Shard Events (計画 §14。GlassBreak だけ) ----
    // ★破片は「短く・多く・crack の直後に密」。減衰が遅く (20-100/s) 開始が 0-250ms に一様だと
    //   長い純音が 150ms 以降に積み上がり、crack より**後のほうが大きい**「ピロロロン」になる。
    //   だから減衰は 60-250/s、開始は r² で crack 側へ寄せ、飛び散る「シャラ」は
    //   別枝の明るいノイズの尾で描く
    if (params.kind == ImpactSoundKind::GlassBreak) {
        const float shardScale = MaterialPreset(params.materialA).shardAmount;
        const int count =
            std::clamp(static_cast<int>(36.0f * shardScale * Clamp(strength, 0.5f, 2.0f) + 0.5f), 0, 72);
        for (int s = 0; s < count; ++s) {
            const float r = rng.Range(0.0f, 1.0f);
            const float start = 0.004f + 0.22f * r * r;
            // 破片の帯域も耳用の倍率で動く (ナイキスト超えは AddDampedSine が捨てる)
            const float freq = rng.Range(2000.0f, 10000.0f) * brightScale;
            const float a = rng.Range(0.03f, 0.12f) * amp;
            const float d = rng.Range(60.0f, 250.0f);
            const float ph = rng.Range(0.0f, static_cast<float>(kTwoPi));
            AddDampedSine(mix, rate, start, freq, a, d, ph);
        }
        // 飛散のノイズ (crack の 1/4 の大きさで ~80ms)。破片の純音を埋めて「ガシャ」に寄せる
        const float debris = pre.noiseAmount * 0.25f * amp * shardScale;
        AddNoiseBurst(mix, rate, rng, 0.01f, debris, 12.0f, Clamp(brightness, 0.0f, 1.3f));
    }

    // ---- 4. Peak / Gain 調整 (計画 §16) ----
    // ピークを約 -1 dBFS に揃える。材質ごとの音量差は SoundAsset.volume と波の振幅で付ける
    // (ここで残すと「静かな材質ほど量子化ノイズが多い」になる)
    float peak = 0.0f;
    for (float v : mix) {
        peak = std::max(peak, std::fabs(v));
    }
    const float gain = (peak > 1e-6f) ? (0.89f / peak) : 0.0f;

    // ---- 5. 終端フェード (クリック防止)。長さの 8% か 5ms の長い方 ----
    const size_t fade = std::max<size_t>(static_cast<size_t>(rate * 0.005), frames / 12);

    // ---- 6-7. int16 化 (SynthCore と同じ丸め: 四捨五入・ゼロから遠い側) ----
    out.sampleRate = rate;
    out.channels = 1;
    out.samples.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        float v = mix[i] * gain;
        if (fade > 0 && i + fade >= frames) {
            v *= static_cast<float>(frames - i) / static_cast<float>(fade);
        }
        v = Clamp(v, -1.0f, 1.0f); // soft clip は不要 (正規化済み)。保険のクランプだけ
        const double s = static_cast<double>(v) * 32767.0;
        out.samples[i] = static_cast<int16_t>(s >= 0.0 ? s + 0.5 : s - 0.5);
    }
}

} // namespace mye
