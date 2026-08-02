#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/SpatialMath.h"
#include "Engine/Engine/Audio/VoicePolicy.h"

// XAudio2 インターフェイスは .cpp でのみ完全定義を使う (前方宣言でヘッダを軽く保つ)
struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SubmixVoice;
struct IXAudio2SourceVoice;

namespace mye {

struct AssetEntry;
struct MixerAsset; // Audio/AudioMixer.h (json を引き込まないようここでは前方宣言)

// 再生ハンドル。世代タグ付きなので、スロットが別の音に再利用された後に来た
// Stop / SetVolume を安全に無視できる (generation==0 は「無効」の予約値)
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

// 1 音源ぶんの 3D パラメータ (M45e)。ワールド座標はエンジンと同じ左手系・メートル単位で、
// X3DAudio も左手系なので**変換は不要**。**決定論レーンの外**
struct AudioSpatial {
    AudioVec3 position;
    AudioVec3 velocity;        // m/s。ドップラー用 (tick 差分で推定する。SpatialMath.h 参照)
    float spatialBlend = 1.0f; // 0 = 2D (定位も減衰もドップラーも無し) / 1 = フル 3D
    float minDistance = 1.0f;
    float maxDistance = 50.0f; // これ以遠は無音 (SpatialMath.h の BuildRolloffCurve の規約)
    int rolloff = 0;           // SoundRolloff と同じ並び (0=Log 1=Linear 2=Inverse)
    float dopplerScale = 1.0f; // 0 = ドップラー無効
    float reverbSend = 0.0f;   // リバーブバスへの送り量。X3DAudio の ReverbLevel に乗る
    float pitch = 1.0f;        // ドップラーと合成する基準ピッチ (アセット/コンポーネント由来)
};

// リスナー (3D 定位の基準点)。向きは正規化済み・直交していること
struct AudioListenerState {
    AudioVec3 position;
    AudioVec3 forward = { 0.0f, 0.0f, 1.0f }; // +Z が前 (エンジンの規約)
    AudioVec3 up = { 0.0f, 1.0f, 0.0f };
    AudioVec3 velocity;
};

// 1 回の再生指定
struct PlayDesc {
    AssetID clip = {};
    int bus = 2;             // 既定バス構成での SE (AudioSystem::kBusSe)
    float volume = 1.0f;     // 線形 0..1 (バス音量とは別に掛かる)
    float pitch = 1.0f;      // 周波数比。kMaxFreqRatio で飽和する
    bool loop = false;
    int32_t priority = 128;  // **大きいほど重要** (VoicePolicy.h と同じ規約)
    // 3D 定位 (M45e)。非 null なら Start() の **前** に定位を適用する — 再生開始の
    // 1 quantum ぶんが無定位で鳴るのを防ぐため。**Play() の呼び出し中だけ有効な借用ポインタ**
    const AudioSpatial* spatial = nullptr;
};

// バス 1 本のランタイム状態 (.mixer.json の MixerBus を「親名 → index」まで解決した形)。
// **決定論レーン外** — sim はこの値を一切読まない
struct AudioBusState {
    std::string name;
    int parent = -1;       // -1 = ルート (mastering voice 直下)
    float volumeDb = 0.0f; // kMinDb (AudioMixer.h) で無音
    bool mute = false;
    bool solo = false;
    float reverbSend = 0.0f; // ルートバスは常に 0 (reverb の出力先がルートのため)
};

// XAudio2 ベースのオーディオ。**出力 sink であり決定論レーン外** (M19 からの不変条件)。
// スクリプトが tick 内で積んだ再生イベントを、EngineLoop がハッシュ後に流す。
// **voice 状態は絶対に hashed state へ戻さない**。読み取り API を sim へ公開しないこと
// (再生位置や再生中判定を sim が読むと、その瞬間にリプレイが壊れる)。
class AudioSystem {
public:
    // 既定バス構成 (.mixer.json が無い時の index)。M45d でバスはデータ駆動になったので、
    // これは「DefaultMixer() の並び」を指す定数でしかない — バス数は BusCount() で見ること
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
    // メーターが扱う最大チャンネル数 (7.1 まで)
    static constexpr int kMaxBusChannels = 8;
    // X3DAUDIO_HANDLE のバイト数 (x3daudio.h の X3DAUDIO_HANDLE_BYTESIZE)。
    // このヘッダへ x3daudio.h (と Windows.h) を引き込まないため生バイトで持つ。
    // SDK 定義とのズレは AudioSystem.cpp の static_assert で検出する
    static constexpr uint32_t kSpatialHandleBytes = 20;

    AudioSystem(); // 既定バス構成をデータとして持つ (デバイスが無くても FindBus が働く)

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
    // ディスク上の .wav/.ogg が書き換わったときの差し替え (M45c、ReloadHub 用)。
    // **未登録のパスは何もしない** (見ていないファイルを勝手に鳴らす側へ引き込まない)。
    // 戻り値 false = 未登録 or 読み込み失敗 (呼び出し側は共有違反としてリトライしてよい)
    bool ReloadClipFile(const std::wstring& path);

    // ---- 再生 ----
    AudioHandle Play(const PlayDesc& desc);
    void Stop(AudioHandle h);
    void SetVoiceVolume(AudioHandle h, float volume);
    void SetVoicePitch(AudioHandle h, float pitch);
    void StopAll();
    int ActiveVoiceCount() const;

    // ---- 3D 定位 (M45e) ----
    // X3DAudio が使えるか。デバイスのチャンネルマスクが取れない環境 (仮想/ループバック) では
    // false になり、**オーディオ全体は落とさず 2D 経路のまま**動き続ける
    bool SpatialReady() const { return x3dReady_; }
    // リスナーを差し替える。AudioSourceSystem がフレーム毎に 1 回だけ呼ぶ
    void SetListener(const AudioListenerState& listener) { listener_ = listener; }
    const AudioListenerState& Listener() const { return listener_; }
    // 再生中の voice へ定位を反映する。ハンドルが古ければ何もしない (false)。
    // spatialBlend の分だけ「XAudio2 の既定行列 (= 2D)」と 3D 行列を線形補間する
    bool ApplyVoiceSpatial(AudioHandle h, const AudioSpatial& s);

    // ---- バス / ミキサー (M45d) ----
    int BusCount() const { return static_cast<int>(buses_.size()); }
    const char* BusName(int bus) const;   // 範囲外は ""
    int BusParent(int bus) const;         // -1 = ルート / 範囲外
    int FindBus(const char* name) const;  // 大文字小文字を無視。見つからなければ -1
    int RootBus() const { return rootBus_; }
    // 名前が解決できなかった音の行き先 ("SE" があればそこ、無ければルート)
    int DefaultBus() const;

    float BusVolumeDb(int bus) const;
    void SetBusVolumeDb(int bus, float db);
    void SetBusVolume(int bus, float linear); // 線形指定 (スクリプト v8 の SetBusVolume)
    float BusVolume(int bus) const;           // 線形
    bool BusMute(int bus) const;
    void SetBusMute(int bus, bool mute);
    bool BusSolo(int bus) const;
    void SetBusSolo(int bus, bool solo);
    float BusReverbSend(int bus) const;
    void SetBusReverbSend(int bus, float send);

    bool HasReverbBus() const { return reverbVoice_ != nullptr; }
    int ReverbPreset() const { return reverbPreset_; }
    void SetReverbPreset(int index);
    float ReverbWetDryMix() const { return reverbWetDry_; }
    void SetReverbWetDryMix(float percent);

    // トポロジ変更 (バスの追加/削除/改名/親変更)。**次の Update() で 1 回だけ**
    // グラフを作り直す — UI の連続操作をフレーム境界で束ねるため。
    // 検証に失敗したミキサーは適用せず、現在のグラフを保つ
    void ApplyMixer(const MixerAsset& m);
    // ランタイムの現在値を .mixer.json 形式へ (Save 用)
    MixerAsset CurrentMixer() const;

    // メーター。**窓が開いている間だけ**呼ぶ (dt = 実時間秒)。
    // 6500fps では同じ処理済みブロックを何度も読むので、ここで実時間の減衰を掛けないと
    // バーが固まる/暴れる。流入する音源が 1 つも無いバスは APO が回らず値が固まるため、
    // 「そのバスへ流れ込む active voice が居るか」で 0 へ落とす
    void PollBusMeters(float dt);
    float BusLevel(int bus) const;    // 線形ピーク (1.0 = 0 dBFS。アタック即時・リリース減衰)
    float BusPeakHold(int bus) const; // 線形ピーク (ゆっくり落ちるピーク保持)

    // ---- フレーム更新 ----
    // 記録/検証中や編集中に true。**再生は止まるが voice 回収は動き続ける**
    // (完全に early-out すると再生し終えた voice がリークする)
    void SetSuspended(bool suspended);
    bool IsSuspended() const { return suspended_; }
    void Update(); // フレーム毎。終了 voice の回収 + 保留中のバスグラフ再構築

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

    // ランタイムのバス 1 本 (enum Bus と名前が衝突しないよう Slot を付ける)
    struct BusSlot {
        AudioBusState s;
        IXAudio2SubmixVoice* voice = nullptr;
        int meterEffect = -1; // volume meter APO の effect index (-1 = メーター無し)
        float level = 0.0f;   // 表示用 (アタック即時・リリース減衰)
        float hold = 0.0f;    // 表示用 (ピーク保持)
    };

    bool BuildBusGraph();
    void DestroyBusGraph();
    void DestroyAllSourceVoices(); // グラフ再構築の前に必ず通す (送り先が消えるため)
    void RebuildBusGraphNow();
    void ApplyBusGains();   // 音量/ミュート/ソロ → 各サブミックスの SetVolume
    void ApplyReverbSends(); // reverbSend → 各バスの reverb 送り行列
    void ApplyReverbParams();
    void UpdateRootBus();
    bool ValidBus(int bus) const { return bus >= 0 && bus < static_cast<int>(buses_.size()); }
    bool EnsureSourceVoice(Voice& v, uint16_t channels, uint32_t sampleRate, int bus);
    void StopSlot(Voice& v);
    int AcquireSlot(int32_t priority);

    // ---- 3D 定位 (M45e) ----
    void InitSpatial(); // X3DAudioInitialize。失敗しても Init 全体は成功のまま (2D で動く)
    void ApplySpatialToVoice(Voice& v, const AudioSpatial& s);
    // voice をスロット再利用したときに前の音の定位が残らないよう既定状態へ戻す。
    // **これを忘れると、3D で使ったスロットに 2D の音が乗った瞬間に減衰が引き継がれる**
    void ResetVoiceTo2D(Voice& v);
    // XAudio2 が voice 生成時に張る既定行列 (= 2D の素通し)。自前で書き起こすと
    // 5.1/7.1 環境で今までと違う鳴り方になるので、**生成直後に読み出して控える**。
    // src チャンネル数だけが変数 (送り先バスは全て masterChannels_ 幅で固定)
    const float* DefaultMatrixFor(Voice& v);

    IXAudio2* xaudio_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SubmixVoice* reverbVoice_ = nullptr; // 全ソースボイスの 2 本目の send 先

    std::vector<BusSlot> buses_;
    int rootBus_ = 0;
    int reverbPreset_ = 0;
    float reverbWetDry_ = 100.0f;

    // 保留中のトポロジ変更 (Update() の頭で 1 回だけ消費する)
    std::vector<AudioBusState> pendingBuses_;
    int pendingReverbPreset_ = 0;
    float pendingReverbWetDry_ = 100.0f;
    bool pendingRebuild_ = false;

    std::vector<Voice> voices_;
    std::unordered_map<uint64_t, Clip> clips_;    // AssetID.value → PCM
    std::unordered_map<uint64_t, AssetID> named_; // HashStr(名前キー) → AssetID

    uint32_t masterChannels_ = 2;
    uint32_t masterSampleRate_ = 44100;
    uint64_t playSeq_ = 0; // 再生開始の通し番号 (スティールの「古さ」判定用)
    bool suspended_ = false;
    bool comInit_ = false;

    // ---- 3D 定位 (M45e) ----
    // X3DAUDIO_HANDLE は BYTE[20]。**uint32_t 配列で持つ**のは、alignas(4) を付けると
    // C4324 (アラインメント指定子による構造体パディング) が出るため — uint32_t なら
    // 4 バイト境界が自然に保証され、警告も出ない
    uint32_t x3dHandle_[kSpatialHandleBytes / 4] = {};
    bool x3dReady_ = false;
    AudioListenerState listener_;
    // スクラッチ (voice 毎フレームの確保を避ける)。X3DAudio は最大 8ch を想定
    float matrixScratch_[kMaxBusChannels * kMaxBusChannels] = {};
    float blendScratch_[kMaxBusChannels * kMaxBusChannels] = {};
    float delayScratch_[kMaxBusChannels] = {};
    // src チャンネル数 (1 / 2) 毎の既定行列キャッシュ。masterChannels_ は Init 後不変なので
    // 送り先による差は出ない
    float defaultMatrix_[2][kMaxBusChannels * kMaxBusChannels] = {};
    bool defaultMatrixValid_[2] = { false, false };
};

} // namespace mye
