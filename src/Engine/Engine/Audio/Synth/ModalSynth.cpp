//====================================================================================
//                          ModalSynth.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          BuildModes (材質スケール則) と 2 次再帰共振器の合成
//====================================================================================
#include "Engine/Engine/Audio/Synth/ModalSynth.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Util/Random.h"

namespace mye {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Mel 32 帯域の中心周波数は等間隔の格子点になるため、そのまま鳴らすとボコーダー的な
// 人工和音になりやすい。メッシュ固有シードと帯域番号から決定論的な ±3.5% のオフセットを
// 生成し、隣接帯域の順序を保ったまま格子感を崩す
float BandDetuneFactor(uint64_t seed, int band)
{
    uint64_t z = seed ^ (static_cast<uint64_t>(band + 1) * 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    // 上位 24 bit を [-1, 1] へ正規化
    const float u = static_cast<float>(z >> 40) * (2.0f / 16777215.0f) - 1.0f;
    constexpr float kMaxDetuneRatio = 0.035f;
    return 1.0f + kMaxDetuneRatio * u;
}

// 接触瞬間の広帯域トランジェント (2 極 LPF 12 dB/oct + 指数減衰ノイズ)。
// 純粋な減衰サイン波だけだと電子音・ベル的になるため、短い打撃の芯を冒頭へ重ねる
void AddTransientBurst(std::vector<double>& mix, uint32_t rate, const ModalModeSet& modes)
{
    if (modes.transientAmp <= 0.0f || modes.transientDecay <= 0.0f || mix.empty()) {
        return;
    }
    const double fc = std::clamp(static_cast<double>(modes.transientCutoffHz), 200.0,
                                 static_cast<double>(rate) * 0.45);
    const double alpha = 1.0 - std::exp(-kTwoPi * fc / static_cast<double>(rate));
    const double invRate = 1.0 / static_cast<double>(rate);
    const double decay = static_cast<double>(modes.transientDecay);
    const double amp = static_cast<double>(modes.transientAmp);
    constexpr double kAttackSec = 0.0002; // 0.2 ms: クリックを抑えつつ硬い立ち上がりを保つ

    Pcg32 rng;
    rng.Seed(modes.transientSeed != 0 ? modes.transientSeed : 0x4D4F44414C54524EULL);
    double lp1 = 0.0;
    double lp2 = 0.0;
    for (size_t n = 0; n < mix.size(); ++n) {
        const double t = static_cast<double>(n) * invRate;
        const double env = std::exp(-decay * t);
        if (env < 1.0e-4) {
            break;
        }
        const double att = t < kAttackSec ? (t / kAttackSec) : 1.0;
        const double noise = static_cast<double>(rng.Range(-1.0f, 1.0f));
        lp1 += alpha * (noise - lp1);
        lp2 += alpha * (lp1 - lp2);
        mix[n] += lp2 * env * att * amp;
    }
}

// [0, attackSamples) の間だけ 0→1 の線形ランプ。以降は 1.0 (spec §4.1「1 ms 線形アタック」)。
// 2 次再帰共振器はサンプルごとの sin() 評価を避けるための漸化式なので、アタックは
// 出力側 (このゲイン) で別途かける — 漸化式そのものに窓をかけると位相が壊れる
double AttackGain(size_t n, size_t attackSamples)
{
    return n < attackSamples ? static_cast<double>(n) / static_cast<double>(attackSamples) : 1.0;
}

// tanh ソフトクリップ。|x| <= 0.8 は無加工 (spec §4.1「|x| > 0.8 のみ」)。
// 0.8 を境に連続 (tanh(0) = 0) で、|x| → ∞ で 1.0 に漸近する
float SoftClip(float x)
{
    constexpr float kThreshold = 0.8f;
    const float mag = std::fabs(x);
    if (mag <= kThreshold) {
        return x;
    }
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float excess = mag - kThreshold;
    return sign * (kThreshold + (1.0f - kThreshold) * std::tanh(excess / (1.0f - kThreshold)));
}

} // namespace

void BuildModes(const ModalCellFeature& feature, const float k[3], const DmNetHeader& hdr,
                const ModalPostParams& post, ModalModeSet& out)
{
    out = ModalModeSet{};

    // 手順 3: mask 閾値。t <= 0 はヘッダ既定 (spec §4.1)。0/1 ちょうどは ln の特異点なので
    // わずかに内側へ寄せる (どちらへ丸めても聴感に影響しない範囲)
    const float t = post.maskThreshold > 0.0f ? post.maskThreshold : hdr.maskThreshold;
    const float tc = std::clamp(t, 1.0e-6f, 1.0f - 1.0e-6f);
    const float logitThreshold = std::log(tc / (1.0f - tc));

    // 手順 6 のスケール係数。E=0 は「参照材質のまま」= σ1=1 (spec §4.1 手順 6)
    const float sigma1 = (post.young > 0.0f && hdr.refYoung > 0.0f) ? (post.young / hdr.refYoung) : 1.0f;
    const float sigma2 = (hdr.refDensity > 0.0f) ? (post.density / hdr.refDensity) : 1.0f;
    const float sigma3 = (hdr.refSizeL > 0.0f) ? (post.sizeL / hdr.refSizeL) : 1.0f;
    const float lambdaScale = sigma1 / (sigma2 * sigma3 * sigma3);
    const float ampSizeScale = std::pow(sigma2, -0.5f) * std::pow(sigma3, -1.5f);

    const float fsNyquistLimit = 0.45f * static_cast<float>(kModalSampleRate);

    for (int i = 0; i < kModalBands; ++i) {
        // 手順 1 (fp16→float は呼び出し側で完了済み) + 手順 4: 3 力軸を mask で選び |k| で加重合成
        float a = 0.0f;
        for (int j = 0; j < 3; ++j) {
            const float maskLogit = feature.v[MaskCh(j, i)];
            if (maskLogit <= logitThreshold) {
                continue; // 手順 3: マスクで落ちる (この力軸・帯域の寄与は 0)
            }
            // 手順 2: log-amp 逆正規化
            const float v = feature.v[AmpCh(j, i)];
            const float lnA = hdr.logAmpMin + v * (hdr.logAmpMax - hdr.logAmpMin);
            a += std::fabs(k[j]) * std::exp(lnA);
        }
        if (a <= 0.0f) {
            continue; // k=0 や全軸マスク落ちはここで弾かれる
        }
        a *= hdr.ampScale * post.gain; // 手順 4 末尾

        // 手順 5: 帯域中心を「参照材質の非減衰角周波数」とみなし、参照材質の λ を作る。
        // detuneSeed != 0 のときは帯域内の決定論的オフセットで Mel 格子感を崩す
        // (手順 6 の材質・サイズスケール則より前に掛けるため、E/ρ/L の比率は厳密に保たれる)
        float fc = hdr.bandCenterHz[i];
        if (post.detuneSeed != 0) {
            fc *= BandDetuneFactor(post.detuneSeed, i);
        }
        const float w = static_cast<float>(kTwoPi) * fc;
        const float cRef = 0.5f * (hdr.refAlpha + hdr.refBeta * w * w);
        float lambda = w * w + cRef * cRef;

        // 手順 6: 材質・サイズのスケール則 (式 12)
        lambda *= lambdaScale;
        a *= ampSizeScale;

        // 手順 7: 目標材質の減衰・周波数。過減衰 / ナイキスト超え / 無音は捨てる
        const float c = 0.5f * (post.alpha + post.beta * lambda);
        const float underRoot = lambda - c * c;
        if (!(underRoot > 0.0f)) {
            continue; // 過減衰 (虚数になる)。否定形 = NaN (縮退した入力) もここで捨てる (M93c)
        }
        const float f = std::sqrt(underRoot) / static_cast<float>(kTwoPi);
        if (f >= fsNyquistLimit) {
            continue; // ナイキスト付近は折り返すので捨てる
        }
        if (a <= kModalTailAmp) {
            continue; // 聞こえないほど小さい
        }

        out.freqHz[out.count] = f;
        out.amp[out.count] = a;
        out.decay[out.count] = c;
        ++out.count;
    }
}

float ModalClipSeconds(const ModalModeSet& modes)
{
    float maxSeconds = 0.0f;
    for (int32_t i = 0; i < modes.count; ++i) {
        const float a = modes.amp[i];
        const float c = modes.decay[i];
        if (a <= kModalTailAmp || c <= 0.0f) {
            continue; // 減衰しない (c<=0) モードは長さの決定に寄与させない (下限へ落ちるだけ)
        }
        const float seconds = std::log(a / kModalTailAmp) / c;
        maxSeconds = (std::max)(maxSeconds, seconds);
    }
    return std::clamp(maxSeconds, 0.05f, 2.0f);
}

void ModalSynthRender(const ModalModeSet& modes, AudioClip& out)
{
    out = AudioClip{};
    const uint32_t rate = static_cast<uint32_t>(kModalSampleRate);
    const double seconds = static_cast<double>(ModalClipSeconds(modes));
    const size_t frames = static_cast<size_t>(seconds * rate + 0.5);
    if (frames == 0) {
        return;
    }

    std::vector<double> mix(frames, 0.0);
    const size_t attackSamples = (std::max)(static_cast<size_t>(1), static_cast<size_t>(0.001 * rate + 0.5));

    for (int32_t i = 0; i < modes.count; ++i) {
        const double freq = static_cast<double>(modes.freqHz[i]);
        const double amp = static_cast<double>(modes.amp[i]);
        const double decay = static_cast<double>(modes.decay[i]);
        if (freq <= 0.0 || freq >= rate * 0.5 || amp <= 0.0) {
            continue; // BuildModes 側で通常は既に落ちているが、直接構築した入力にも備える
        }

        // 2 次再帰共振器: y[n] = amp * r^n * sin(theta n) (位相 0) を漸化式で進める。
        // y[0] = 0、y[1] だけ sin を直接評価し、以降は乗算 2 回 + 減算 1 回で 1 サンプル進む
        // (sin/cos をサンプルごとに呼ばない — 32 本 × 最大 2 秒でも軽い理由)
        const double theta = kTwoPi * freq / rate;
        const double r = std::exp(-decay / static_cast<double>(rate));
        const double coeff = 2.0 * r * std::cos(theta);
        const double rSq = r * r;

        double yPrev2 = 0.0;                      // y[0]
        double yPrev1 = amp * r * std::sin(theta); // y[1]
        if (frames > 1) {
            mix[1] += yPrev1 * AttackGain(1, attackSamples);
        }
        for (size_t n = 2; n < frames; ++n) {
            const double y = coeff * yPrev1 - rSq * yPrev2;
            mix[n] += y * AttackGain(n, attackSamples);
            yPrev2 = yPrev1;
            yPrev1 = y;
        }
    }

    AddTransientBurst(mix, rate, modes);

    out.sampleRate = rate;
    out.channels = 1;
    out.samples.resize(frames);
    for (size_t n = 0; n < frames; ++n) {
        const float clipped = std::clamp(SoftClip(static_cast<float>(mix[n])), -1.0f, 1.0f);
        const double s = static_cast<double>(clipped) * 32767.0;
        out.samples[n] = static_cast<int16_t>(s >= 0.0 ? s + 0.5 : s - 0.5);
    }
}

} // namespace mye
