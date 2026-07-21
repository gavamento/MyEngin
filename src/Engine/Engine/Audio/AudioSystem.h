#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// XAudio2 インターフェイスは .cpp でのみ完全定義を使う (前方宣言でヘッダを軽く保つ)
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace mye {

// XAudio2 ベースの簡易オーディオ (M19)。**出力 sink であり決定論レーン外**。
// スクリプトが tick 内で積んだ再生イベントを、EngineLoop がハッシュ後に Play へ流す。
// voice 状態は絶対に hashed state へ戻さない。
class AudioSystem {
public:
    bool Init();
    void Shutdown();
    bool IsReady() const { return xaudio_ != nullptr; }

    // .wav (PCM 整数) をキーで登録。冪等 (登録済みキーは何もしない)。成功で true
    bool LoadWav(const std::string& key, const std::wstring& path);

    // 登録済みサウンドをワンショット再生 (volume 0..1)。未登録キーは無視
    void Play(const std::string& key, float volume);

    // 毎フレーム呼ぶ: 再生し終えた source voice を破棄してプールを掃除する
    void Update();

private:
    struct Sound {
        std::vector<uint8_t> data; // PCM バイト列
        uint16_t formatTag = 1;    // WAVE_FORMAT_PCM
        uint16_t channels = 1;
        uint32_t sampleRate = 44100;
        uint32_t byteRate = 0;
        uint16_t blockAlign = 0;
        uint16_t bitsPerSample = 16;
    };

    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    std::vector<IXAudio2SourceVoice*> active_; // 再生中の source voice
    std::unordered_map<std::string, Sound> sounds_;
    bool comInit_ = false;
};

} // namespace mye
