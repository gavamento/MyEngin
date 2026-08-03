#include "Engine/Engine/Audio/AudioSystem.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#include <Windows.h>
#include <x3daudio.h>
#include <xaudio2.h>
#include <xaudio2fx.h>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/MusicStream.h"
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

// I3DL2 プリセット。**AudioMixer.h の kReverbPresetNames と同じ並び**であること
// (名前で .mixer.json に保存するので、並びを変えても既存ファイルは壊れないが、
//  この 2 つの表がズレると「選んだ名前と違う響き」になる)
const XAUDIO2FX_REVERB_I3DL2_PARAMETERS kReverbPresets[] = {
    XAUDIO2FX_I3DL2_PRESET_DEFAULT,    // Room=-10000 = 実質リバーブ無し
    XAUDIO2FX_I3DL2_PRESET_GENERIC,   XAUDIO2FX_I3DL2_PRESET_ROOM,
    XAUDIO2FX_I3DL2_PRESET_SMALLROOM, XAUDIO2FX_I3DL2_PRESET_MEDIUMROOM,
    XAUDIO2FX_I3DL2_PRESET_LARGEROOM, XAUDIO2FX_I3DL2_PRESET_MEDIUMHALL,
    XAUDIO2FX_I3DL2_PRESET_CAVE,      XAUDIO2FX_I3DL2_PRESET_UNDERWATER,
    XAUDIO2FX_I3DL2_PRESET_ARENA,     XAUDIO2FX_I3DL2_PRESET_PLATE,
};
static_assert(sizeof(kReverbPresets) / sizeof(kReverbPresets[0]) == kReverbPresetCount,
              "reverb preset table must match AudioMixer.h's name table");

// メーターの減衰速度 (フルスケール/秒)。アタックは即時、リリースだけこの速度で落とす
constexpr float kMeterFallPerSec = 1.6f;
constexpr float kMeterHoldFallPerSec = 0.35f;

// ---- 3D 定位 (M45e) ----
// AudioSystem.h は x3daudio.h を include できない (Windows.h を引き込んでしまう) ので
// ハンドルを生バイトで持っている。SDK 側の定義とズレたらここで落とす
static_assert(AudioSystem::kSpatialHandleBytes == X3DAUDIO_HANDLE_BYTESIZE,
              "X3DAUDIO_HANDLE size changed — update AudioSystem::kSpatialHandleBytes");
static_assert(AudioSystem::kSpatialHandleBytes % sizeof(uint32_t) == 0,
              "x3dHandle_ is stored as uint32_t[] — the byte size must divide evenly");

// ステレオエミッタのチャンネル方位 (前方向から時計回りのラジアン)。左 = 270°、右 = 90°
const float kStereoAzimuths[2] = { 3.0f * X3DAUDIO_PI / 2.0f, X3DAUDIO_PI / 2.0f };
// マルチチャンネルエミッタの「チャンネル間の広がり」(ワールド単位)
constexpr float kEmitterChannelRadius = 1.0f;

// GetChannelMask が 0 を返す環境 (仮想 / ループバックデバイス) 用のフォールバック
DWORD DefaultChannelMask(uint32_t channels)
{
    switch (channels) {
    case 1: return SPEAKER_MONO;
    case 2: return SPEAKER_STEREO;
    case 4: return SPEAKER_QUAD;
    case 6: return SPEAKER_5POINT1;
    case 8: return SPEAKER_7POINT1;
    default: return 0;
    }
}

float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

X3DAUDIO_VECTOR ToX3D(const AudioVec3& v)
{
    return X3DAUDIO_VECTOR{ v.x, v.y, v.z };
}

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

bool EqualNoCase(const std::string& a, const char* b)
{
    return b != nullptr && _stricmp(a.c_str(), b) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// 初期化 / 終了
// ---------------------------------------------------------------------------

AudioSystem::AudioSystem()
{
    // 既定バス構成はデバイスに依存しない**データ**として持つ。こうしておくと
    // --no-audio / selftest (Init を呼ばない) でも FindBus / MakePlayDesc が働く
    const MixerAsset def = DefaultMixer();
    const std::vector<int> parents = MixerBusParents(def);
    buses_.reserve(def.buses.size());
    for (size_t i = 0; i < def.buses.size(); ++i) {
        BusSlot b;
        b.s.name = def.buses[i].name;
        b.s.parent = parents[i] >= 0 ? parents[i] : -1;
        buses_.push_back(std::move(b));
    }
    UpdateRootBus();
    // ストリーマは常に構築しておく (ワーカーが起きるのは Init のとき)。
    // こうすると --no-audio / selftest でも PlayMusic が null チェック無しに no-op になる
    music_ = std::make_unique<MusicStreamer>();
}

// MusicStreamer が前方宣言なので、デストラクタは**必ずここ** (完全定義が見える場所) に置く
AudioSystem::~AudioSystem()
{
    Shutdown();
}

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
    if (masterChannels_ > static_cast<uint32_t>(kMaxBusChannels)) {
        masterChannels_ = static_cast<uint32_t>(kMaxBusChannels);
    }
    masterSampleRate_ = details.InputSampleRate != 0 ? details.InputSampleRate : 44100u;

    if (!BuildBusGraph()) {
        MYE_LOG_WARN("[audio] bus graph creation failed — audio disabled");
        Shutdown();
        return false;
    }

    voices_.resize(static_cast<size_t>(kMaxVoices));
    InitSpatial(); // 失敗しても 2D 経路で動き続ける (オーディオ全体は落とさない)
    music_->Init(xaudio_); // BGM ストリーマのワーカー起動 (M45f)
    MYE_LOG_INFO("[audio] XAudio2 ready (%u ch @ %u Hz, %d voices, %d buses)", masterChannels_,
                 masterSampleRate_, kMaxVoices, BusCount());
    return true;
}

// X3DAudio の初期化。**x3daudio.lib は Win10 SDK に存在せず、xaudio2.lib が
// X3DAudioInitialize / X3DAudioCalculate をエクスポートしている** (ファイル冒頭の注記参照)。
void AudioSystem::InitSpatial()
{
    x3dReady_ = false;
    if (master_ == nullptr) {
        return;
    }
    // XAUDIO2_DEVICE_DETAILS は 2.9 で廃止。スピーカー構成は GetChannelMask で取る
    DWORD mask = 0;
    if (FAILED(master_->GetChannelMask(&mask)) || mask == 0) {
        // 仮想 / ループバックデバイスは 0 を返すことがある → チャンネル数から推定する
        mask = DefaultChannelMask(masterChannels_);
    }
    if (mask == 0) {
        MYE_LOG_WARN("[audio] speaker mask unavailable (%u ch) — 3D disabled, 2D playback continues",
                     masterChannels_);
        return;
    }
    // 世界単位はメートルなので既定の音速 (343.5 m/s) がそのまま正しい
    if (FAILED(X3DAudioInitialize(mask, X3DAUDIO_SPEED_OF_SOUND,
                                  reinterpret_cast<BYTE*>(x3dHandle_)))) {
        MYE_LOG_WARN("[audio] X3DAudioInitialize failed — 3D disabled, 2D playback continues");
        return;
    }
    x3dReady_ = true;
    MYE_LOG_INFO("[audio] X3DAudio ready (speaker mask 0x%08lX)", static_cast<unsigned long>(mask));
}

void AudioSystem::Shutdown()
{
    // ★BGM が最初。ストリーマはワーカースレッドを持つので、**バスグラフを壊す前に
    //   スレッドを殺し切って voice を破棄する**必要がある (Shutdown が中でその順を守る)
    if (music_) {
        music_->Shutdown();
    }
    DestroyAllSourceVoices();
    voices_.clear();
    x3dReady_ = false;
    defaultMatrixValid_[0] = false;
    defaultMatrixValid_[1] = false;
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
//   source voice ─┬─[0]→ bus[b] ─→ ... → root bus ─→ mastering voice
//                 └─[1]→ reverbBus ────────┘
//   bus[b] (非ルート) ─[1]→ reverbBus            ... バス毎のリバーブ送り (M45d)
//
// ドライバスは **mastering voice と同じチャンネル幅**で作る。X3DAudioCalculate が返す
// 行列は「最終ミックスのチャンネル数」前提 (x3daudio.h:273) なので、途中で幅が変わると
// M45e のパンニングが黙って壊れる。
//
// ★ProcessingStage は「送り元 < 送り先」でなければ 1 quantum 遅れる/落ちる。
//   深さから stage = 2*(maxDepth - depth) + 1、リバーブ = 2*maxDepth を割り当てると
//   「深いバス < リバーブ < ルート」が常に成立する (深さ 0 だけの構成でも成立)。
// 送り先は CreateSourceVoice / CreateSubmixVoice 時に固定される — トポロジが変わる時だけ
// 全 voice を作り直す (音量/ミュート/ソロ/送り量は SetVolume / SetOutputMatrix で足りる)。
bool AudioSystem::BuildBusGraph()
{
    if (xaudio_ == nullptr || buses_.empty()) {
        return false;
    }
    const size_t n = buses_.size();

    // 深さ (ルートからの距離)。ApplyMixer が検証済みなので木であることは保証されている
    std::vector<int> depth(n, 0);
    int maxDepth = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = 0;
        int cur = static_cast<int>(i);
        while (buses_[static_cast<size_t>(cur)].s.parent >= 0 && d <= static_cast<int>(n)) {
            cur = buses_[static_cast<size_t>(cur)].s.parent;
            ++d;
        }
        depth[i] = d;
        maxDepth = d > maxDepth ? d : maxDepth;
    }
    const uint32_t reverbStage = static_cast<uint32_t>(2 * maxDepth);
    auto stageOf = [&depth, maxDepth](size_t i) {
        return static_cast<uint32_t>(2 * (maxDepth - depth[i]) + 1);
    };

    // volume meter (レベルメーター用)。in-place APO なので OutputChannels は入力と同じ
    auto createBus = [&](size_t i, const XAUDIO2_VOICE_SENDS* sends) {
        IUnknown* meterApo = nullptr;
        XAUDIO2_EFFECT_DESCRIPTOR fx = {};
        XAUDIO2_EFFECT_CHAIN chain = { 1, &fx };
        const bool hasMeter = SUCCEEDED(XAudio2CreateVolumeMeter(&meterApo));
        if (hasMeter) {
            fx.pEffect = meterApo;
            fx.InitialState = TRUE;
            fx.OutputChannels = masterChannels_;
        }
        const HRESULT hr =
            xaudio_->CreateSubmixVoice(&buses_[i].voice, masterChannels_, masterSampleRate_, 0,
                                       stageOf(i), sends, hasMeter ? &chain : nullptr);
        if (meterApo != nullptr) {
            meterApo->Release(); // サブミックスが AddRef 済み。ここで手放さないとリークする
        }
        if (FAILED(hr)) {
            buses_[i].voice = nullptr;
            return false;
        }
        buses_[i].meterEffect = hasMeter ? 0 : -1;
        return true;
    };

    // 1) ルートバス (mastering voice へ直結)
    const size_t root = static_cast<size_t>(rootBus_);
    if (!createBus(root, nullptr)) {
        return false;
    }

    // 2) リバーブ用サブミックス (ステレオ・レート上限クランプ)。ウェット側は X3DAudio が
    //    スカラー ReverbLevel しか返さないので、ここが 2ch でも定位は失われない
    IUnknown* reverbApo = nullptr;
    if (SUCCEEDED(XAudio2CreateReverb(&reverbApo))) {
        XAUDIO2_SEND_DESCRIPTOR toRootDesc = { 0, buses_[root].voice };
        XAUDIO2_VOICE_SENDS toRoot = { 1, &toRootDesc };
        XAUDIO2_EFFECT_DESCRIPTOR fx = {};
        fx.pEffect = reverbApo;
        fx.InitialState = TRUE;
        fx.OutputChannels = kReverbChannels;
        XAUDIO2_EFFECT_CHAIN chain = { 1, &fx };

        const uint32_t reverbRate = std::min(masterSampleRate_, kReverbMaxRate);
        const HRESULT hr = xaudio_->CreateSubmixVoice(&reverbVoice_, kReverbChannels, reverbRate, 0,
                                                      reverbStage, &toRoot, &chain);
        reverbApo->Release();
        if (FAILED(hr)) {
            MYE_LOG_WARN("[audio] reverb submix creation failed (0x%08lX) — reverb disabled",
                         static_cast<unsigned long>(hr));
            reverbVoice_ = nullptr;
        }
    } else {
        MYE_LOG_WARN("[audio] XAudio2CreateReverb failed — reverb disabled");
    }

    // 3) 残りのバスを深さの昇順で作る (親が先に存在している必要がある)。
    //    非ルートは常に [親, リバーブ] の 2 本の send を張っておく — 送り量 0 で作っておけば
    //    リバーブ送りの変更が SetOutputMatrix だけで済み、voice の作り直しが要らない
    for (int d = 1; d <= maxDepth; ++d) {
        for (size_t i = 0; i < n; ++i) {
            if (depth[i] != d) {
                continue;
            }
            IXAudio2SubmixVoice* parent = buses_[static_cast<size_t>(buses_[i].s.parent)].voice;
            if (parent == nullptr) {
                return false;
            }
            XAUDIO2_SEND_DESCRIPTOR sends[2] = { { 0, parent }, { 0, reverbVoice_ } };
            XAUDIO2_VOICE_SENDS sendList = { reverbVoice_ != nullptr ? 2u : 1u, sends };
            if (!createBus(i, &sendList)) {
                return false;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (buses_[i].voice == nullptr) {
            return false; // 孤立バス (深さが計算できなかった) — ApplyMixer の検証漏れ
        }
    }

    ApplyBusGains();
    ApplyReverbSends();
    ApplyReverbParams();
    return true;
}

void AudioSystem::DestroyBusGraph()
{
    // 送り元から先に破棄する (入力を持つ voice の DestroyVoice は未定義動作)。
    // 深いバス → リバーブ → ルート の順になるよう、深さの降順で回す
    const size_t n = buses_.size();
    std::vector<int> depth(n, 0);
    int maxDepth = 0;
    for (size_t i = 0; i < n; ++i) {
        int d = 0;
        int cur = static_cast<int>(i);
        while (buses_[static_cast<size_t>(cur)].s.parent >= 0 && d <= static_cast<int>(n)) {
            cur = buses_[static_cast<size_t>(cur)].s.parent;
            ++d;
        }
        depth[i] = d;
        maxDepth = d > maxDepth ? d : maxDepth;
    }
    for (int d = maxDepth; d >= 1; --d) {
        for (size_t i = 0; i < n; ++i) {
            if (depth[i] == d && buses_[i].voice != nullptr) {
                buses_[i].voice->DestroyVoice();
                buses_[i].voice = nullptr;
            }
        }
    }
    if (reverbVoice_ != nullptr) {
        reverbVoice_->DestroyVoice();
        reverbVoice_ = nullptr;
    }
    for (size_t i = 0; i < n; ++i) {
        if (buses_[i].voice != nullptr) { // 深さ 0 (ルート) が最後に残る
            buses_[i].voice->DestroyVoice();
            buses_[i].voice = nullptr;
        }
        buses_[i].meterEffect = -1;
        buses_[i].level = 0.0f;
        buses_[i].hold = 0.0f;
    }
}

void AudioSystem::DestroyAllSourceVoices()
{
    // ★BGM の voice も送り先サブミックスを掴んでいるので、ここで必ず一緒に破棄する。
    //   残すと再構築で消えたバスへ送り続けて落ちる。トポロジ変更 (バスの追加/削除/改名/
    //   親変更) はエディタ操作でしか起きないので、BGM が止まることは許容する
    if (music_) {
        music_->StopNow();
    }
    for (Voice& v : voices_) {
        if (v.voice != nullptr) {
            v.voice->Stop(0);
            v.voice->FlushSourceBuffers(); // クリップのバイト列を参照する buffer を先に手放す
            v.voice->DestroyVoice();
            v.voice = nullptr;
        }
        v.active = false;
        v.clip = {};
        v.channels = 0;
        v.sampleRate = 0;
    }
}

void AudioSystem::UpdateRootBus()
{
    rootBus_ = 0;
    for (size_t i = 0; i < buses_.size(); ++i) {
        if (buses_[i].s.parent < 0) {
            rootBus_ = static_cast<int>(i);
            return;
        }
    }
}

// トポロジ変更の実体。**ソースボイスの送り先が消えるので、必ず先に全 voice を捨てる**
void AudioSystem::RebuildBusGraphNow()
{
    std::vector<AudioBusState> next = std::move(pendingBuses_);
    pendingBuses_.clear();
    pendingRebuild_ = false;
    reverbPreset_ = pendingReverbPreset_;
    reverbWetDry_ = pendingReverbWetDry_;

    if (xaudio_ == nullptr) {
        // デバイス無し: データだけ差し替える (FindBus / 保存が正しく働けばよい)
        buses_.clear();
        for (AudioBusState& s : next) {
            BusSlot b;
            b.s = std::move(s);
            buses_.push_back(std::move(b));
        }
        UpdateRootBus();
        return;
    }

    DestroyAllSourceVoices();
    DestroyBusGraph();
    buses_.clear();
    for (AudioBusState& s : next) {
        BusSlot b;
        b.s = std::move(s);
        buses_.push_back(std::move(b));
    }
    UpdateRootBus();
    if (!BuildBusGraph()) {
        MYE_LOG_ERROR("[audio] bus graph rebuild failed — falling back to the default mixer");
        DestroyBusGraph();
        buses_.clear();
        const MixerAsset def = DefaultMixer();
        const std::vector<int> parents = MixerBusParents(def);
        for (size_t i = 0; i < def.buses.size(); ++i) {
            BusSlot b;
            b.s.name = def.buses[i].name;
            b.s.parent = parents[i] >= 0 ? parents[i] : -1;
            buses_.push_back(std::move(b));
        }
        UpdateRootBus();
        BuildBusGraph();
    }
    MYE_LOG_INFO("[audio] bus graph rebuilt: %d buses, root '%s', reverb %s", BusCount(),
                 BusName(rootBus_), reverbVoice_ != nullptr ? "on" : "off");
}

void AudioSystem::ApplyMixer(const MixerAsset& m)
{
    std::string err;
    if (!ValidateMixer(m, &err)) {
        MYE_LOG_WARN("[audio] mixer rejected (%s) — keeping the current bus graph", err.c_str());
        return;
    }
    const std::vector<int> parents = MixerBusParents(m);
    pendingBuses_.clear();
    pendingBuses_.reserve(m.buses.size());
    for (size_t i = 0; i < m.buses.size(); ++i) {
        AudioBusState s;
        s.name = m.buses[i].name;
        s.parent = parents[i] >= 0 ? parents[i] : -1;
        s.volumeDb = std::clamp(m.buses[i].volumeDb, kMinDb, kMaxBusDb);
        s.mute = m.buses[i].mute;
        s.solo = m.buses[i].solo;
        // ルートは reverb の出力先なので、そこから送ると閉路になる
        s.reverbSend = s.parent < 0 ? 0.0f : std::clamp(m.buses[i].reverbSend, 0.0f, 1.0f);
        pendingBuses_.push_back(std::move(s));
    }
    pendingReverbPreset_ = ReverbPresetIndex(m.reverbPreset.c_str());
    pendingReverbWetDry_ = std::clamp(m.reverbWetDryMix, 0.0f, 100.0f);
    pendingRebuild_ = true;
}

MixerAsset AudioSystem::CurrentMixer() const
{
    MixerAsset m;
    m.buses.reserve(buses_.size());
    for (const BusSlot& b : buses_) {
        MixerBus out;
        out.name = b.s.name;
        out.parent = b.s.parent >= 0 ? buses_[static_cast<size_t>(b.s.parent)].s.name : std::string();
        out.volumeDb = b.s.volumeDb;
        out.mute = b.s.mute;
        out.solo = b.s.solo;
        out.reverbSend = b.s.reverbSend;
        m.buses.push_back(std::move(out));
    }
    m.reverbPreset = ReverbPresetName(reverbPreset_);
    m.reverbWetDryMix = reverbWetDry_;
    return m;
}

// 音量 / ミュート / ソロ → 各サブミックスの SetVolume。**voice の作り直しは起きない**。
// ソロ規則そのものは AudioMixer.h の純関数を使う (selftest が見ているのと同じ 1 実装)
void AudioSystem::ApplyBusGains()
{
    const size_t n = buses_.size();
    std::vector<int> parents(n, -1);
    std::vector<uint8_t> mute(n, 0);
    std::vector<uint8_t> solo(n, 0);
    for (size_t i = 0; i < n; ++i) {
        parents[i] = buses_[i].s.parent;
        mute[i] = buses_[i].s.mute ? 1u : 0u;
        solo[i] = buses_[i].s.solo ? 1u : 0u;
    }
    const std::vector<uint8_t> silenced = SoloEffectiveMutes(parents, mute, solo);
    for (size_t i = 0; i < n; ++i) {
        const float gain = silenced[i] != 0 ? 0.0f : DbToLinear(buses_[i].s.volumeDb);
        if (buses_[i].voice != nullptr) {
            buses_[i].voice->SetVolume(gain);
        }
    }
}

// バス毎のリバーブ送り量 → 送り行列。ステレオへの落とし込みは偶数ch→L / 奇数ch→R
void AudioSystem::ApplyReverbSends()
{
    if (reverbVoice_ == nullptr) {
        return;
    }
    const uint32_t src = masterChannels_;
    float matrix[kMaxMatrix] = {};
    for (BusSlot& b : buses_) {
        if (b.voice == nullptr || b.s.parent < 0) {
            continue; // ルートは送らない (閉路になる)
        }
        for (uint32_t d = 0; d < kReverbChannels; ++d) {
            for (uint32_t s = 0; s < src; ++s) {
                const bool route = (src == 1) || ((s % kReverbChannels) == d);
                matrix[s + src * d] = route ? b.s.reverbSend : 0.0f;
            }
        }
        b.voice->SetOutputMatrix(reverbVoice_, src, kReverbChannels, matrix);
    }
}

void AudioSystem::ApplyReverbParams()
{
    if (reverbVoice_ == nullptr) {
        return;
    }
    const int idx = (reverbPreset_ >= 0 && reverbPreset_ < kReverbPresetCount) ? reverbPreset_ : 0;
    XAUDIO2FX_REVERB_I3DL2_PARAMETERS i3dl2 = kReverbPresets[idx];
    XAUDIO2FX_REVERB_PARAMETERS native = {};
    ReverbConvertI3DL2ToNative(&i3dl2, &native, FALSE); // FALSE = 5.1/7.1 用でなくステレオ
    native.WetDryMix = std::clamp(reverbWetDry_, 0.0f, 100.0f);
    reverbVoice_->SetEffectParameters(0, &native, sizeof(native));
}

// ---------------------------------------------------------------------------
// バスの参照 / 操作
// ---------------------------------------------------------------------------

const char* AudioSystem::BusName(int bus) const
{
    return ValidBus(bus) ? buses_[static_cast<size_t>(bus)].s.name.c_str() : "";
}

int AudioSystem::BusParent(int bus) const
{
    return ValidBus(bus) ? buses_[static_cast<size_t>(bus)].s.parent : -1;
}

int AudioSystem::FindBus(const char* name) const
{
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < buses_.size(); ++i) {
        if (EqualNoCase(buses_[i].s.name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int AudioSystem::DefaultBus() const
{
    const int se = FindBus("SE");
    return se >= 0 ? se : rootBus_;
}

float AudioSystem::BusVolumeDb(int bus) const
{
    return ValidBus(bus) ? buses_[static_cast<size_t>(bus)].s.volumeDb : kMinDb;
}

void AudioSystem::SetBusVolumeDb(int bus, float db)
{
    if (!ValidBus(bus)) {
        return;
    }
    buses_[static_cast<size_t>(bus)].s.volumeDb = std::clamp(db, kMinDb, kMaxBusDb);
    ApplyBusGains();
}

void AudioSystem::SetBusVolume(int bus, float linear)
{
    SetBusVolumeDb(bus, LinearToDb(linear));
}

float AudioSystem::BusVolume(int bus) const
{
    return DbToLinear(BusVolumeDb(bus));
}

bool AudioSystem::BusMute(int bus) const
{
    return ValidBus(bus) && buses_[static_cast<size_t>(bus)].s.mute;
}

void AudioSystem::SetBusMute(int bus, bool mute)
{
    if (!ValidBus(bus)) {
        return;
    }
    buses_[static_cast<size_t>(bus)].s.mute = mute;
    ApplyBusGains();
}

bool AudioSystem::BusSolo(int bus) const
{
    return ValidBus(bus) && buses_[static_cast<size_t>(bus)].s.solo;
}

void AudioSystem::SetBusSolo(int bus, bool solo)
{
    if (!ValidBus(bus)) {
        return;
    }
    buses_[static_cast<size_t>(bus)].s.solo = solo;
    ApplyBusGains();
}

float AudioSystem::BusReverbSend(int bus) const
{
    return ValidBus(bus) ? buses_[static_cast<size_t>(bus)].s.reverbSend : 0.0f;
}

void AudioSystem::SetBusReverbSend(int bus, float send)
{
    if (!ValidBus(bus) || buses_[static_cast<size_t>(bus)].s.parent < 0) {
        return; // ルートは reverb の出力先なので送れない
    }
    buses_[static_cast<size_t>(bus)].s.reverbSend = std::clamp(send, 0.0f, 1.0f);
    ApplyReverbSends();
}

void AudioSystem::SetReverbPreset(int index)
{
    reverbPreset_ = (index >= 0 && index < kReverbPresetCount) ? index : 0;
    ApplyReverbParams();
}

void AudioSystem::SetReverbWetDryMix(float percent)
{
    reverbWetDry_ = std::clamp(percent, 0.0f, 100.0f);
    ApplyReverbParams();
}

void AudioSystem::PollBusMeters(float dt)
{
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    const size_t n = buses_.size();
    // このバスへ流れ込む active voice が居るか (子から親へ伝播)。
    // 入力の無いサブミックスは APO が回らず GetEffectParameters が**前の値を返し続ける**ため、
    // これが無いとメーターが振り切ったまま固まる
    std::vector<uint8_t> fed(n, 0);
    for (const Voice& v : voices_) {
        if (!v.active || !ValidBus(v.bus)) {
            continue;
        }
        int cur = v.bus;
        for (size_t step = 0; cur >= 0 && step <= n; ++step) {
            fed[static_cast<size_t>(cur)] = 1;
            cur = buses_[static_cast<size_t>(cur)].s.parent;
        }
    }

    float peaks[kMaxBusChannels] = {};
    for (size_t i = 0; i < n; ++i) {
        BusSlot& b = buses_[i];
        float peak = 0.0f;
        if (fed[i] != 0 && b.voice != nullptr && b.meterEffect >= 0) {
            XAUDIO2FX_VOLUMEMETER_LEVELS levels = {};
            levels.pPeakLevels = peaks;
            levels.pRMSLevels = nullptr;
            levels.ChannelCount = masterChannels_;
            if (SUCCEEDED(b.voice->GetEffectParameters(static_cast<UINT32>(b.meterEffect), &levels,
                                                       sizeof(levels)))) {
                for (uint32_t c = 0; c < masterChannels_; ++c) {
                    const float a = peaks[c] < 0.0f ? -peaks[c] : peaks[c];
                    peak = a > peak ? a : peak;
                }
            }
        }
        peak = peak > 4.0f ? 4.0f : peak; // +12 dB で頭打ち (異常値でバーが暴れないように)
        // アタックは即時・リリースだけ実時間で落とす (でないと 6500fps でチラつく)
        b.level = peak > b.level ? peak : std::max(peak, b.level - kMeterFallPerSec * dt);
        b.hold = peak > b.hold ? peak : std::max(b.level, b.hold - kMeterHoldFallPerSec * dt);
    }
}

float AudioSystem::BusLevel(int bus) const
{
    return ValidBus(bus) ? buses_[static_cast<size_t>(bus)].level : 0.0f;
}

float AudioSystem::BusPeakHold(int bus) const
{
    return ValidBus(bus) ? buses_[static_cast<size_t>(bus)].hold : 0.0f;
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
        { XAUDIO2_SEND_USEFILTER, buses_[static_cast<size_t>(bus)].voice },
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

    // ★XAudio2 が張った既定行列を **書き換える前に** 読み出して控える (M45e)。
    //   spatialBlend の 2D 側と ResetVoiceTo2D はこれを使う — 自前で書き起こすと
    //   5.1/7.1 環境で従来と違う鳴り方になるため、推測せず実物を控えるのが要点。
    //   ここを逃すと以降は 3D 行列で上書き済みの値しか読めない
    if (channels >= 1 && channels <= 2) {
        const size_t di = static_cast<size_t>(channels) - 1;
        if (!defaultMatrixValid_[di]) {
            sv->GetOutputMatrix(buses_[static_cast<size_t>(bus)].voice, channels, masterChannels_,
                                defaultMatrix_[di]);
            defaultMatrixValid_[di] = true;
        }
    }

    // ★リバーブ送りは既定行列だと「素通し」になる。X3DAudio の ReverbLevel (M45e) か
    //   バス毎の送り (M45d、サブミックス側の別経路) で明示的に開けるまでは全部ドライに
    //   したいので、生成直後にゼロで潰しておく。
    if (reverbVoice_ != nullptr) {
        v.voice->SetOutputMatrix(reverbVoice_, channels, kReverbChannels, kZeroMatrix);
    }
    return true;
}

const float* AudioSystem::DefaultMatrixFor(Voice& v)
{
    if (v.channels < 1 || v.channels > 2) {
        return nullptr; // 3D に載せないチャンネル構成 (既定行列を触っていないので復元も不要)
    }
    const size_t di = static_cast<size_t>(v.channels) - 1;
    return defaultMatrixValid_[di] ? defaultMatrix_[di] : nullptr;
}

// スロットを再利用するときに前の音の定位を必ず消す。
// **これを忘れると、3D で使ったスロットに 2D の音が乗った瞬間に減衰と LPF を引き継ぐ**
// (voice はフォーマットとバスが一致する限り作り直されないため)。
void AudioSystem::ResetVoiceTo2D(Voice& v)
{
    if (v.voice == nullptr || !ValidBus(v.bus)) {
        return;
    }
    IXAudio2SubmixVoice* dry = buses_[static_cast<size_t>(v.bus)].voice;
    if (const float* def = DefaultMatrixFor(v)) {
        v.voice->SetOutputMatrix(dry, v.channels, masterChannels_, def);
    }
    if (reverbVoice_ != nullptr) {
        v.voice->SetOutputMatrix(reverbVoice_, v.channels, kReverbChannels, kZeroMatrix);
    }
    // フィルタも開け切る (前の音の LPF が残ると籠もったまま鳴る)
    XAUDIO2_FILTER_PARAMETERS pass = { LowPassFilter, XAUDIO2_MAX_FILTER_FREQUENCY, 1.0f };
    v.voice->SetOutputFilterParameters(dry, &pass);
    if (reverbVoice_ != nullptr) {
        v.voice->SetOutputFilterParameters(reverbVoice_, &pass);
    }
}

// X3DAudio で 1 voice ぶんの定位を計算して反映する。
// spatialBlend は「XAudio2 の既定行列 (2D)」と「X3DAudio の行列 (3D)」の線形補間で表現し、
// ドップラー / LPF / リバーブ送りも同じ係数で恒等値側へ寄せる — blend=0 が
// **従来の 2D 再生と完全に同一**になることが重要 (spatialBlend 既定 0 の .sound.json が
// M45e の導入で鳴り方を変えてはいけない)。
void AudioSystem::ApplySpatialToVoice(Voice& v, const AudioSpatial& s)
{
    if (v.voice == nullptr || !ValidBus(v.bus)) {
        return;
    }
    const uint32_t srcCh = v.channels;
    // マルチチャンネルエミッタは pChannelAzimuths が要る。デコーダが返すのは 1ch / 2ch だけ
    // (stb_vorbis は STB_VORBIS_MAX_CHANNELS 2) なので、それ以外は 3D を諦めて素通しにする
    // — 定位より「音が消えないこと」を優先する
    if (!x3dReady_ || srcCh < 1 || srcCh > 2) {
        ResetVoiceTo2D(v);
        return;
    }
    const uint32_t dstCh = masterChannels_;
    const float blend = s.spatialBlend < 0.0f ? 0.0f : (s.spatialBlend > 1.0f ? 1.0f : s.spatialBlend);

    X3DAUDIO_LISTENER listener = {};
    listener.Position = ToX3D(listener_.position);
    listener.Velocity = ToX3D(listener_.velocity);
    listener.OrientFront = ToX3D(listener_.forward);
    listener.OrientTop = ToX3D(listener_.up);
    listener.pCone = nullptr;

    // 距離減衰カーブ (正規化 0..1)。CurveDistanceScaler = maxDistance なので、
    // 正規化距離 1.0 がちょうど maxDistance に対応する
    AudioCurvePoint pts[kMaxRolloffCurvePoints];
    const int pointCount =
        BuildRolloffCurve(s.rolloff, s.minDistance, s.maxDistance, pts, kMaxRolloffCurvePoints);
    X3DAUDIO_DISTANCE_CURVE_POINT curvePoints[kMaxRolloffCurvePoints];
    for (int i = 0; i < pointCount; ++i) {
        curvePoints[i].Distance = pts[i].distance;
        curvePoints[i].DSPSetting = pts[i].dsp;
    }
    X3DAUDIO_DISTANCE_CURVE volumeCurve = { curvePoints, static_cast<UINT32>(pointCount) };

    X3DAUDIO_EMITTER emitter = {};
    emitter.pCone = nullptr;
    emitter.OrientFront = X3DAUDIO_VECTOR{ 0.0f, 0.0f, 1.0f }; // 左手系 +Z が前
    emitter.OrientTop = X3DAUDIO_VECTOR{ 0.0f, 1.0f, 0.0f };
    emitter.Position = ToX3D(s.position);
    emitter.Velocity = ToX3D(s.velocity);
    emitter.InnerRadius = 0.0f;
    emitter.InnerRadiusAngle = 0.0f;
    emitter.ChannelCount = srcCh;
    emitter.ChannelRadius = kEmitterChannelRadius;
    // 単一チャンネルでは参照されないが、null を渡さない方が診断時に紛れが無い
    emitter.pChannelAzimuths = const_cast<float*>(kStereoAzimuths);
    emitter.pVolumeCurve = pointCount >= 2 ? &volumeCurve : nullptr;
    emitter.pLFECurve = nullptr;       // 既定 (逆二乗) のまま
    emitter.pLPFDirectCurve = nullptr; // 既定 [0,1]→[1,0.75]
    emitter.pLPFReverbCurve = nullptr;
    emitter.pReverbCurve = nullptr;
    emitter.CurveDistanceScaler = s.maxDistance > 1e-4f ? s.maxDistance : 1e-4f;
    emitter.DopplerScaler = s.dopplerScale > 0.0f ? s.dopplerScale : 0.0f;

    X3DAUDIO_DSP_SETTINGS dsp = {};
    dsp.pMatrixCoefficients = matrixScratch_;
    dsp.pDelayTimes = delayScratch_;
    dsp.SrcChannelCount = srcCh;
    dsp.DstChannelCount = dstCh;

    UINT32 flags = X3DAUDIO_CALCULATE_MATRIX | X3DAUDIO_CALCULATE_DOPPLER
                 | X3DAUDIO_CALCULATE_LPF_DIRECT;
    if (reverbVoice_ != nullptr) {
        flags |= X3DAUDIO_CALCULATE_REVERB | X3DAUDIO_CALCULATE_LPF_REVERB;
    }
    X3DAudioCalculate(reinterpret_cast<const BYTE*>(x3dHandle_), &listener, &emitter, flags, &dsp);

    IXAudio2SubmixVoice* dry = buses_[static_cast<size_t>(v.bus)].voice;

    // ---- ドライ行列 (X3DAudio と XAudio2 は同じ [srcCount * D + S] レイアウト) ----
    const float* def2D = DefaultMatrixFor(v);
    if (def2D == nullptr) {
        def2D = matrixScratch_; // 既定行列が取れていなければ補間を恒等にする (= 純 3D)
    }
    const size_t count = static_cast<size_t>(srcCh) * dstCh;
    for (size_t i = 0; i < count; ++i) {
        blendScratch_[i] = Lerp(def2D[i], matrixScratch_[i], blend);
    }
    v.voice->SetOutputMatrix(dry, srcCh, dstCh, blendScratch_);

    // ---- リバーブ送り ----
    // X3DAudio がウェット側に返すのはスカラー 1 個 (リバーブ APO が mono/stereo 入力に
    // 限られるため)。mono は両 ch へ、stereo は同 index へ 1:1 で送る — 全要素へ複製すると
    // ステレオ音源だけリバーブが 2 倍のエネルギーで入ってしまう
    if (reverbVoice_ != nullptr) {
        const float level = s.reverbSend * Lerp(1.0f, dsp.ReverbLevel, blend);
        float wet[2 * kReverbChannels] = {};
        for (uint32_t d = 0; d < kReverbChannels; ++d) {
            for (uint32_t sc = 0; sc < srcCh; ++sc) {
                const bool feed = (srcCh == 1) || (sc == d);
                wet[srcCh * d + sc] = feed ? level : 0.0f;
            }
        }
        v.voice->SetOutputMatrix(reverbVoice_, srcCh, kReverbChannels, wet);
    }

    // ---- LPF (経路別。voice 全体の SetFilterParameters では per-send を表現できない) ----
    // 係数 1.0 = 素通し。2*sin(pi/6 * coeff) が [0,1] → [0,1] の正規化周波数に対応する
    auto applyLpf = [&](IXAudio2Voice* dst, float coefficient) {
        const float c = Lerp(1.0f, coefficient, blend);
        float freq = 2.0f * std::sin(X3DAUDIO_PI / 6.0f * c);
        if (!(freq > 0.0f)) { // NaN もここで閉じ切る側に落ちる
            freq = 0.0f;
        } else if (freq > XAUDIO2_MAX_FILTER_FREQUENCY) {
            freq = XAUDIO2_MAX_FILTER_FREQUENCY; // sin の丸めで 1.0 を僅かに超えることがある
        }
        XAUDIO2_FILTER_PARAMETERS p = { LowPassFilter, freq, 1.0f };
        v.voice->SetOutputFilterParameters(dst, &p);
    };
    applyLpf(dry, dsp.LPFDirectCoefficient);
    if (reverbVoice_ != nullptr) {
        applyLpf(reverbVoice_, dsp.LPFReverbCoefficient);
    }

    // ---- ドップラー ----
    v.voice->SetFrequencyRatio(ClampPitch(s.pitch * Lerp(1.0f, dsp.DopplerFactor, blend)));
}

bool AudioSystem::ApplyVoiceSpatial(AudioHandle h, const AudioSpatial& s)
{
    if (!h.Valid() || h.index >= voices_.size()) {
        return false;
    }
    Voice& v = voices_[h.index];
    if (v.generation != h.generation || !v.active || v.voice == nullptr) {
        return false; // すでに別の音に再利用されている / 鳴り終わっている
    }
    ApplySpatialToVoice(v, s);
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
    const int bus = ValidBus(desc.bus) ? desc.bus : DefaultBus();
    if (buses_[static_cast<size_t>(bus)].voice == nullptr) {
        return {};
    }

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
    // ★定位は Start() の **前** に確定させる (再生開始の 1 quantum が無定位で鳴るのを防ぐ)。
    //   2D の音でも必ず ResetVoiceTo2D を通す — スロットは使い回されるので、前に 3D で
    //   鳴らした音の減衰行列と LPF がそのまま残っているため。SetFrequencyRatio より後に
    //   置くのは、3D 側がドップラーを載せた比で上書きするから
    if (desc.spatial != nullptr) {
        ApplySpatialToVoice(v, *desc.spatial);
    } else {
        ResetVoiceTo2D(v);
    }
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

// ---------------------------------------------------------------------------
// BGM ストリーミング (M45f)。ボイスプールとは別レーン
// ---------------------------------------------------------------------------

bool AudioSystem::PlayMusic(const MusicDesc& d)
{
    if (xaudio_ == nullptr || suspended_ || !music_) {
        return false; // 記録/検証中と --no-audio では鳴らさない (決定論契約 2)
    }
    const int bus = ValidBus(d.bus) ? d.bus : DefaultBus();
    IXAudio2SubmixVoice* dest = buses_[static_cast<size_t>(bus)].voice;
    if (dest == nullptr) {
        return false;
    }
    MusicRequest r;
    r.path = d.path;
    r.key = d.key;
    r.volume = d.volume;
    r.fadeSeconds = d.fadeSeconds;
    r.loop = d.loop;
    r.loopStartFrame = d.loopStartFrame;
    r.loopEndFrame = d.loopEndFrame;
    return music_->Play(r, dest);
}

void AudioSystem::StopMusic(float fadeSeconds)
{
    if (music_) {
        music_->Stop(fadeSeconds);
    }
}

bool AudioSystem::MusicPlaying() const
{
    return music_ && music_->Playing();
}

uint64_t AudioSystem::MusicKey() const
{
    return music_ ? music_->CurrentKey() : 0;
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
        // 立ち上がりで 1 回だけ全停止する。記録/検証中に実音を出さないための境界。
        // ★BGM は StopAll では止まらない別レーンなので明示的に止める (M45f)
        StopAll();
        if (music_) {
            music_->StopNow();
        }
    }
}

void AudioSystem::Update(float dt)
{
    // ★トポロジ変更はここ (フレーム境界) でしか適用しない。UI のドラッグ操作で
    //   1 フレームに何度 ApplyMixer が呼ばれても、グラフの作り直しは 1 回で済む
    if (pendingRebuild_) {
        RebuildBusGraphNow();
    }
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
    // BGM のフェード進行と鳴り終わり回収 (M45f)。**suspend 中も回す** — 上の voice 回収と
    // 同じ理由で、止めた側の後始末までは動かし続ける必要がある
    if (music_) {
        music_->Update(dt);
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
    d.bus = DefaultBus();
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
