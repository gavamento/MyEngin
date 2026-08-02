#include "Engine/Engine/Audio/SynthCore.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// 位相 [0,1) から素の波形値 [-1,1] を得る (Noise 以外)
double WaveValue(SynthWave wave, double phase01, double duty)
{
    switch (wave) {
    case SynthWave::Sine:
        return std::sin(phase01 * kTwoPi);
    case SynthWave::Square:
        return phase01 < duty ? 1.0 : -1.0;
    case SynthWave::Saw:
        return 2.0 * phase01 - 1.0;
    case SynthWave::Triangle:
        return phase01 < 0.5 ? (4.0 * phase01 - 1.0) : (3.0 - 4.0 * phase01);
    case SynthWave::Noise:
    default:
        return 0.0; // Noise は呼び出し側で Pcg32 から取る
    }
}

// ADSR。t はサンプル時刻 (秒)、total は全長 (秒)
double Envelope(double t, double total, double attack, double decay, double sustain, double release)
{
    // 3 区間の合計が全長を超える場合は比例縮小する (パラメータの矛盾で無音にしない)
    const double sum = attack + decay + release;
    if (sum > total && sum > 0.0) {
        const double k = total / sum;
        attack *= k;
        decay *= k;
        release *= k;
    }
    const double releaseStart = total - release;
    if (t < attack) {
        return attack > 0.0 ? (t / attack) : 1.0;
    }
    if (t < attack + decay) {
        return decay > 0.0 ? (1.0 + (sustain - 1.0) * ((t - attack) / decay)) : sustain;
    }
    if (t < releaseStart) {
        return sustain;
    }
    if (release <= 0.0) {
        return sustain;
    }
    const double k = (total - t) / release;
    return sustain * (k > 0.0 ? k : 0.0);
}

} // namespace

void SynthRender(const SynthParams& params, AudioClip& out)
{
    out = AudioClip{};
    const uint32_t sampleRate = params.sampleRate != 0 ? params.sampleRate : 44100u;
    const uint16_t channels = params.channels != 0 ? params.channels : uint16_t{ 1 };
    const double total = params.durationSec > 0.0f ? static_cast<double>(params.durationSec) : 0.0;
    const size_t frames = static_cast<size_t>(total * sampleRate + 0.5);
    if (frames == 0) {
        return;
    }

    out.sampleRate = sampleRate;
    out.channels = channels;
    out.samples.resize(frames * channels);

    Pcg32 rng;
    rng.Seed(params.noiseSeed);

    const double amp = static_cast<double>(Clamp01(params.amplitude));
    const double duty = static_cast<double>(Clamp01(params.duty));
    const double f0 = static_cast<double>(params.freqStart);
    const double f1 = static_cast<double>(params.freqEnd);
    const double invRate = 1.0 / static_cast<double>(sampleRate);

    // 位相は周波数を積分して進める (スイープでも位相が連続する = クリックノイズが出ない)
    double phase = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) * invRate;
        const double u = frames > 1 ? static_cast<double>(i) / static_cast<double>(frames - 1) : 0.0;
        const double freq = f0 + (f1 - f0) * u;

        double v = 0.0;
        if (params.wave == SynthWave::Noise) {
            v = static_cast<double>(rng.NextFloat01()) * 2.0 - 1.0;
        } else {
            v = WaveValue(params.wave, phase, duty);
        }
        const double env = Envelope(t, total, static_cast<double>(params.attackSec),
                                    static_cast<double>(params.decaySec),
                                    static_cast<double>(Clamp01(params.sustainLevel)),
                                    static_cast<double>(params.releaseSec));
        double s = v * env * amp * 32767.0;
        if (s > 32767.0) {
            s = 32767.0;
        } else if (s < -32768.0) {
            s = -32768.0;
        }
        const int16_t q = static_cast<int16_t>(s >= 0.0 ? s + 0.5 : s - 0.5);
        for (uint16_t c = 0; c < channels; ++c) {
            out.samples[i * channels + c] = q; // 全チャンネル同一 (モノラル素材の複製)
        }

        phase += freq * invRate;
        if (phase >= 1.0) {
            phase -= std::floor(phase);
        }
    }
}

bool WriteWavToMemory(const AudioClip& clip, std::vector<uint8_t>& out)
{
    out.clear();
    if (clip.Empty()) {
        return false;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(clip.ByteSize());
    const uint16_t channels = clip.channels;
    const uint32_t sampleRate = clip.sampleRate;
    const uint16_t bits = 16;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t byteRate = sampleRate * blockAlign;

    out.resize(44 + dataBytes);
    uint8_t* p = out.data();
    auto put32 = [&p](uint32_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        p += 4;
    };
    auto put16 = [&p](uint16_t v) {
        p[0] = static_cast<uint8_t>(v & 0xFF);
        p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        p += 2;
    };
    auto putTag = [&p](const char* s) {
        std::memcpy(p, s, 4);
        p += 4;
    };

    putTag("RIFF");
    put32(36 + dataBytes);
    putTag("WAVE");
    putTag("fmt ");
    put32(16);
    put16(1); // WAVE_FORMAT_PCM
    put16(channels);
    put32(sampleRate);
    put32(byteRate);
    put16(blockAlign);
    put16(bits);
    putTag("data");
    put32(dataBytes);
    std::memcpy(p, clip.samples.data(), dataBytes);
    return true;
}

bool WriteWavToFile(const AudioClip& clip, const std::wstring& path)
{
    std::vector<uint8_t> bytes;
    if (!WriteWavToMemory(clip, bytes)) {
        MYE_LOG_WARN("[audio] wav encode failed (empty clip): %s", WideToUtf8(path).c_str());
        return false;
    }
    std::ofstream f(std::filesystem::path{ path }, std::ios::binary | std::ios::trunc);
    if (!f) {
        MYE_LOG_ERROR("[audio] could not write wav: %s", WideToUtf8(path).c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        MYE_LOG_ERROR("[audio] wav write failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    MYE_LOG_INFO("[audio] wrote wav: %s (%zu bytes)", WideToUtf8(path).c_str(), bytes.size());
    return true;
}

} // namespace mye
