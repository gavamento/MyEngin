#include "Engine/Engine/Audio/AudioSelfTest.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/Audio/SynthCore.h"
#include "Engine/Engine/Audio/VoicePolicy.h"

namespace mye {
namespace {

void Put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

void Put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

void PutTag(std::vector<uint8_t>& v, const char* s)
{
    for (int i = 0; i < 4; ++i) {
        v.push_back(static_cast<uint8_t>(s[i]));
    }
}

// 任意のフォーマットの WAVE をメモリ上に組み立てる (デコーダを叩くための治具)。
// extensible=true なら fmt チャンクを 40 バイトの WAVE_FORMAT_EXTENSIBLE 形式にする。
std::vector<uint8_t> BuildWav(uint16_t formatTag, uint16_t bits, uint16_t channels,
                              uint32_t sampleRate, const std::vector<uint8_t>& data,
                              bool extensible)
{
    std::vector<uint8_t> fmt;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * (bits / 8));
    Put16(fmt, extensible ? uint16_t{ 0xFFFE } : formatTag);
    Put16(fmt, channels);
    Put32(fmt, sampleRate);
    Put32(fmt, sampleRate * blockAlign);
    Put16(fmt, blockAlign);
    Put16(fmt, bits);
    if (extensible) {
        Put16(fmt, 22);            // cbSize
        Put16(fmt, bits);          // wValidBitsPerSample
        Put32(fmt, 3);             // dwChannelMask
        Put16(fmt, formatTag);     // SubFormat GUID の先頭 2 バイト = 実フォーマット
        Put16(fmt, 0);
        for (int i = 0; i < 12; ++i) { // GUID 残り
            fmt.push_back(0);
        }
    }

    std::vector<uint8_t> out;
    PutTag(out, "RIFF");
    Put32(out, static_cast<uint32_t>(4 + 8 + fmt.size() + 8 + data.size()));
    PutTag(out, "WAVE");
    PutTag(out, "fmt ");
    Put32(out, static_cast<uint32_t>(fmt.size()));
    out.insert(out.end(), fmt.begin(), fmt.end());
    PutTag(out, "data");
    Put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

} // namespace

bool RunAudioSelfTest()
{
    MYE_LOG_INFO("==== Audio self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) WAV デコード: 16bit PCM (最頻ケース。memcpy 経路) ----
    {
        std::vector<uint8_t> data;
        const int16_t src[6] = { 0, 32767, -32768, 1234, -1234, 100 };
        data.resize(sizeof(src));
        std::memcpy(data.data(), src, sizeof(src));

        AudioClip clip;
        const bool ok = DecodeWav(BuildWav(1, 16, 2, 48000, data, false).data(),
                                  BuildWav(1, 16, 2, 48000, data, false).size(), clip);
        check(ok, "wav: pcm16 decodes");
        check(clip.channels == 2 && clip.sampleRate == 48000, "wav: pcm16 format fields");
        check(clip.samples.size() == 6 && clip.Frames() == 3, "wav: pcm16 frame count");
        bool same = ok && clip.samples.size() == 6;
        for (size_t i = 0; same && i < 6; ++i) {
            same = clip.samples[i] == src[i];
        }
        check(same, "wav: pcm16 samples are bit-exact (incl. both rails)");
    }

    // ---- (2) WAV デコード: 8bit は符号なし (128 が無音) ----
    {
        const std::vector<uint8_t> data = { 128, 255, 0, 192 };
        AudioClip clip;
        const auto bytes = BuildWav(1, 8, 1, 22050, data, false);
        const bool ok = DecodeWav(bytes.data(), bytes.size(), clip);
        check(ok && clip.samples.size() == 4, "wav: pcm8 decodes");
        check(ok && clip.samples[0] == 0, "wav: pcm8 midpoint 128 maps to silence");
        check(ok && clip.samples[1] == static_cast<int16_t>((255 - 128) * 256) &&
                  clip.samples[2] == static_cast<int16_t>((0 - 128) * 256),
              "wav: pcm8 rails map symmetrically");
    }

    // ---- (3) WAV デコード: 24bit は上位 16bit を採る ----
    {
        // 0x7FFFFF (最大) / 0x800000 (最小) / 0x000000
        const std::vector<uint8_t> data = { 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00 };
        AudioClip clip;
        const auto bytes = BuildWav(1, 24, 1, 44100, data, false);
        const bool ok = DecodeWav(bytes.data(), bytes.size(), clip);
        check(ok && clip.samples.size() == 3, "wav: pcm24 decodes");
        check(ok && clip.samples[0] == 32767 && clip.samples[1] == -32768 && clip.samples[2] == 0,
              "wav: pcm24 sign-extends and shifts to 16bit");
    }

    // ---- (4) WAV デコード: IEEE float32 (M19 実装が無言で弾いていた形式) ----
    {
        const float src[4] = { 0.0f, 1.0f, -1.0f, 0.5f };
        std::vector<uint8_t> data(sizeof(src));
        std::memcpy(data.data(), src, sizeof(src));
        AudioClip clip;
        const auto bytes = BuildWav(3, 32, 1, 44100, data, false);
        const bool ok = DecodeWav(bytes.data(), bytes.size(), clip);
        check(ok && clip.samples.size() == 4, "wav: ieee float32 decodes");
        check(ok && clip.samples[0] == 0 && clip.samples[1] == 32767 && clip.samples[2] == -32767,
              "wav: float rails clamp into 16bit range");
        check(ok && std::abs(static_cast<int>(clip.samples[3]) - 16384) <= 2,
              "wav: float 0.5 maps near half scale");
    }

    // ---- (5) WAV デコード: WAVE_FORMAT_EXTENSIBLE (SubFormat から実フォーマットを取る) ----
    {
        std::vector<uint8_t> data;
        const int16_t src[4] = { 10, -10, 20, -20 };
        data.resize(sizeof(src));
        std::memcpy(data.data(), src, sizeof(src));
        AudioClip clip;
        const auto bytes = BuildWav(1, 16, 2, 44100, data, true);
        const bool ok = DecodeWav(bytes.data(), bytes.size(), clip);
        check(ok && clip.channels == 2 && clip.samples.size() == 4,
              "wav: WAVE_FORMAT_EXTENSIBLE resolves via SubFormat");
    }

    // ---- (6) 壊れた入力を掴まない ----
    {
        AudioClip clip;
        check(!DecodeWav(nullptr, 0, clip), "wav: null input rejected");
        const uint8_t junk[64] = {};
        check(!DecodeWav(junk, sizeof(junk), clip), "wav: non-RIFF rejected");
        // data チャンクサイズが嘘 (0xFFFFFFFF) でも len でクランプして落ちない
        std::vector<uint8_t> bad = BuildWav(1, 16, 1, 44100, { 1, 0, 2, 0 }, false);
        bad[bad.size() - 4 - 4] = 0xFF;
        bad[bad.size() - 4 - 3] = 0xFF;
        bad[bad.size() - 4 - 2] = 0xFF;
        bad[bad.size() - 4 - 1] = 0xFF;
        AudioClip clamped;
        check(DecodeWav(bad.data(), bad.size(), clamped) && clamped.samples.size() == 2,
              "wav: oversized data chunk clamps to buffer");
    }

    // ---- (7) 合成 → WAV 書き出し → デコード のラウンドトリップがサンプル完全一致 ----
    {
        SynthParams p;
        p.wave = SynthWave::Sine;
        p.freqStart = 440.0f;
        p.freqEnd = 880.0f;
        p.durationSec = 0.05f;
        p.sampleRate = 44100;
        p.channels = 2;
        AudioClip gen;
        SynthRender(p, gen);
        check(!gen.Empty() && gen.channels == 2, "synth: renders a stereo clip");
        check(gen.Frames() == static_cast<size_t>(0.05 * 44100 + 0.5), "synth: frame count matches duration");

        std::vector<uint8_t> wav;
        check(WriteWavToMemory(gen, wav), "synth: encodes to RIFF");
        AudioClip back;
        const bool ok = DecodeWav(wav.data(), wav.size(), back);
        check(ok, "synth: encoded RIFF decodes back");
        bool identical = ok && back.samples.size() == gen.samples.size() &&
                         back.channels == gen.channels && back.sampleRate == gen.sampleRate;
        for (size_t i = 0; identical && i < gen.samples.size(); ++i) {
            identical = back.samples[i] == gen.samples[i];
        }
        check(identical, "synth: write->decode round trip is sample-exact");
    }

    // ---- (8) 合成は決定論的 (Noise を含めて 2 回が完全一致) ----
    {
        SynthParams p;
        p.wave = SynthWave::Noise;
        p.durationSec = 0.02f;
        AudioClip a;
        AudioClip b;
        SynthRender(p, a);
        SynthRender(p, b);
        check(a.samples == b.samples && !a.samples.empty(),
              "synth: noise is deterministic for the same seed");
        p.noiseSeed = 12345;
        AudioClip c;
        SynthRender(p, c);
        check(c.samples != a.samples, "synth: a different seed changes the noise");
    }

    // ---- (9) ボイススティール: 優先度が低いものから、同値なら古いものから ----
    {
        VoiceSlotInfo slots[4] = {};
        slots[0] = { true, 200, 1 }; // 重要 (奪わない)
        slots[1] = { true, 100, 2 };
        slots[2] = { true, 100, 5 }; // 同優先度だが新しい
        slots[3] = { true, 150, 3 };
        check(PickVoiceToSteal(slots, 4, 150) == 1,
              "steal: lowest priority wins, oldest breaks the tie");
        // 走査順に依存しないこと (逆順に並べても同じスロットが選ばれる)
        VoiceSlotInfo rev[4] = { slots[3], slots[2], slots[1], slots[0] };
        check(PickVoiceToSteal(rev, 4, 150) == 2, "steal: result is independent of scan order");
        // 自分より重要な音しか鳴っていなければ奪わない (= 発音をあきらめる)
        VoiceSlotInfo high[2] = {};
        high[0] = { true, 250, 1 };
        high[1] = { true, 240, 2 };
        check(PickVoiceToSteal(high, 2, 100) < 0, "steal: nothing stolen when all are louder ranked");
        // 非アクティブは候補にならない
        VoiceSlotInfo inactive[2] = {};
        inactive[0] = { false, 0, 1 };
        inactive[1] = { true, 100, 2 };
        check(PickVoiceToSteal(inactive, 2, 128) == 1, "steal: inactive slots are not candidates");
    }

    // ---- (10) dB ⇄ 線形 ----
    {
        check(std::abs(DbToLinear(0.0f) - 1.0f) < 1e-5f, "db: 0dB == unity gain");
        check(std::abs(DbToLinear(-6.0f) - 0.5011872f) < 1e-4f, "db: -6dB is about half amplitude");
        check(DbToLinear(kMinDb) == 0.0f, "db: floor is exactly silent");
        check(std::abs(LinearToDb(1.0f)) < 1e-5f, "db: unity gain == 0dB");
        check(LinearToDb(0.0f) == kMinDb, "db: silence clamps to the floor");
        const float rt = LinearToDb(DbToLinear(-12.0f));
        check(std::abs(rt + 12.0f) < 1e-3f, "db: round trip");
    }

    // ---- (11) ディスク往復 (M45b の Save as .wav が通る経路そのもの) ----
    // WriteWavToMemory はメモリ内で検証済みなので、ここでは **ファイル出口と入口**
    // (WriteWavToFile → LoadAudioFile) がサンプルを保つことだけを見る
    {
        std::error_code ec;
        const std::filesystem::path path =
            std::filesystem::temp_directory_path(ec) / L"mye_audio_selftest.wav";
        std::filesystem::remove(path, ec);
        SynthParams p;
        p.wave = SynthWave::Triangle;
        p.durationSec = 0.03f;
        p.channels = 1;
        AudioClip gen;
        SynthRender(p, gen);
        check(WriteWavToFile(gen, path.wstring()), "wav file: written to disk");
        AudioClip back;
        const bool loaded = LoadAudioFile(path.wstring(), back);
        check(loaded, "wav file: loaded back from disk");
        check(loaded && back.samples == gen.samples && back.channels == gen.channels &&
                  back.sampleRate == gen.sampleRate,
              "wav file: disk round trip is sample-exact");
        std::filesystem::remove(path, ec);
    }

    // ---- (12) .sound.json のラウンドトリップ (M45c) ----
    // 「保存したものがそのまま読み戻る」ことと、**旧形式の文字列 clip 参照が
    //   数値 GUID と両立する**ことを見る (.mat.json / .controller.json と同じ M39a 規約)
    {
        SoundAsset s;
        s.name = "hit";
        s.variations.push_back({ 0x1122334455667788ull, {}, 3 });
        s.variations.push_back({ 0xAABBCCDDEEFF0011ull, {}, 1 });
        s.volume = 0.8f;
        s.volumeRandom = 0.1f;
        s.pitch = 1.2f;
        s.pitchRandom = 0.05f;
        s.loop = true;
        s.stream = true;
        s.bus = "BGM";
        s.priority = 200;
        s.maxInstances = 4;
        s.spatialBlend = 1.0f;
        s.minDistance = 2.0f;
        s.maxDistance = 30.0f;
        s.rolloff = SoundRolloff::Inverse;
        s.dopplerScale = 0.5f;
        s.reverbSend = 0.25f;
        s.loopStartSample = 100;
        s.loopEndSample = 200;

        SoundAsset back;
        const bool ok = SoundLibrary::FromJson(SoundLibrary::ToJson(s), back);
        check(ok, "sound: ToJson/FromJson round trip parses");
        check(ok && back.variations.size() == 2 && back.variations[0].clip == s.variations[0].clip
                  && back.variations[1].weight == 1,
              "sound: variations survive as numeric GUIDs");
        check(ok && back.volume == s.volume && back.pitch == s.pitch && back.loop && back.stream
                  && back.bus == "BGM" && back.priority == 200 && back.maxInstances == 4,
              "sound: 2D playback fields round trip");
        check(ok && back.spatialBlend == 1.0f && back.minDistance == 2.0f
                  && back.maxDistance == 30.0f && back.rolloff == SoundRolloff::Inverse
                  && back.dopplerScale == 0.5f && back.reverbSend == 0.25f,
              "sound: 3D fields round trip (incl. rolloff enum by name)");
        check(ok && back.loopStartSample == 100 && back.loopEndSample == 200,
              "sound: loop points round trip");

        // 旧形式 (文字列パス) と欠損フィールドの前方互換
        SoundAsset legacy;
        const nlohmann::json lj = nlohmann::json::parse(
            R"({"variations":[{"clip":"sfx/hit.wav"}],"loop":1})");
        const bool lok = SoundLibrary::FromJson(lj, legacy);
        check(lok && legacy.variations.size() == 1 && legacy.variations[0].clip == 0
                  && legacy.variations[0].clipPath == "sfx/hit.wav",
              "sound: legacy string clip goes to clipPath (resolved on load)");
        check(lok && legacy.loop && legacy.bus == "SE" && legacy.volume == 1.0f,
              "sound: numeric bool is accepted and missing fields take defaults");
    }

    // ---- (13) バリエーション抽選は重みどおり・走査順非依存 ----
    {
        SoundAsset s;
        s.variations.push_back({ 11, {}, 3 }); // roll 0,1,2
        s.variations.push_back({ 22, {}, 0 }); // weight 0 = 候補外
        s.variations.push_back({ 33, {}, 1 }); // roll 3
        check(PickVariationIndex(s, 0) == 0 && PickVariationIndex(s, 2) == 0,
              "sound: weight 3 covers the first three rolls");
        check(PickVariationIndex(s, 3) == 2, "sound: weight-0 entry is skipped");
        check(PickVariationIndex(s, 4) == 0, "sound: roll wraps by total weight");
        SoundAsset empty;
        check(PickVariationIndex(empty, 0) < 0, "sound: no candidate yields -1");
        SoundAsset unresolved;
        unresolved.variations.push_back({ 0, "missing.wav", 5 });
        check(PickVariationIndex(unresolved, 0) < 0, "sound: unresolved clip is not a candidate");

        // PlayDesc への写像 (バス名解決 + 揺らぎ 0 で決定論)
        SoundAsset p;
        p.variations.push_back({ 99, {}, 1 });
        p.bus = "ui";
        p.volume = 0.5f;
        p.pitch = 1.5f;
        p.priority = 7;
        p.loop = true;
        const PlayDesc d = MakePlayDesc(p, 0, 0.0f, 0.0f);
        check(d.clip.value == 99 && d.bus == AudioSystem::kBusUi && d.volume == 0.5f
                  && d.pitch == 1.5f && d.priority == 7 && d.loop,
              "sound: MakePlayDesc maps fields and resolves the bus name case-insensitively");
        SoundAsset bad;
        bad.variations.push_back({ 1, {}, 1 });
        bad.bus = "NoSuchBus";
        check(MakePlayDesc(bad, 0, 0.0f, 0.0f).bus == AudioSystem::kBusSe,
              "sound: unknown bus name falls back to SE");
    }

    // ※OGG デコードは selftest に fixture を持たない (エンコーダを同梱しないため)。
    //   実 .ogg での確認は M45f (BGM ストリーミング) の実機検証で担保する。
    //   M45a 時点では実ファイル 1 本を一時ハーネスで通し、1ch/48kHz/0.35s/peak 4474 =
    //   無音でないことを実測済み。

    if (failCount == 0) {
        MYE_LOG_INFO("==== Audio self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Audio self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
