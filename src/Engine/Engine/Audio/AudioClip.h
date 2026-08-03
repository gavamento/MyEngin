#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// デコード済み PCM。エンジン内部の音声は **16bit 符号付き整数・インターリーブ**へ正規化する
// (XAudio2 のソースボイスへそのまま渡せる形。8/24/32bit や IEEE float の入力も 16bit へ落とす)。
// サンプル配置: frame0 の全ch → frame1 の全ch → ... (WAVE と同じ順)。
struct AudioClip {
    std::vector<int16_t> samples;
    uint32_t sampleRate = 44100;
    uint16_t channels = 1;

    size_t Frames() const { return channels != 0 ? samples.size() / channels : 0; }
    double Seconds() const
    {
        return sampleRate != 0 ? static_cast<double>(Frames()) / sampleRate : 0.0;
    }
    bool Empty() const { return samples.empty() || channels == 0 || sampleRate == 0; }
    size_t ByteSize() const { return samples.size() * sizeof(int16_t); }
};

// ---- デコーダ (すべて純関数 = ファイル I/O も D3D も XAudio2 も踏まない) ----
// selftest がデバイス無しで叩けるよう、バイト列 → AudioClip の変換だけを担う。

// RIFF/WAVE のヘッダ解析結果。**デコードせずに** data チャンクの位置と形式だけを返す
// (ストリーミング再生 (M45f) が数百 MB の wav を丸ごと展開せずに読み進むために要る)
struct WavInfo {
    uint16_t formatTag = 0; // 1 = PCM / 3 = IEEE float (EXTENSIBLE は SubFormat まで解決済み)
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bits = 0;
    size_t dataOffset = 0;          // bytes 先頭から data チャンク**本体**までのオフセット
    size_t dataBytes = 0;           // len でクランプ済み (壊れたファイルで溢れないこと)
    uint64_t dataBytesDeclared = 0; // チャンクヘッダが名乗る長さ (クランプ前)

    uint16_t BytesPerSample() const { return static_cast<uint16_t>(bits / 8u); }
    uint32_t BytesPerFrame() const { return static_cast<uint32_t>(channels) * BytesPerSample(); }
};

// ヘッダだけを解析する。**先頭数十 KB だけ渡してもよい** (data 本体は含まれていなくてよい) —
// 見つかった data チャンク本体の長さは、クランプ前の生値が dataBytesDeclared に入る。
// 非対応フォーマット (ADPCM 等) と壊れたファイルは false
bool ParseWavHeader(const uint8_t* bytes, size_t len, WavInfo& out);

// WAVE の生サンプル列 → 16bit PCM。count は**サンプル数** (フレーム数 × チャンネル数)。
// **DecodeWav もストリーマもここを通す** — 対応フォーマットの判断が 2 箇所に分かれると、
// 「全展開では鳴るのにストリーミングでは無音」のような食い違いが静かに入る
void ConvertWavSamples(const uint8_t* src, size_t count, uint16_t formatTag, uint16_t bits,
                       int16_t* dst);

// RIFF/WAVE。PCM 8/16/24/32bit・IEEE float 32/64bit・WAVE_FORMAT_EXTENSIBLE に対応
bool DecodeWav(const uint8_t* bytes, size_t len, AudioClip& out);

// Ogg Vorbis (stb_vorbis)。3ch 以上は STB_VORBIS_MAX_CHANNELS=2 の制約で失敗する
bool DecodeOgg(const uint8_t* bytes, size_t len, AudioClip& out);

// 先頭マジック ("RIFF" / "OggS") で振り分ける。拡張子には依存しない
bool DecodeAudio(const uint8_t* bytes, size_t len, AudioClip& out);

// ファイルを丸ごと読んで DecodeAudio に流す (唯一の非純粋な入口)
bool LoadAudioFile(const std::wstring& path, AudioClip& out);

} // namespace mye
