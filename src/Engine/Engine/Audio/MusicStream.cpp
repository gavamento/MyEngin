#include "Engine/Engine/Audio/MusicStream.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <Windows.h>
#include <xaudio2.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/StbVorbis.h"
#include "Engine/Platform/PathUtil.h"

// xaudio2.lib は AudioSystem.cpp の #pragma comment で既にリンクされている
// (このサブシステム全体で 1 本。x3daudio.lib は Win10 SDK に存在しないので足さないこと)。

namespace mye {
namespace {

// ヘッダ走査のために先読みするバイト数。**data チャンクのヘッダ**さえ入っていればよいので、
// LIST/INFO 付きの wav でも 64KB あれば実用上十分 (本体は読まない = 数百 MB でも即座に開く)
constexpr size_t kWavHeaderProbeBytes = 64 * 1024;

// stb_vorbis に渡すアリーナ。**非 NULL を渡した時だけ alloca を通らなくなる**
// (stb_vorbis.c:930 の temp_alloc は alloc_buffer が無いと alloca へ落ちる)。
// ワーカースレッドの既定スタックは 1MB しかないので、ここを NULL にすると
// 素性の分からない ogg でスタックオーバーフローし得る
constexpr int kVorbisArenaBytes = 1024 * 1024;

} // namespace

// ---------------------------------------------------------------------------
// ストリーミング元 (宣言は MusicStream.h。selftest から直接叩けるようにしてある)
// ---------------------------------------------------------------------------

bool MusicSource::Open(const std::wstring& path)
{
    Close();
    std::error_code ec;
    const std::filesystem::path fsPath{ path };
    const auto fileSize = std::filesystem::file_size(fsPath, ec);
    if (ec || fileSize == 0) {
        return false;
    }

    std::ifstream f(fsPath, std::ios::binary);
    if (!f) {
        return false;
    }
    const size_t probeLen = static_cast<size_t>(
        fileSize < kWavHeaderProbeBytes ? fileSize : kWavHeaderProbeBytes);
    std::vector<uint8_t> probe(probeLen);
    f.read(reinterpret_cast<char*>(probe.data()), static_cast<std::streamsize>(probeLen));
    if (static_cast<size_t>(f.gcount()) != probeLen) {
        return false;
    }

    if (ParseWavHeader(probe.data(), probe.size(), info_)) {
        // wav: ファイルは開いたままにして、必要なフレームだけ seek + read する
        const uint32_t bpf = info_.BytesPerFrame();
        if (bpf == 0 || info_.dataOffset >= static_cast<size_t>(fileSize)) {
            return false;
        }
        const uint64_t avail = static_cast<uint64_t>(fileSize) - info_.dataOffset;
        // data チャンクが名乗る長さは信用せず必ずファイル長でクランプする
        // (0xFFFFFFFF を書くストリーム wav が実在する)
        const uint64_t bytes = (info_.dataBytesDeclared > 0 && info_.dataBytesDeclared <= avail)
                                   ? info_.dataBytesDeclared
                                   : avail;
        totalFrames_ = static_cast<int64_t>(bytes / bpf);
        if (totalFrames_ <= 0) {
            return false;
        }
        channels_ = info_.channels;
        sampleRate_ = info_.sampleRate;
        file_ = std::move(f);
        pos_ = 0;
        return true;
    }
    f.close();

    // ogg: 圧縮バイト列を丸ごとメモリへ (展開はしない)
    if (fileSize > static_cast<uintmax_t>(INT32_MAX)) {
        return false;
    }
    std::ifstream fo(fsPath, std::ios::binary);
    if (!fo) {
        return false;
    }
    oggBytes_.resize(static_cast<size_t>(fileSize));
    fo.read(reinterpret_cast<char*>(oggBytes_.data()),
            static_cast<std::streamsize>(oggBytes_.size()));
    if (static_cast<size_t>(fo.gcount()) != oggBytes_.size()) {
        Close();
        return false;
    }

    arena_.assign(static_cast<size_t>(kVorbisArenaBytes), 0);
    stb_vorbis_alloc alloc = {};
    alloc.alloc_buffer = arena_.data();
    alloc.alloc_buffer_length_in_bytes = kVorbisArenaBytes;
    int err = 0;
    vorbis_ = stb_vorbis_open_memory(oggBytes_.data(), static_cast<int>(oggBytes_.size()), &err,
                                     &alloc);
    if (vorbis_ == nullptr) {
        // アリーナ不足だけは救う (setup ぶんも同じアリーナから取るため、
        // チャンネル数やブロックサイズによっては 1MB を超える)。
        // その場合 alloca 経路へ戻るので、ワーカーのスタック消費は増える
        arena_.clear();
        arena_.shrink_to_fit();
        vorbis_ = stb_vorbis_open_memory(oggBytes_.data(), static_cast<int>(oggBytes_.size()), &err,
                                         nullptr);
    }
    if (vorbis_ == nullptr) {
        Close();
        return false;
    }
    const stb_vorbis_info vi = stb_vorbis_get_info(vorbis_);
    channels_ = static_cast<uint16_t>(vi.channels);
    sampleRate_ = vi.sample_rate;
    totalFrames_ = static_cast<int64_t>(stb_vorbis_stream_length_in_samples(vorbis_));
    if (channels_ == 0 || sampleRate_ == 0 || totalFrames_ <= 0) {
        Close();
        return false;
    }
    pos_ = 0;
    return true;
}

void MusicSource::Close()
{
    if (vorbis_ != nullptr) {
        stb_vorbis_close(vorbis_);
        vorbis_ = nullptr;
    }
    if (file_.is_open()) {
        file_.close();
    }
    file_.clear();
    oggBytes_.clear();
    oggBytes_.shrink_to_fit();
    arena_.clear();
    arena_.shrink_to_fit();
    scratch_.clear();
    scratch_.shrink_to_fit();
    info_ = WavInfo{};
    channels_ = 0;
    sampleRate_ = 0;
    totalFrames_ = 0;
    pos_ = 0;
}

bool MusicSource::Seek(int64_t frame)
{
    if (frame < 0 || frame > totalFrames_) {
        return false;
    }
    if (vorbis_ != nullptr) {
        if (stb_vorbis_seek(vorbis_, static_cast<unsigned int>(frame)) == 0) {
            return false;
        }
    }
    pos_ = frame; // wav は Read の中で毎回 seekg するので位置を覚えるだけでよい
    return true;
}

int64_t MusicSource::Read(int16_t* dst, int64_t frames)
{
    if (dst == nullptr || frames <= 0 || channels_ == 0) {
        return 0;
    }
    const int64_t avail = totalFrames_ - pos_;
    if (avail <= 0) {
        return 0;
    }
    if (frames > avail) {
        frames = avail;
    }
    return vorbis_ != nullptr ? ReadOgg(dst, frames) : ReadWav(dst, frames);
}

int64_t MusicSource::ReadWav(int16_t* dst, int64_t frames)
{
    const uint32_t bpf = info_.BytesPerFrame();
    const size_t need = static_cast<size_t>(frames) * bpf;
    if (scratch_.size() < need) {
        scratch_.resize(need);
    }
    // 常に絶対位置で seek する — 短い読み取りやエラーでストリーム位置がずれても
    // 次のブロックが必ず正しい場所から読める
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(info_.dataOffset +
                                            static_cast<size_t>(pos_) * bpf),
                std::ios::beg);
    file_.read(reinterpret_cast<char*>(scratch_.data()), static_cast<std::streamsize>(need));
    const std::streamsize got = file_.gcount();
    if (got <= 0) {
        return 0;
    }
    const int64_t gotFrames = static_cast<int64_t>(static_cast<size_t>(got) / bpf);
    if (gotFrames <= 0) {
        return 0;
    }
    // ★変換は DecodeWav と同じ 1 本を通す (Audio/AudioClip.h)。ここに独自の
    //   ビット深度分岐を書くと「全展開では鳴るのにストリーミングでは無音」が起きる
    ConvertWavSamples(scratch_.data(), static_cast<size_t>(gotFrames) * channels_, info_.formatTag,
                      info_.bits, dst);
    pos_ += gotFrames;
    return gotFrames;
}

int64_t MusicSource::ReadOgg(int16_t* dst, int64_t frames)
{
    const int n = stb_vorbis_get_samples_short_interleaved(
        vorbis_, channels_, dst, static_cast<int>(frames * channels_));
    if (n <= 0) {
        return 0;
    }
    pos_ += n;
    return n;
}

// ---------------------------------------------------------------------------
// ボイスコールバック
// ---------------------------------------------------------------------------

namespace {

// ★ここはすべて **XAudio2 のオーディオスレッド**で走る。やってよいのは SetEvent だけで、
//   SubmitSourceBuffer / Stop / GetState / DestroyVoice を呼ぶと自己デッドロックする。
//   mu_ も取らない — 取ると「メインが mu_ を持ったまま DestroyVoice」で相互待ちになる。
class MusicVoiceCallback : public IXAudio2VoiceCallback {
public:
    HANDLE bufferEnd = nullptr;

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override
    {
        if (bufferEnd != nullptr) {
            SetEvent(bufferEnd);
        }
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

} // namespace

// ---------------------------------------------------------------------------
// スロット
// ---------------------------------------------------------------------------

// 1 スロット = 1 曲。**アドレスが動いてはいけない** (オーディオスレッドが cb と ring を
// 直接見ているため) ので MusicStreamer は unique_ptr の固定配列で持つ。
struct MusicStreamer::Slot {
    IXAudio2SourceVoice* voice = nullptr;
    MusicVoiceCallback cb; // ★voice より長生きすること (DestroyVoice まで参照される)
    std::vector<int16_t> ring; // kMusicRingBlocks × blockFrames × channels
    MusicSource src;

    uint64_t key = 0;
    int channels = 0;
    int blockFrames = 0;
    int lastBlock = kMusicRingBlocks - 1;
    int64_t cursor = 0; // 次に読むフレーム位置 (ソースレート基準)
    MusicLoop loop;
    bool looping = false;
    bool active = false;  // スロット使用中
    bool feeding = false; // まだリングへ供給する (false = 投入完了、鳴り終わりを待つだけ)
    bool playing = false; // Start() 済み。これ以降に queued==0 になったら供給が間に合っていない
    bool underrun = false; // アンダーランを報告済み (1 回の再生につき 1 度だけ warn する)
    int64_t loops = 0;     // ループ点を回った回数 (停止時にログへ出す = 継ぎ目の実測になる)

    float volume = 1.0f;        // 曲そのものの音量
    float gain = 1.0f;          // フェードのゲイン
    float appliedVolume = -1.0f; // 最後に SetVolume した値 (無駄な呼び出しを省く)
    double fadeElapsed = 0.0;
    double fadeLength = 0.0;
    int fadeDir = 0; // +1 = フェードイン / -1 = フェードアウト / 0 = 定常
};

// ---------------------------------------------------------------------------
// 初期化 / 終了
// ---------------------------------------------------------------------------

// Slot の完全定義が見える場所でしか default にできない (unique_ptr の削除子の実体化)
MusicStreamer::MusicStreamer() = default;

MusicStreamer::~MusicStreamer()
{
    Shutdown();
}

void MusicStreamer::Init(IXAudio2* xaudio)
{
    Shutdown();
    if (xaudio == nullptr) {
        return;
    }
    bufferEnd_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); // 自動リセット
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);       // 手動リセット
    if (bufferEnd_ == nullptr || quit_ == nullptr) {
        MYE_LOG_WARN("[music] event creation failed — BGM streaming disabled");
        Shutdown();
        return;
    }
    for (std::unique_ptr<Slot>& p : slots_) {
        p = std::make_unique<Slot>();
        p->cb.bufferEnd = static_cast<HANDLE>(bufferEnd_);
    }
    xaudio_ = xaudio;
    worker_ = std::thread(&MusicStreamer::WorkerLoop, this);
    MYE_LOG_INFO("[music] streamer ready (%d slots, %d x %d ms ring)", kSlots, kMusicRingBlocks,
                 kMusicBlockMs);
}

void MusicStreamer::Shutdown()
{
    // ★破棄順はここが唯一の正。崩すとオーディオスレッドが消えたメモリを読む:
    //   1. ワーカーを殺し切る (**voice を触る前に**)
    if (worker_.joinable()) {
        if (quit_ != nullptr) {
            SetEvent(static_cast<HANDLE>(quit_));
        }
        worker_.join();
    }
    //   2. Stop → 3. FlushSourceBuffers → 4. DestroyVoice → 5. リング/デコーダ解放
    //      (ReleaseSlot がこの順で行う)
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (std::unique_ptr<Slot>& p : slots_) {
            if (p) {
                ReleaseSlot(*p);
                p.reset();
            }
        }
        xaudio_ = nullptr;
    }
    if (bufferEnd_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(bufferEnd_));
        bufferEnd_ = nullptr;
    }
    if (quit_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(quit_));
        quit_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// スロット操作 (すべて mu_ 保持下で呼ぶこと)
// ---------------------------------------------------------------------------

void MusicStreamer::ReleaseSlot(Slot& s)
{
    if (s.playing) {
        MYE_LOG_INFO("[music] stopped (%lld loop point crossings, underrun: %s)",
                     static_cast<long long>(s.loops), s.underrun ? "yes" : "no");
    }
    if (s.voice != nullptr) {
        s.voice->Stop(0);
        s.voice->FlushSourceBuffers(); // 提出済み buffer の参照を切る
        // DestroyVoice はオーディオスレッドが voice を離すまでブロックする。
        // **これより後でしかリングを解放してはいけない**
        s.voice->DestroyVoice();
        s.voice = nullptr;
    }
    s.src.Close();
    s.ring.clear();
    s.ring.shrink_to_fit();
    s.key = 0;
    s.channels = 0;
    s.blockFrames = 0;
    s.lastBlock = kMusicRingBlocks - 1;
    s.cursor = 0;
    s.loop = MusicLoop{};
    s.looping = false;
    s.active = false;
    s.feeding = false;
    s.playing = false;
    s.underrun = false;
    s.loops = 0;
    s.volume = 1.0f;
    s.gain = 1.0f;
    s.appliedVolume = -1.0f;
    s.fadeElapsed = 0.0;
    s.fadeLength = 0.0;
    s.fadeDir = 0;
}

int MusicStreamer::AcquireSlot()
{
    for (int i = 0; i < kSlots; ++i) {
        if (slots_[static_cast<size_t>(i)] && !slots_[static_cast<size_t>(i)]->active) {
            return i;
        }
    }
    // 全スロット使用中 = クロスフェードの最中にさらに次の曲が来た。
    // **一番小さく鳴っている方**を捨てる (聞こえている音への影響が最小)
    int victim = -1;
    float lowest = 0.0f;
    for (int i = 0; i < kSlots; ++i) {
        const std::unique_ptr<Slot>& p = slots_[static_cast<size_t>(i)];
        if (!p) {
            continue;
        }
        const float g = p->gain * p->volume;
        if (victim < 0 || g < lowest) {
            victim = i;
            lowest = g;
        }
    }
    return victim;
}

void MusicStreamer::FillSlot(Slot& s)
{
    if (s.voice == nullptr || !s.feeding) {
        return;
    }
    XAUDIO2_VOICE_STATE st = {};
    s.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    // ★リングが空になったまま供給が続いている = ワーカーが間に合わず音が途切れている。
    //   ストリーミングの唯一の実害なので必ず表に出す (1 再生につき 1 度だけ)
    if (s.playing && s.feeding && st.BuffersQueued == 0 && !s.underrun) {
        s.underrun = true;
        MYE_LOG_WARN("[music] ring underrun (%d x %d ms could not keep up)", kMusicRingBlocks,
                     kMusicBlockMs);
    }
    int refill = MusicRefillCount(static_cast<int>(st.BuffersQueued), kMusicRingBlocks);

    while (refill-- > 0 && s.feeding) {
        const int block = MusicNextBlock(s.lastBlock, kMusicRingBlocks);
        int16_t* dst = s.ring.data() + static_cast<size_t>(block) * s.blockFrames * s.channels;

        int64_t written = 0;
        bool justWrapped = false; // ループ直後にまた 0 なら壊れた素材 = 無限ループを防ぐ
        while (written < s.blockFrames) {
            const int64_t want = s.blockFrames - written;
            const int64_t take =
                MusicChunkFrames(s.cursor, want, s.loop, s.looping, s.src.TotalFrames());
            if (take <= 0) {
                if (!s.looping || justWrapped || !s.src.Seek(s.loop.start)) {
                    s.feeding = false; // 終端 (または壊れたループ点)
                    break;
                }
                // ★ループ点はブロック境界で量子化しない — 同じブロックの続きを
                //   loopStart から埋め続けるので、サンプル単位で継ぎ目なく回る
                s.cursor = s.loop.start;
                ++s.loops;
                justWrapped = true;
                continue;
            }
            const int64_t got = s.src.Read(dst + written * s.channels, take);
            if (got <= 0) {
                s.feeding = false; // 読み取り失敗 (共有違反/破損) はそこで打ち切る
                break;
            }
            written += got;
            s.cursor += got;
            justWrapped = false;
        }

        if (written <= 0) {
            break;
        }
        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes =
            static_cast<UINT32>(static_cast<size_t>(written) * s.channels * sizeof(int16_t));
        buf.pAudioData = reinterpret_cast<const BYTE*>(dst);
        if (!s.feeding) {
            buf.Flags = XAUDIO2_END_OF_STREAM; // 最後のブロック
        }
        if (FAILED(s.voice->SubmitSourceBuffer(&buf))) {
            s.feeding = false;
            break;
        }
        s.lastBlock = block;
    }
}

// ---------------------------------------------------------------------------
// 再生
// ---------------------------------------------------------------------------

bool MusicStreamer::Play(const MusicRequest& r, IXAudio2SubmixVoice* dest)
{
    if (xaudio_ == nullptr || dest == nullptr || r.path.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);

    // 同じ曲が既に鳴っていれば頭出しし直さない (= シーン遷移での BGM 引き継ぎ)。
    // フェードアウト中のものは「消えていく音」なので対象外
    if (r.key != 0) {
        for (const std::unique_ptr<Slot>& p : slots_) {
            if (p && p->active && p->key == r.key && p->fadeDir >= 0) {
                p->volume = std::clamp(r.volume, 0.0f, 1.0f);
                return true;
            }
        }
    }

    const int idx = AcquireSlot();
    if (idx < 0) {
        return false;
    }
    Slot& s = *slots_[static_cast<size_t>(idx)];
    ReleaseSlot(s); // 前の曲の残骸 (voice / リング / ファイル) を必ず消してから開く

    if (!s.src.Open(r.path)) {
        MYE_LOG_WARN("[music] open failed: %s", WideToUtf8(r.path).c_str());
        return false;
    }
    s.channels = s.src.Channels();
    s.blockFrames =
        static_cast<int>(static_cast<int64_t>(s.src.SampleRate()) * kMusicBlockMs / 1000);
    if (s.channels <= 0 || s.blockFrames <= 0 || s.src.TotalFrames() <= 0) {
        ReleaseSlot(s);
        return false;
    }
    // リングは開いた時に 1 度だけ確保する。**再生中に resize してはいけない**
    // (オーディオスレッドが読んでいるバッファのアドレスが動く)
    s.ring.assign(static_cast<size_t>(s.blockFrames) * s.channels * kMusicRingBlocks, 0);
    s.loop = ResolveMusicLoop(r.loopStartFrame, r.loopEndFrame, s.src.TotalFrames());
    s.looping = r.loop;
    s.cursor = 0;
    s.lastBlock = kMusicRingBlocks - 1;
    s.key = r.key;
    s.volume = std::clamp(r.volume, 0.0f, 1.0f);
    s.feeding = true;
    s.active = true;

    // BGM は素の 2D 再生 — 送りは BGM バス 1 本だけで、3D 行列もリバーブ送りも通さない。
    // ピッチも動かさないので MaxFrequencyRatio は 1.0
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = static_cast<WORD>(s.channels);
    wfx.nSamplesPerSec = s.src.SampleRate();
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(s.channels * 2);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    XAUDIO2_SEND_DESCRIPTOR send = { 0, dest };
    XAUDIO2_VOICE_SENDS sendList = { 1, &send };
    if (FAILED(xaudio_->CreateSourceVoice(&s.voice, &wfx, 0, 1.0f, &s.cb, &sendList, nullptr))) {
        s.voice = nullptr;
        ReleaseSlot(s);
        MYE_LOG_WARN("[music] CreateSourceVoice failed: %s", WideToUtf8(r.path).c_str());
        return false;
    }

    // 旧曲をフェードアウトへ回す
    const double fade = r.fadeSeconds > 0.0f ? static_cast<double>(r.fadeSeconds) : 0.0;
    for (const std::unique_ptr<Slot>& p : slots_) {
        if (!p || p.get() == &s || !p->active) {
            continue;
        }
        if (fade <= 0.0) {
            ReleaseSlot(*p);
            continue;
        }
        p->fadeLength = fade;
        // ★フェード中の曲を上書きするときは、今のゲインに相当する経過時間から継ぐ
        //   (0 から始めるとゲインが跳ね上がってクリックノイズになる)
        p->fadeElapsed = MusicFadeOutElapsedFor(p->gain, fade);
        p->fadeDir = -1;
        p->key = 0; // もう終わる曲なので同一判定には引っかけない
    }

    s.fadeElapsed = 0.0;
    s.fadeLength = fade;
    s.fadeDir = fade > 0.0 ? 1 : 0;
    s.gain = fade > 0.0 ? 0.0f : 1.0f;
    s.appliedVolume = s.gain * s.volume;
    s.voice->SetVolume(s.appliedVolume);

    // ★リングを満たしてから Start する。先読み 0 で始めると出だしが途切れる。
    //   ここだけメインスレッドでデコードするが、0.8 秒ぶんで数 ms 程度
    FillSlot(s);
    if (FAILED(s.voice->Start(0))) {
        ReleaseSlot(s);
        return false;
    }
    s.playing = true; // これ以降 queued==0 はアンダーラン (供給が間に合っていない)
    SetEvent(static_cast<HANDLE>(bufferEnd_)); // 以後の補充はワーカーに任せる
    MYE_LOG_INFO("[music] play: %s (%u ch @ %u Hz, %lld frames, loop %lld..%lld)",
                 WideToUtf8(r.path).c_str(), s.channels, s.src.SampleRate(),
                 static_cast<long long>(s.src.TotalFrames()), static_cast<long long>(s.loop.start),
                 static_cast<long long>(s.loop.end));
    return true;
}

void MusicStreamer::Stop(float fadeSeconds)
{
    if (xaudio_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const std::unique_ptr<Slot>& p : slots_) {
        if (!p || !p->active) {
            continue;
        }
        if (fadeSeconds <= 0.0f) {
            ReleaseSlot(*p);
            continue;
        }
        if (p->fadeDir < 0) {
            continue; // 既にフェードアウト中 (長さを上書きすると音量が跳ねる)
        }
        p->fadeLength = static_cast<double>(fadeSeconds);
        p->fadeElapsed = MusicFadeOutElapsedFor(p->gain, p->fadeLength);
        p->fadeDir = -1;
        p->key = 0;
    }
}

void MusicStreamer::StopNow()
{
    if (xaudio_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const std::unique_ptr<Slot>& p : slots_) {
        if (p) {
            ReleaseSlot(*p);
        }
    }
}

void MusicStreamer::Update(float dt)
{
    if (xaudio_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (const std::unique_ptr<Slot>& p : slots_) {
        if (!p || !p->active) {
            continue;
        }
        Slot& s = *p;
        if (s.fadeDir != 0) {
            // ★絶対経過時間で進める。ゲインを毎フレーム掛け足す実装にすると
            //   6500fps では丸め誤差が積もってフェード長そのものが変わる
            s.fadeElapsed += dt > 0.0f ? static_cast<double>(dt) : 0.0;
            const MusicFade g = MusicCrossfadeGains(s.fadeElapsed, s.fadeLength);
            s.gain = s.fadeDir < 0 ? g.from : g.to;
            if (s.fadeElapsed >= s.fadeLength) {
                if (s.fadeDir < 0) {
                    ReleaseSlot(s); // 消え切った
                    continue;
                }
                s.fadeDir = 0;
                s.gain = 1.0f;
            }
        }
        const float want = s.gain * s.volume;
        if (s.voice != nullptr && want != s.appliedVolume) {
            s.voice->SetVolume(want);
            s.appliedVolume = want;
        }
        // 非ループの鳴り終わりを回収する (ここで回収しないとスロットが埋まったままになる)
        if (s.voice != nullptr && !s.feeding) {
            XAUDIO2_VOICE_STATE st = {};
            s.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued == 0) {
                ReleaseSlot(s);
            }
        }
    }
}

bool MusicStreamer::Playing() const
{
    std::lock_guard<std::mutex> lock(mu_);
    for (const std::unique_ptr<Slot>& p : slots_) {
        if (p && p->active && p->fadeDir >= 0) {
            return true;
        }
    }
    return false;
}

uint64_t MusicStreamer::CurrentKey() const
{
    std::lock_guard<std::mutex> lock(mu_);
    for (const std::unique_ptr<Slot>& p : slots_) {
        if (p && p->active && p->fadeDir >= 0) {
            return p->key;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ワーカースレッド (**ECS も World も一切読まない** — 決定論契約 7)
// ---------------------------------------------------------------------------

void MusicStreamer::WorkerLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE handles[2] = { static_cast<HANDLE>(bufferEnd_), static_cast<HANDLE>(quit_) };
    for (;;) {
        // タイムアウトを置くのは起床イベントを取りこぼしても必ず追いつくため
        // (ブロック 200ms に対して 50ms は十分に細かい)。
        // **バッファ毎のログは出さない** — logging::SetCurrentFrame はメインスレッド専用
        const DWORD w = WaitForMultipleObjects(2, handles, FALSE, 50);
        if (w == WAIT_OBJECT_0 + 1) {
            break; // quit_
        }
        std::lock_guard<std::mutex> lock(mu_);
        for (const std::unique_ptr<Slot>& p : slots_) {
            if (p && p->active) {
                FillSlot(*p);
            }
        }
    }
}

} // namespace mye
