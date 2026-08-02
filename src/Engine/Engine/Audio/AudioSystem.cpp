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
#include "Engine/Engine/Audio/AudioMixer.h"
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
    MYE_LOG_INFO("[audio] XAudio2 ready (%u ch @ %u Hz, %d voices, %d buses)", masterChannels_,
                 masterSampleRate_, kMaxVoices, BusCount());
    return true;
}

void AudioSystem::Shutdown()
{
    DestroyAllSourceVoices();
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

// 音量 / ミュート / ソロ → 各サブミックスの SetVolume。**voice の作り直しは起きない**
void AudioSystem::ApplyBusGains()
{
    bool anySolo = false;
    for (const BusSlot& b : buses_) {
        anySolo = anySolo || b.s.solo;
    }
    const size_t n = buses_.size();
    for (size_t i = 0; i < n; ++i) {
        bool audible = true;
        if (anySolo) {
            // 「ソロバス自身 + その祖先 + その子孫」だけを残す
            audible = false;
            int cur = static_cast<int>(i);
            for (size_t step = 0; step <= n; ++step) { // 自分 → 根: 途中にソロがあれば子孫
                if (buses_[static_cast<size_t>(cur)].s.solo) {
                    audible = true;
                    break;
                }
                if (buses_[static_cast<size_t>(cur)].s.parent < 0) {
                    break;
                }
                cur = buses_[static_cast<size_t>(cur)].s.parent;
            }
            if (!audible) {
                // 自分がソロの祖先か (子孫のどれかがソロなら鳴らす経路が要る)
                for (size_t j = 0; j < n && !audible; ++j) {
                    if (!buses_[j].s.solo) {
                        continue;
                    }
                    int p = buses_[j].s.parent;
                    for (size_t step = 0; p >= 0 && step <= n; ++step) {
                        if (p == static_cast<int>(i)) {
                            audible = true;
                            break;
                        }
                        p = buses_[static_cast<size_t>(p)].s.parent;
                    }
                }
            }
        }
        const float gain = (buses_[i].s.mute || !audible) ? 0.0f : DbToLinear(buses_[i].s.volumeDb);
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

    // ★リバーブ送りは既定行列だと「素通し」になる。M45e が X3DAudio の ReverbLevel を
    //   書き込むまでは全部ドライで鳴らしたいので、生成直後にゼロで潰しておく
    //   (バス毎のリバーブ送りは M45d でサブミックス側に入っている)。
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
