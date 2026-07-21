#include "Engine/Engine/Audio/AudioSystem.h"

#include <cstring>

#include <Windows.h>
#include <xaudio2.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

// XAudio2 2.9 は Win10+ に標準搭載 (再頒布 DLL 不要)。Engine.lib 経由で exe に伝播
#pragma comment(lib, "xaudio2.lib")

namespace mye {
namespace {

// 4 バイトの chunk id を uint32 (リトルエンディアン) に
constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

uint32_t ReadU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

} // namespace

bool AudioSystem::Init()
{
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
    MYE_LOG_INFO("[audio] XAudio2 ready");
    return true;
}

void AudioSystem::Shutdown()
{
    for (IXAudio2SourceVoice* v : active_) {
        v->Stop(0);
        v->DestroyVoice();
    }
    active_.clear();
    sounds_.clear();
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (xaudio_) {
        xaudio_->Release();
        xaudio_ = nullptr;
    }
    if (comInit_) {
        CoUninitialize();
        comInit_ = false;
    }
}

bool AudioSystem::LoadWav(const std::string& key, const std::wstring& path)
{
    if (sounds_.find(key) != sounds_.end()) {
        return true; // 冪等
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        MYE_LOG_WARN("[audio] wav open failed: %s", WideToUtf8(path).c_str());
        return false;
    }
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read < 44) {
        MYE_LOG_WARN("[audio] wav read failed/too small: %s", WideToUtf8(path).c_str());
        return false;
    }

    // RIFF/WAVE を検証して fmt / data chunk を探す
    if (ReadU32(&bytes[0]) != FourCC('R', 'I', 'F', 'F') ||
        ReadU32(&bytes[8]) != FourCC('W', 'A', 'V', 'E')) {
        MYE_LOG_WARN("[audio] not a RIFF/WAVE file: %s", WideToUtf8(path).c_str());
        return false;
    }
    Sound snd;
    bool haveFmt = false, haveData = false;
    size_t off = 12;
    while (off + 8 <= bytes.size()) {
        const uint32_t id = ReadU32(&bytes[off]);
        const uint32_t csz = ReadU32(&bytes[off + 4]);
        const size_t body = off + 8;
        if (id == FourCC('f', 'm', 't', ' ') && body + 16 <= bytes.size()) {
            snd.formatTag = ReadU16(&bytes[body]);
            snd.channels = ReadU16(&bytes[body + 2]);
            snd.sampleRate = ReadU32(&bytes[body + 4]);
            snd.byteRate = ReadU32(&bytes[body + 8]);
            snd.blockAlign = ReadU16(&bytes[body + 12]);
            snd.bitsPerSample = ReadU16(&bytes[body + 14]);
            haveFmt = true;
        } else if (id == FourCC('d', 'a', 't', 'a')) {
            const size_t n = (body + csz <= bytes.size()) ? csz : (bytes.size() - body);
            snd.data.assign(bytes.begin() + static_cast<long long>(body),
                            bytes.begin() + static_cast<long long>(body + n));
            haveData = true;
        }
        off = body + csz + (csz & 1); // chunk は word 境界
    }
    if (!haveFmt || !haveData || snd.formatTag != WAVE_FORMAT_PCM) {
        MYE_LOG_WARN("[audio] unsupported wav (need PCM fmt+data): %s", WideToUtf8(path).c_str());
        return false;
    }
    sounds_[key] = std::move(snd);
    MYE_LOG_INFO("[audio] loaded wav: %s", key.c_str());
    return true;
}

void AudioSystem::Play(const std::string& key, float volume)
{
    if (!xaudio_) {
        return;
    }
    auto it = sounds_.find(key);
    if (it == sounds_.end()) {
        return;
    }
    const Sound& s = it->second;

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = s.channels;
    wfx.nSamplesPerSec = s.sampleRate;
    wfx.wBitsPerSample = s.bitsPerSample;
    wfx.nBlockAlign = s.blockAlign ? s.blockAlign
                                   : static_cast<uint16_t>(s.channels * s.bitsPerSample / 8);
    wfx.nAvgBytesPerSec = s.byteRate ? s.byteRate : wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2SourceVoice* voice = nullptr;
    if (FAILED(xaudio_->CreateSourceVoice(&voice, &wfx))) {
        return;
    }
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = static_cast<UINT32>(s.data.size());
    buf.pAudioData = s.data.data();
    buf.Flags = XAUDIO2_END_OF_STREAM;
    voice->SetVolume(volume < 0.0f ? 0.0f : volume);
    if (FAILED(voice->SubmitSourceBuffer(&buf)) || FAILED(voice->Start(0))) {
        voice->DestroyVoice();
        return;
    }
    active_.push_back(voice);
}

void AudioSystem::Update()
{
    if (!xaudio_) {
        return;
    }
    // 再生し終えた (バッファが残っていない) voice を破棄
    for (size_t i = 0; i < active_.size();) {
        XAUDIO2_VOICE_STATE st = {};
        active_[i]->GetState(&st, 0);
        if (st.BuffersQueued == 0) {
            active_[i]->DestroyVoice();
            active_[i] = active_.back();
            active_.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace mye
