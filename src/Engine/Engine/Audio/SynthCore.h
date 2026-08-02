#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Audio/AudioClip.h"

namespace mye {

// 手続き的な波形生成。**純関数** (I/O もデバイスも踏まない) なのでヘッドレス selftest から
// 直接叩ける。M45b の Sound Generator ウィンドウはこの上に UI を被せるだけ。
// 乱数は spec 11.2 規則 8 に従い Pcg32 のみ (rand() 禁止)。

enum class SynthWave : int32_t {
    Sine = 0,
    Square = 1,
    Saw = 2,
    Triangle = 3,
    Noise = 4,
};

struct SynthParams {
    SynthWave wave = SynthWave::Sine;
    float freqStart = 440.0f;    // Hz
    float freqEnd = 440.0f;      // Hz (freqStart と同じならスイープ無し)
    float durationSec = 0.5f;
    float amplitude = 0.5f;      // 0..1
    float duty = 0.5f;           // Square のデューティ比 0..1
    // ADSR (秒)。attack + decay + release が durationSec を超える場合は比例縮小される
    float attackSec = 0.01f;
    float decaySec = 0.05f;
    float sustainLevel = 0.7f;   // 0..1
    float releaseSec = 0.10f;
    uint32_t sampleRate = 44100;
    uint16_t channels = 1;
    uint64_t noiseSeed = 0x9E3779B97F4A7C15ull; // Noise 波形の決定論シード
};

// params から PCM を合成する。同じ params なら常にビット単位で同じ結果になる
void SynthRender(const SynthParams& params, AudioClip& out);

// AudioClip → 16bit PCM の RIFF/WAVE バイト列 (44 バイトヘッダ + data)
bool WriteWavToMemory(const AudioClip& clip, std::vector<uint8_t>& out);

// WriteWavToMemory の結果をファイルへ書く (唯一の非純粋な出口)
bool WriteWavToFile(const AudioClip& clip, const std::wstring& path);

} // namespace mye
