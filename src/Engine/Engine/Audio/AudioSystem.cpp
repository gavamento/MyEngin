#include "Engine/Engine/Audio/AudioSystem.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <Windows.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/GpuResources.h" // AssetEntry

// XAudio2 2.9 は Win10+ に標準搭載 (再頒布 DLL 不要)。Engine.lib 経由で exe に伝播。
// ★このライブラリ 1 本で X3DAudioInitialize / X3DAudioCalculate / CreateAudioReverb /
//   CreateAudioVolumeMeter も供給される。**x3daudio.lib は Win10 SDK に存在しない**ので
//   追加してはいけない (LNK1104 になる)。実測で確認済み。
#pragma comment(lib, "xaudio2.lib")

namespace mye {
namespace {

// リバーブサブミックスは APO の制約でステレオ・[20000,48000]Hz に限られる
// (xaudio2fx.h:104-122)。96/192kHz デバイスでも作成に失敗しないよう必ずクランプする。
constexpr uint32_t kReverbMaxRate = 48000;
constexpr uint32_t kReverbChannels = 2;

// SetOutputMatrix 用のゼロ行列 (src * dst)。voice 毎フレームの確保を避けるため定数で持つ
constexpr size_t kMaxMatrix = 8 * 8;
const float kZeroMatrix[kMaxMatrix] = {};

float ClampVolume(float v)
{
    if (!(v > 0.0f)) { // NaN もここで 0 に落ちる
        return 0.0f;
    }
    return v > 1.0f ? 1.0f : v;
}

float ClampPitch(float p)
{
    if (!(p > XAUDIO2_MIN_FREQ_RATIO)) {
        return XAUDIO2_MIN_FREQ_RATIO;
    }
    return p > AudioSystem::kMaxFreqRatio ? AudioSystem::kMaxFreqRatio : p;
}

} // namespace

// ---------------------------------------------------------------------------
// 初期化 / 終了
// ---------------------------------------------------------------------------

bool AudioSystem::Init(bool enabled)
{
    if (!enabled) {
        MYE_LOG_INFO("[audio] disabled by request (--no-audio)");
        return false;
    }
    // XAudio2Create は COM を必要とする。既に初期化済みでも RPC_E_CHANGED_MODE 以外は許容
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    comInit_ = SUCCEEDED(co);

    if (FAILED(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        MYE_LOG_WARN("[audio] XAudio2Create failed — audio disabled");
        xaudio_ = nullptr;
        return false;
    }
    if (FAILED(xaudio_->CreateMasteringVoice(&master_))) {
        MYE_LOG_WARN("[audio] CreateMasteringVoice failed — audio disabled");
        xaudio_->Release();
        xaudio_ = nullptr;
        return false;
    }

    // XAUDIO2_DEVICE_DETAILS は 2.9 で廃止 → GetVoiceDetails / GetChannelMask を使う
    XAUDIO2_VOICE_DETAILS details = {};
    master_->GetVoiceDetails(&details);
    masterChannels_ = details.InputChannels != 0 ? details.InputChannels : 2u;
    masterSampleRate_ = details.InputSampleRate != 0 ? details.InputSampleRate : 44100u;

    if (!BuildBusGraph()) {
        MYE_LOG_WARN("[audio] bus graph creation failed — audio disabled");
        Shutdown();
        return false;
    }

    voices_.resize(static_cast<size_t>(kMaxVoices));
    MYE_LOG_INFO("[audio] XAudio2 ready (%u ch @ %u Hz, %d voices, %d buses)", masterChannels_,
                 masterSampleRate_, kMaxVoices, static_cast<int>(kBusCount));
    return true;
}

void AudioSystem::Shutdown()
{
    for (Voice& v : voices_) {
        if (v.voice != nullptr) {
            v.voice->Stop(0);
            v.voice->FlushSourceBuffers();
            v.voice->DestroyVoice(); // クリップのバイト列を参照する buffer を先に手放す
            v.voice = nullptr;
        }
        v.active = false;
    }
    voices_.clear();
    DestroyBusGraph();
    clips_.clear();
    named_.clear();
    if (master_ != nullptr) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (xaudio_ != nullptr) {
        xaudio_->Release();
        xaudio_ = nullptr;
    }
    if (comInit_) {
        CoUninitialize();
        comInit_ = false;
    }
}

// ---------------------------------------------------------------------------
// バスグラフ
// ---------------------------------------------------------------------------
//
//   source voice ─┬─[0]→ dryBus[b] ─→ Master ─→ mastering voice
//                 └─[1]→ reverbBus ──────┘
//
// ドライバスは **mastering voice と同じチャンネル幅**で作る。X3DAudioCalculate が返す
// 行列は「最終ミックスのチャンネル数」前提 (x3daudio.h:273) なので、途中で幅が変わると
// M45e のパンニングが黙って壊れる。
// 送り先は CreateSourceVoice 時に固定される — バス割当が変わる時だけ voice を作り直す。
bool AudioSystem::BuildBusGraph()
{
    // 親を先に作る。サブミックスは processingStage の昇順に処理されるので、
    // 子 (stage 0) → 親 (stage 1) の順序を stage で表現する
    XAUDIO2_SEND_DESCRIPTOR toMasterDesc = { 0, nullptr };
    XAUDIO2_VOICE_SENDS toMaster = { 1, &toMasterDesc };

    // Master バス (mastering voice へ送る唯一のサブミックス)
    if (FAILED(xaudio_->CreateSubmixVoice(&busVoice_[kBusMaster], masterChannels_,
                                          masterSampleRate_, 0, 1, nullptr, nullptr))) {
        return false;
    }
    // 子バス (BGM / SE / UI) → Master
    toMasterDesc.pOutputVoice = busVoice_[kBusMaster];
    for (int b = kBusMaster + 1; b < kBusCount; ++b) {
        if (FAILED(xaudio_->CreateSubmixVoice(&busVoice_[b], masterChannels_, masterSampleRate_, 0,
                                              0, &toMaster, nullptr))) {
            return false;
        }
    }

    // リバーブ用サブミックス (ステレオ・レート上限クランプ)。ウェット側は X3DAudio が
    // スカラー ReverbLevel しか返さないので、ここが 2ch でも定位は失われない
    IUnknown* reverbApo = nullptr;
    if (FAILED(XAudio2CreateReverb(&reverbApo))) {
        MYE_LOG_WARN("[audio] XAudio2CreateReverb failed — reverb bus disabled");
        return true; // リバーブ無しでも動く (ソースは send を 1 本に減らす)
    }
    XAUDIO2_EFFECT_DESCRIPTOR fx = {};
    fx.pEffect = reverbApo;
    fx.InitialState = TRUE;
    fx.OutputChannels = kReverbChannels;
    XAUDIO2_EFFECT_CHAIN chain = { 1, &fx };

    const uint32_t reverbRate = std::min(masterSampleRate_, kReverbMaxRate);
    const HRESULT hr = xaudio_->CreateSubmixVoice(&reverbVoice_, kReverbChannels, reverbRate, 0, 0,
                                                 &toMaster, &chain);
    reverbApo->Release(); // サブミックスが AddRef 済み。ここで手放さないとリークする
    if (FAILED(hr)) {
        MYE_LOG_WARN("[audio] reverb submix creation failed (0x%08lX) — reverb bus disabled",
                     static_cast<unsigned long>(hr));
        reverbVoice_ = nullptr;
        return true;
    }

    // 既定プリセットは Room=-10000 (実質無音) なので、M45d でプリセットを選ぶまで無害
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2 = XAUDIO2FX_I3DL2_PRESET_DEFAULT;
    XAUDIO2FX_REVERB_PARAMETERS native = {};
    ReverbConvertI3DL2ToNative(&i3dl2, &native, FALSE); // FALSE = 5.1/7.1 用でなくステレオ
    reverbVoice_->SetEffectParameters(0, &native, sizeof(native));
    return true;
}

void AudioSystem::DestroyBusGraph()
{
    // 子から先に破棄する (入力を持つ voice の DestroyVoice は未定義動作)
    if (reverbVoice_ != nullptr) {
        reverbVoice_->DestroyVoice();
        reverbVoice_ = nullptr;
    }
    for (int b = kBusCount - 1; b >= 0; --b) {
        if (busVoice_[b] != nullptr) {
            busVoice_[b]->DestroyVoice();
            busVoice_[b] = nullptr;
        }
    }
}

void AudioSystem::SetBusVolume(int bus, float linear)
{
    if (bus < 0 || bus >= kBusCount) {
        return;
    }
    busVolume_[bus] = linear < 0.0f ? 0.0f : linear;
    if (busVoice_[bus] != nullptr) {
        busVoice_[bus]->SetVolume(busVolume_[bus]);
    }
}

float AudioSystem::BusVolume(int bus) const
{
    return (bus >= 0 && bus < kBusCount) ? busVolume_[bus] : 0.0f;
}

const char* AudioSystem::BusName(int bus)
{
    switch (bus) {
    case kBusMaster: return "Master";
    case kBusBgm: return "BGM";
    case kBusSe: return "SE";
    case kBusUi: return "UI";
    default: return "";
    }
}

int AudioSystem::FindBus(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }
    for (int b = 0; b < kBusCount; ++b) {
        if (_stricmp(name, BusName(b)) == 0) {
            return b;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// クリップ
// ---------------------------------------------------------------------------

AssetID AudioSystem::IdForFile(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return AssetID{ assetkey::Resolve(NormalizePathKey(path)) };
}

AssetID AudioSystem::LoadClipFile(const std::wstring& path)
{
    if (xaudio_ == nullptr) {
        // --no-audio / デバイス無し。鳴らせないものを展開しても意味が無く、BGM の
        // ような大きなファイルでは無駄な時間とメモリになるだけなので何もしない
        return {};
    }
    const AssetID id = IdForFile(path);
    if (id.IsNull()) {
        return {};
    }
    if (clips_.find(id.value) != clips_.end()) {
        return id; // 冪等
    }
    AudioClip clip;
    if (!LoadAudioFile(path, clip)) {
        return {};
    }
    const double seconds = clip.Seconds();
    std::string name = WideToUtf8(std::filesystem::path{ path }.stem().wstring());
    MYE_LOG_INFO("[audio] loaded clip: %s (%u ch @ %u Hz, %.2fs)", name.c_str(), clip.channels,
                 clip.sampleRate, seconds);
    return RegisterClip(id, std::move(clip), name);
}

AssetID AudioSystem::RegisterClip(AssetID id, AudioClip clip, const std::string& name)
{
    if (id.IsNull() || clip.Empty()) {
        return {};
    }
    StopVoicesUsingClip(id); // 差し替え前に参照者を止める (use-after-free 防止)
    Clip& slot = clips_[id.value];
    slot.data = std::move(clip);
    slot.name = name;
    return id;
}

std::vector<AssetEntry> AudioSystem::Enumerate() const
{
    std::vector<AssetEntry> out;
    out.reserve(clips_.size());
    for (auto it = clips_.begin(); it != clips_.end(); ++it) {
        out.push_back(AssetEntry{ AssetID{ it->first }, it->second.name });
    }
    // 明示キーで並べる (spec 11.2 規則 7: ハッシュの反復順を表に出さない)
    std::sort(out.begin(), out.end(), [](const AssetEntry& a, const AssetEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.id.value < b.id.value;
    });
    return out;
}

bool AudioSystem::ReloadClipFile(const std::wstring& path)
{
    if (xaudio_ == nullptr) {
        return false;
    }
    const AssetID id = IdForFile(path);
    if (id.IsNull() || clips_.find(id.value) == clips_.end()) {
        return false; // ロードしていないファイルは対象外
    }
    AudioClip clip;
    if (!LoadAudioFile(path, clip)) {
        return false; // 書き込み途中の共有違反など
    }
    const std::string name = WideToUtf8(std::filesystem::path{ path }.stem().wstring());
    // RegisterClip が差し替え前に StopVoicesUsingClip を呼ぶ (use-after-free 防止)
    return !RegisterClip(id, std::move(clip), name).IsNull();
}

void AudioSystem::StopVoicesUsingClip(AssetID id)
{
    for (Voice& v : voices_) {
        if (v.active && v.clip == id) {
            StopSlot(v);
        }
    }
}

// ---------------------------------------------------------------------------
// 再生
// ---------------------------------------------------------------------------

void AudioSystem::StopSlot(Voice& v)
{
    if (v.voice != nullptr) {
        v.voice->Stop(0);
        v.voice->FlushSourceBuffers(); // 提出済み buffer の参照を切る
    }
    v.active = false;
    v.clip = {};
}

// 空きスロットを取る。無ければ VoicePolicy の規則で 1 本奪う (-1 = 再生をあきらめる)
int AudioSystem::AcquireSlot(int32_t priority)
{
    for (size_t i = 0; i < voices_.size(); ++i) {
        if (!voices_[i].active) {
            return static_cast<int>(i);
        }
    }
    std::vector<VoiceSlotInfo> info(voices_.size());
    for (size_t i = 0; i < voices_.size(); ++i) {
        info[i].active = voices_[i].active;
        info[i].priority = voices_[i].priority;
        info[i].startSeq = voices_[i].startSeq;
    }
    const int victim = PickVoiceToSteal(info.data(), info.size(), priority);
    if (victim >= 0) {
        StopSlot(voices_[static_cast<size_t>(victim)]);
    }
    return victim;
}

// send リストは CreateSourceVoice 時に固定されるので、フォーマットかバスが変われば作り直す
bool AudioSystem::EnsureSourceVoice(Voice& v, uint16_t channels, uint32_t sampleRate, int bus)
{
    if (v.voice != nullptr && v.channels == channels && v.sampleRate == sampleRate &&
        v.bus == bus) {
        return true;
    }
    if (v.voice != nullptr) {
        v.voice->Stop(0);
        v.voice->FlushSourceBuffers();
        v.voice->DestroyVoice();
        v.voice = nullptr;
    }

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(channels * 2);
    wfx.nAvgBytesPerSec = sampleRate * wfx.nBlockAlign;

    // [0] = ドライ (M45e が X3DAudio の行列を書く) / [1] = リバーブ送り (スカラー)。
    // 双方に SEND_USEFILTER を付けるのは、X3DAudio が LPF 係数を **経路別**に返すため
    // (voice 全体の SetFilterParameters では per-send を表現できない)。
    XAUDIO2_SEND_DESCRIPTOR sends[2] = {
        { XAUDIO2_SEND_USEFILTER, busVoice_[bus] },
        { XAUDIO2_SEND_USEFILTER, reverbVoice_ },
    };
    XAUDIO2_VOICE_SENDS sendList = { reverbVoice_ != nullptr ? 2u : 1u, sends };

    IXAudio2SourceVoice* sv = nullptr;
    if (FAILED(xaudio_->CreateSourceVoice(&sv, &wfx, XAUDIO2_VOICE_USEFILTER, kMaxFreqRatio,
                                          nullptr, &sendList, nullptr))) {
        return false;
    }
    v.voice = sv;
    v.channels = channels;
    v.sampleRate = sampleRate;
    v.bus = bus;

    // ★リバーブ送りは既定行列だと「素通し」になる。M45e が X3DAudio の ReverbLevel を
    //   書き込むまでは全部ドライで鳴らしたいので、生成直後にゼロで潰しておく。
    if (reverbVoice_ != nullptr) {
        v.voice->SetOutputMatrix(reverbVoice_, channels, kReverbChannels, kZeroMatrix);
    }
    return true;
}

AudioHandle AudioSystem::Play(const PlayDesc& desc)
{
    if (xaudio_ == nullptr || suspended_) {
        return {};
    }
    const auto it = clips_.find(desc.clip.value);
    if (it == clips_.end()) {
        return {};
    }
    const AudioClip& clip = it->second.data;
    if (clip.Empty()) {
        return {};
    }
    const int bus = (desc.bus >= 0 && desc.bus < kBusCount) ? desc.bus : static_cast<int>(kBusSe);

    const int slot = AcquireSlot(desc.priority);
    if (slot < 0) {
        return {}; // 全ボイスが自分より重要 = 発音を捨てる
    }
    Voice& v = voices_[static_cast<size_t>(slot)];
    if (!EnsureSourceVoice(v, clip.channels, clip.sampleRate, bus)) {
        return {};
    }

    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = static_cast<UINT32>(clip.ByteSize());
    // pAudioData は clips_ の実体を直接指す。**再生中にクリップを差し替えてはいけない**
    // (RegisterClip / StopVoicesUsingClip が先に停止する契約)
    buf.pAudioData = reinterpret_cast<const BYTE*>(clip.samples.data());
    buf.Flags = XAUDIO2_END_OF_STREAM;
    if (desc.loop) {
        buf.LoopCount = XAUDIO2_LOOP_INFINITE;
    }

    v.voice->SetVolume(ClampVolume(desc.volume));
    v.voice->SetFrequencyRatio(ClampPitch(desc.pitch));
    if (FAILED(v.voice->SubmitSourceBuffer(&buf)) || FAILED(v.voice->Start(0))) {
        StopSlot(v);
        return {};
    }

    v.active = true;
    v.priority = desc.priority;
    v.clip = desc.clip;
    v.startSeq = ++playSeq_;
    ++v.generation;
    if (v.generation == 0) {
        v.generation = 1; // 0 は「無効ハンドル」の予約値
    }

    AudioHandle h;
    h.index = static_cast<uint32_t>(slot);
    h.generation = v.generation;
    return h;
}

void AudioSystem::Stop(AudioHandle h)
{
    if (!h.Valid() || h.index >= voices_.size()) {
        return;
    }
    Voice& v = voices_[h.index];
    if (v.generation != h.generation || !v.active) {
        return; // すでに別の音に再利用されている
    }
    StopSlot(v);
}

void AudioSystem::SetVoiceVolume(AudioHandle h, float volume)
{
    if (!h.Valid() || h.index >= voices_.size()) {
        return;
    }
    Voice& v = voices_[h.index];
    if (v.generation != h.generation || !v.active || v.voice == nullptr) {
        return;
    }
    v.voice->SetVolume(ClampVolume(volume));
}

void AudioSystem::SetVoicePitch(AudioHandle h, float pitch)
{
    if (!h.Valid() || h.index >= voices_.size()) {
        return;
    }
    Voice& v = voices_[h.index];
    if (v.generation != h.generation || !v.active || v.voice == nullptr) {
        return;
    }
    v.voice->SetFrequencyRatio(ClampPitch(pitch));
}

void AudioSystem::StopAll()
{
    for (Voice& v : voices_) {
        if (v.active) {
            StopSlot(v);
        }
    }
}

int AudioSystem::ActiveVoiceCount() const
{
    int n = 0;
    for (const Voice& v : voices_) {
        if (v.active) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// フレーム更新
// ---------------------------------------------------------------------------

void AudioSystem::SetSuspended(bool suspended)
{
    if (suspended == suspended_) {
        return;
    }
    suspended_ = suspended;
    if (suspended_) {
        // 立ち上がりで 1 回だけ全停止する。記録/検証中に実音を出さないための境界
        StopAll();
    }
}

void AudioSystem::Update()
{
    if (xaudio_ == nullptr) {
        return;
    }
    // ★suspended でも回収は必ず回す。ここで early-out すると再生し終えた voice が
    //   active のまま残り、スロットが枯れて以後何も鳴らなくなる。
    for (Voice& v : voices_) {
        if (!v.active || v.voice == nullptr) {
            continue;
        }
        XAUDIO2_VOICE_STATE st = {};
        v.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) { // ループ再生中は 1 のまま = 回収されない
            v.active = false;
            v.clip = {};
        }
    }
}

// ---------------------------------------------------------------------------
// 互換シム (M19 の API)
// ---------------------------------------------------------------------------

bool AudioSystem::LoadWav(const std::string& key, const std::wstring& path)
{
    const AssetID id = LoadClipFile(path);
    if (id.IsNull()) {
        return false;
    }
    named_[HashStr(key)] = id;
    return true;
}

void AudioSystem::Play(const std::string& key, float volume)
{
    PlayDesc d;
    d.clip = ResolveClipKey(HashStr(key));
    d.volume = volume;
    d.bus = kBusSe;
    Play(d);
}

AssetID AudioSystem::ResolveClipKey(uint64_t key) const
{
    if (key == 0) {
        return {};
    }
    const auto named = named_.find(key);
    if (named != named_.end()) {
        return named->second;
    }
    // 名前キーで引けなければ AssetID (パス GUID) 直指定とみなす
    return clips_.find(key) != clips_.end() ? AssetID{ key } : AssetID{};
}

} // namespace mye
