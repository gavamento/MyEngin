#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/VoicePolicy.h"

// XAudio2 インターフェイスは .cpp でのみ完全定義を使う (前方宣言でヘッダを軽く保つ)
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2SourceVoice;

namespace mye {

struct AssetEntry;

// 再生ハンドル。世代タグ付きなので、スロットが別の音に再利用された後に来た
// Stop / SetVolume を安全に無視できる (generation==0 は「無効」の予約値)。
struct AudioHandle {
    uint32_t index = 0;
    uint32_t generation = 0;

    bool Valid() const { return generation != 0; }
    uint64_t Pack() const
    {
        return (static_cast<uint64_t>(generation) << 32) | static_cast<uint64_t>(index);
    }
    static AudioHandle Unpack(uint64_t v)
    {
        AudioHandle h;
        h.index = static_cast<uint32_t>(v & 0xFFFFFFFFu);
        h.generation = static_cast<uint32_t>(v >> 32);
        return h;
    }
};

// 1 回の再生指定。3D 用フィールドは M45e で足す (ここでは 2D 部分だけ)
struct PlayDesc {
    AssetID clip = {};
    int bus = 2;             // AudioSystem::kBusSe
    float volume = 1.0f;     // 線形 0..1 (バス音量とは別に掛かる)
    float pitch = 1.0f;      // 周波数比。kMaxFreqRatio で飽和する
    bool loop = false;
    int32_t priority = 128;  // **大きいほど重要** (VoicePolicy.h と同じ規約)
};

// XAudio2 ベースのオーディオ。**出力 sink であり決定論レーン外** (M19 からの不変条件)。
// スクリプトが tick 内で積んだ再生イベントを、EngineLoop がハッシュ後に流す。
// **voice 状態は絶対に hashed state へ戻さない**。読み取り API を sim へ公開しないこと
// (再生位置や再生中判定を sim が読むと、その瞬間にリプレイが壊れる)。
class AudioSystem {
public:
    // 既定バス。.mixer.json でのデータ駆動化は M45d。並びを変えると既存 .sound.json が壊れる
    enum Bus : int {
        kBusMaster = 0,
        kBusBgm = 1,
        kBusSe = 2,
        kBusUi = 3,
        kBusCount = 4,
    };

    // 同時発音数の上限。超過分は VoicePolicy の規則でスティールする
    static constexpr int kMaxVoices = 64;
    // ソースボイス生成時に固定する周波数比の上限。ドップラー (M45e) × ピッチ揺らぎ (M45c) を
    // 合成しても飽和しないよう、XAUDIO2_DEFAULT_FREQ_RATIO (2.0) ではなく 4.0 を使う
    static constexpr float kMaxFreqRatio = 4.0f;

    bool Init(bool enabled = true); // enabled=false (--no-audio) なら全 API が no-op になる
    void Shutdown();
    bool IsReady() const { return xaudio_ != nullptr; }

    // ---- クリップ ----
    // パス → AssetID (= GUID。移動/リネームに追従。TextureLibrary::IdForFile と同一規約)
    static AssetID IdForFile(const std::wstring& path);
    AssetID LoadClipFile(const std::wstring& path); // 冪等。失敗は null AssetID
    // 手続き生成クリップ等をそのまま登録する (Sound Generator のプレビュー / selftest 用)
    AssetID RegisterClip(AssetID id, AudioClip clip, const std::string& name);
    bool HasClip(AssetID id) const { return clips_.find(id.value) != clips_.end(); }
    std::vector<AssetEntry> Enumerate() const;
    // ★ホットリロード前に必ず呼ぶこと: 再生中の XAUDIO2_BUFFER はクリップのバイト列を
    //   直接指しているので、停止せずに差し替えると use-after-free になる
    void StopVoicesUsingClip(AssetID id);

    // ---- 再生 ----
    AudioHandle Play(const PlayDesc& desc);
    void Stop(AudioHandle h);
    void SetVoiceVolume(AudioHandle h, float volume);
    void SetVoicePitch(AudioHandle h, float pitch);
    void StopAll();
    int ActiveVoiceCount() const;

    // ---- バス ----
    void SetBusVolume(int bus, float linear);
    float BusVolume(int bus) const;
    static const char* BusName(int bus);
    static int FindBus(const char* name); // 見つからなければ -1

    // ---- フレーム更新 ----
    // 記録/検証中や編集中に true。**再生は止まるが voice 回収は動き続ける**
    // (完全に early-out すると再生し終えた voice がリークする)
    void SetSuspended(bool suspended);
    bool IsSuspended() const { return suspended_; }
    void Update(); // フレーム毎。終了 voice の回収

    // ---- 名前キー (互換シム。M19 の API と、それを 64bit へ潰したスクリプト経路) ----
    bool LoadWav(const std::string& key, const std::wstring& path);
    void Play(const std::string& key, float volume);
    // ScriptAudioEvent.key (64bit) → AssetID。name-key ハッシュとして解決を試み、
    // 該当が無ければ AssetID そのものとみなす (スクリプトは名前でもパス GUID でも指せる)
    AssetID ResolveClipKey(uint64_t key) const;

private:
    struct Clip {
        AudioClip data;
        std::string name;
    };

    // 1 スロット = 1 ソースボイス。フォーマット/バスが一致する限り voice を作り直さない
    // (send リストは CreateSourceVoice 時に固定されるので、バスが変わると作り直しが要る)
    struct Voice {
        IXAudio2SourceVoice* voice = nullptr;
        uint32_t generation = 0; // 0 = 未使用。Play の度に +1
        bool active = false;
        int32_t priority = 128;
        uint64_t startSeq = 0;
        AssetID clip = {};
        // voice を再利用できるかの判定キー
        int bus = kBusSe;
        uint16_t channels = 0;
        uint32_t sampleRate = 0;
    };

    bool BuildBusGraph();
    void DestroyBusGraph();
    bool EnsureSourceVoice(Voice& v, uint16_t channels, uint32_t sampleRate, int bus);
    void StopSlot(Voice& v);
    int AcquireSlot(int32_t priority);

    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SubmixVoice* busVoice_[kBusCount] = {};
    IXAudio2SubmixVoice* reverbVoice_ = nullptr; // 全ソースボイスの 2 本目の send 先
    float busVolume_[kBusCount] = { 1.0f, 1.0f, 1.0f, 1.0f };

    std::vector<Voice> voices_;
    std::unordered_map<uint64_t, Clip> clips_;   // AssetID.value → PCM
    std::unordered_map<uint64_t, AssetID> named_; // HashStr(名前キー) → AssetID

    uint32_t masterChannels_ = 2;
    uint32_t masterSampleRate_ = 44100;
    uint64_t playSeq_ = 0; // 再生開始の通し番号 (スティールの「古さ」判定用)
    bool suspended_ = false;
    bool comInit_ = false;
};

} // namespace mye
