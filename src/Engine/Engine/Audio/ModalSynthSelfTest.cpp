//====================================================================================
//                          ModalSynthSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          ModalSynth (BuildModes / 合成 / Mel) のヘッドレス検査
//====================================================================================
#include "Engine/Engine/Audio/ModalSynthSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/ModalSynth.h"
#include "Engine/Engine/Modal/ModalTypes.h"

namespace mye {
namespace {

int16_t PeakOf(const AudioClip& c)
{
    int32_t peak = 0;
    for (int16_t s : c.samples) {
        const int32_t a = s < 0 ? -static_cast<int32_t>(s) : s;
        peak = (std::max)(peak, a);
    }
    return static_cast<int16_t>((std::min)(peak, 32767));
}

// [t0, t1) 秒の RMS (0..1)。ImpactSynthSelfTest.cpp の同名関数と同じ定義 (この 2 ファイルは
// リンクされないので共有せず、意図的にそれぞれで完結させている)
double RmsIn(const AudioClip& c, double t0, double t1)
{
    const size_t a = static_cast<size_t>(t0 * c.sampleRate);
    const size_t b = (std::min)(c.samples.size(), static_cast<size_t>(t1 * c.sampleRate));
    if (b <= a) {
        return 0.0;
    }
    double acc = 0.0;
    for (size_t i = a; i < b; ++i) {
        const double v = c.samples[i] / 32767.0;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(b - a));
}

// [t0, t1) 秒の窓にある「正へ向かう」零交差をサブサンプル線形補間で拾い、
// (交差数-1)/(最後-最初の時刻) で周波数を推定する。単純にサンプル数で数えるより
// 1 桁以上精度が出る (1 周期あたり数十サンプルしかない高い周波数でも ±1% を満たすため)
double ZeroCrossingHz(const AudioClip& c, double t0, double t1)
{
    const size_t a = (std::max)(static_cast<size_t>(1), static_cast<size_t>(t0 * c.sampleRate));
    const size_t b = (std::min)(c.samples.size(), static_cast<size_t>(t1 * c.sampleRate));
    std::vector<double> crossTimes;
    for (size_t i = a; i < b; ++i) {
        const double s0 = c.samples[i - 1];
        const double s1 = c.samples[i];
        if (s0 < 0.0 && s1 >= 0.0) {
            const double frac = (s1 == s0) ? 0.0 : (-s0) / (s1 - s0);
            crossTimes.push_back((static_cast<double>(i - 1) + frac) / c.sampleRate);
        }
    }
    if (crossTimes.size() < 2) {
        return 0.0;
    }
    return (static_cast<double>(crossTimes.size()) - 1.0) / (crossTimes.back() - crossTimes.front());
}

} // namespace

bool RunModalSynthSelfTest()
{
    MYE_LOG_INFO("==== ModalSynth self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) 単一モード: 決定論・零交差周波数・RMS 減衰比 ----
    {
        ModalModeSet modes;
        modes.count = 1;
        modes.freqHz[0] = 440.0f;
        modes.decay[0] = 5.0f;
        modes.amp[0] = 0.5f;
        AudioClip a, b;
        ModalSynthRender(modes, a);
        ModalSynthRender(modes, b);
        check(!a.Empty() && a.channels == 1 && a.sampleRate == 44100 && a.samples == b.samples,
              "same mode set renders bit-identical mono 44100 PCM");

        const double hz = ZeroCrossingHz(a, 0.02, 0.12);
        check(hz > 0.0 && std::fabs(hz - 440.0) / 440.0 < 0.01,
              "single mode zero-crossing frequency is within 1% of 440 Hz");

        const double rmsLo = RmsIn(a, 0.1, 0.2);
        const double rmsHi = RmsIn(a, 0.6, 0.7);
        const double expected = std::exp(-2.5);
        check(rmsLo > 0.0 && std::fabs((rmsHi / rmsLo) - expected) / expected < 0.05,
              "RMS ratio between [0.6,0.7]s and [0.1,0.2]s matches exp(-2.5) within 5%");
    }

    // ---- (2) 振幅を 2 倍にするとピークも 2 倍 (線形域、ソフトクリップ手前) ----
    {
        ModalModeSet m1;
        m1.count = 1;
        m1.freqHz[0] = 300.0f;
        m1.decay[0] = 3.0f;
        m1.amp[0] = 0.2f;
        ModalModeSet m2 = m1;
        m2.amp[0] = 0.4f;
        AudioClip c1, c2;
        ModalSynthRender(m1, c1);
        ModalSynthRender(m2, c2);
        const int16_t p1 = PeakOf(c1);
        const int16_t p2 = PeakOf(c2);
        check(p1 > 0 && std::fabs(static_cast<double>(p2) / p1 - 2.0) < 0.02,
              "doubling a single mode's amplitude doubles the rendered peak");
    }

    // ---- (3) 長さ規則: 0.05s の下限 / 2.0s の上限 ----
    {
        ModalModeSet fast;
        fast.count = 1;
        fast.freqHz[0] = 1000.0f;
        fast.decay[0] = 500.0f;
        fast.amp[0] = kModalTailAmp * 1.001f; // ln(a/tail) ≈ 0 → 生の T ≈ 0
        check(std::fabs(ModalClipSeconds(fast) - 0.05f) < 1.0e-6f,
              "clip length clamps to the 0.05s floor");

        ModalModeSet slow;
        slow.count = 1;
        slow.freqHz[0] = 200.0f;
        slow.decay[0] = 1.0e-4f; // ほぼ減衰しない → 生の T が極端に大きい
        slow.amp[0] = 1.0f;
        check(std::fabs(ModalClipSeconds(slow) - 2.0f) < 1.0e-6f,
              "clip length clamps to the 2.0s ceiling");
    }

    // ---- (4) MelBandCenters: 別経路の double 計算と 1e-2 Hz で照合 (ドリフト検知) ----
    {
        float centers[kModalBands];
        MelBandCenters(centers);
        auto melOf = [](double f) { return 2595.0 * std::log10(1.0 + f / 700.0); };
        auto invMel = [](double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); };
        const double melMin = melOf(100.0);
        const double melMax = melOf(10000.0);
        bool matches = true;
        bool monotonic = true;
        float prev = 0.0f;
        for (int i = 0; i < kModalBands; ++i) {
            const double m0 = melMin + (melMax - melMin) * (static_cast<double>(i) / kModalBands);
            const double m1 = melMin + (melMax - melMin) * (static_cast<double>(i + 1) / kModalBands);
            const double expected = invMel(0.5 * (m0 + m1));
            matches = matches && std::fabs(static_cast<double>(centers[i]) - expected) < 1.0e-2;
            if (i > 0) {
                monotonic = monotonic && centers[i] > prev;
            }
            prev = centers[i];
        }
        check(matches, "MelBandCenters matches an independently written double reference within 1e-2 Hz");
        check(monotonic, "MelBandCenters is strictly increasing across the 32 bands");
    }

    // ---- (5) BuildModes: 材質・サイズのスケール則 (spec §4.1 手順 6、相対 1e-4) ----
    {
        DmNetHeader hdr;
        hdr.refYoung = 7.0e10f;
        hdr.refDensity = 2700.0f;
        hdr.refSizeL = 0.3f;
        hdr.refAlpha = 0.0f;
        hdr.refBeta = 0.0f;
        hdr.ampScale = 1.0f;
        hdr.maskThreshold = 0.5f;
        hdr.logAmpMin = -1.0f;
        hdr.logAmpMax = 1.0f;
        constexpr int kBand = 10;
        hdr.bandCenterHz[kBand] = 500.0f;

        ModalCellFeature feat{};
        feat.v[MaskCh(0, kBand)] = 5.0f; // 閾値ロジット (ln(0.5/0.5)=0) を確実に超える
        feat.v[AmpCh(0, kBand)] = 0.5f;  // lnA = -1 + 0.5*2 = 0 → amp = 1.0
        const float k[3] = { 1.0f, 0.0f, 0.0f };

        ModalPostParams base;
        base.young = hdr.refYoung;
        base.density = hdr.refDensity;
        base.sizeL = hdr.refSizeL;
        base.alpha = 0.0f;
        base.beta = 0.0f;
        base.gain = 1.0f;

        ModalModeSet outBase;
        BuildModes(feat, k, hdr, base, outBase);
        check(outBase.count == 1 && std::fabs(outBase.freqHz[0] - 500.0f) < 1.0e-2f
                  && std::fabs(outBase.amp[0] - 1.0f) < 1.0e-4f,
              "reference material at unit size reproduces the band center frequency and amp=1");

        ModalPostParams e4 = base;
        e4.young = base.young * 4.0f;
        ModalModeSet outE;
        BuildModes(feat, k, hdr, e4, outE);
        check(outE.count == 1 && std::fabs(outE.freqHz[0] / outBase.freqHz[0] - 2.0f) < 1.0e-4f
                  && std::fabs(outE.amp[0] / outBase.amp[0] - 1.0f) < 1.0e-4f,
              "E x4 doubles the frequency and leaves the amplitude unchanged");

        ModalPostParams rho4 = base;
        rho4.density = base.density * 4.0f;
        ModalModeSet outRho;
        BuildModes(feat, k, hdr, rho4, outRho);
        check(outRho.count == 1 && std::fabs(outRho.freqHz[0] / outBase.freqHz[0] - 0.5f) < 1.0e-4f
                  && std::fabs(outRho.amp[0] / outBase.amp[0] - 0.5f) < 1.0e-4f,
              "rho x4 halves both the frequency and the amplitude");

        ModalPostParams l2 = base;
        l2.sizeL = base.sizeL * 2.0f;
        ModalModeSet outL;
        BuildModes(feat, k, hdr, l2, outL);
        const float expectedAmpRatioL = std::pow(2.0f, -1.5f);
        check(outL.count == 1 && std::fabs(outL.freqHz[0] / outBase.freqHz[0] - 0.5f) < 1.0e-4f
                  && std::fabs(outL.amp[0] / outBase.amp[0] - expectedAmpRatioL) < 1.0e-4f,
              "L x2 halves the frequency and scales the amplitude by 2^-1.5");
    }

    // ---- (6) mask 閾値で帯域が落ちる ----
    {
        DmNetHeader hdr;
        hdr.refAlpha = 0.0f;
        hdr.refBeta = 0.0f;
        hdr.logAmpMin = -1.0f;
        hdr.logAmpMax = 1.0f;
        hdr.bandCenterHz[5] = 300.0f;
        ModalCellFeature feat{};
        feat.v[MaskCh(0, 5)] = 0.0f; // シグモイド後 0.5 相当の logit
        feat.v[AmpCh(0, 5)] = 0.5f;
        const float k[3] = { 1.0f, 0.0f, 0.0f };
        ModalPostParams post;
        post.alpha = 0.0f;
        post.beta = 0.0f;

        ModalModeSet loose, strict;
        post.maskThreshold = 0.1f; // 閾値ロジット = ln(0.1/0.9) < 0 → 0.0 logit は通る
        BuildModes(feat, k, hdr, post, loose);
        post.maskThreshold = 0.9f; // 閾値ロジット = ln(0.9/0.1) > 0 → 0.0 logit は落ちる
        BuildModes(feat, k, hdr, post, strict);
        check(loose.count == 1 && strict.count == 0,
              "raising ModalSound.maskThreshold past the mask logit drops the band");
    }

    // ---- (7) k=0 → count 0 ----
    {
        DmNetHeader hdr;
        hdr.bandCenterHz[0] = 200.0f;
        ModalCellFeature feat{};
        feat.v[MaskCh(0, 0)] = 10.0f;
        feat.v[AmpCh(0, 0)] = 0.9f;
        const float kZero[3] = { 0.0f, 0.0f, 0.0f };
        ModalPostParams post;
        ModalModeSet out;
        BuildModes(feat, kZero, hdr, post, out);
        check(out.count == 0, "a zero force vector never excites any mode");
    }

    // ---- (8) 過減衰 / ナイキスト超えの棄却 ----
    {
        DmNetHeader hdr;
        hdr.bandCenterHz[0] = 300.0f;   // 通常の減衰なら生存する帯域
        hdr.bandCenterHz[1] = 21000.0f; // 0.45*44100=19845Hz を超える帯域
        hdr.refAlpha = 0.0f;
        hdr.refBeta = 0.0f;
        hdr.logAmpMin = -1.0f;
        hdr.logAmpMax = 1.0f;
        ModalCellFeature feat{};
        feat.v[MaskCh(0, 0)] = 10.0f;
        feat.v[AmpCh(0, 0)] = 0.5f;
        feat.v[MaskCh(0, 1)] = 10.0f;
        feat.v[AmpCh(0, 1)] = 0.5f;
        const float k[3] = { 1.0f, 0.0f, 0.0f };

        ModalPostParams overDamped;
        overDamped.young = hdr.refYoung;
        overDamped.density = hdr.refDensity;
        overDamped.sizeL = hdr.refSizeL;
        overDamped.alpha = 1.0e5f; // c を巨大にして lambda - c^2 <= 0 (過減衰) にする
        overDamped.beta = 0.0f;
        ModalModeSet outDamped;
        BuildModes(feat, k, hdr, overDamped, outDamped);
        bool band0Present = false;
        for (int32_t i = 0; i < outDamped.count; ++i) {
            band0Present = band0Present || std::fabs(outDamped.freqHz[i] - 300.0f) < 50.0f;
        }
        check(!band0Present, "extreme Rayleigh alpha over-damps band 0 and it is dropped");

        ModalPostParams normal;
        normal.young = hdr.refYoung;
        normal.density = hdr.refDensity;
        normal.sizeL = hdr.refSizeL;
        ModalModeSet outNormal;
        BuildModes(feat, k, hdr, normal, outNormal);
        bool band1Present = false;
        for (int32_t i = 0; i < outNormal.count; ++i) {
            band1Present = band1Present || outNormal.freqHz[i] > 19000.0f;
        }
        check(outNormal.count == 1 && !band1Present,
              "a band centered above the Nyquist limit is dropped even though band 0 survives");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== ModalSynth self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== ModalSynth self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
