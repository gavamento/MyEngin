#include "Engine/Engine/Audio/AudioSelfTest.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioClip.h"
#include "Engine/Engine/Audio/AudioMixer.h"
#include "Engine/Engine/Audio/AudioSourceSystem.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/Audio/SpatialMath.h"
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

        // PlayDesc への写像 (バス名解決 + 揺らぎ 0 で決定論)。
        // **Init を呼ばない AudioSystem** は既定バス構成をデータとしてだけ持つので、
        // デバイス非依存のままバス名解決を検証できる
        const AudioSystem audio;
        SoundAsset p;
        p.variations.push_back({ 99, {}, 1 });
        p.bus = "ui";
        p.volume = 0.5f;
        p.pitch = 1.5f;
        p.priority = 7;
        p.loop = true;
        const PlayDesc d = MakePlayDesc(p, 0, 0.0f, 0.0f, audio);
        check(d.clip.value == 99 && d.bus == AudioSystem::kBusUi && d.volume == 0.5f
                  && d.pitch == 1.5f && d.priority == 7 && d.loop,
              "sound: MakePlayDesc maps fields and resolves the bus name case-insensitively");
        SoundAsset bad;
        bad.variations.push_back({ 1, {}, 1 });
        bad.bus = "NoSuchBus";
        check(MakePlayDesc(bad, 0, 0.0f, 0.0f, audio).bus == AudioSystem::kBusSe,
              "sound: unknown bus name falls back to SE");
    }

    // ---- (14) ミキサー: dB 変換 / トポロジ検証 / ソロ (M45d) ----
    {
        check(std::fabs(DbToLinear(0.0f) - 1.0f) < 1e-5f, "mixer: 0 dB is unity gain");
        check(DbToLinear(kMinDb) == 0.0f && DbToLinear(kMinDb - 10.0f) == 0.0f,
              "mixer: the bottom of the fader is exact silence");
        check(std::fabs(DbToLinear(-6.0f) - 0.5011872f) < 1e-5f, "mixer: -6 dB is about half");
        check(LinearToDb(0.0f) == kMinDb && LinearToDb(-1.0f) == kMinDb,
              "mixer: non-positive gain maps to the bottom of the fader");
        check(std::fabs(LinearToDb(DbToLinear(-12.5f)) + 12.5f) < 1e-3f,
              "mixer: dB <-> linear round trips");

        std::string err;
        const MixerAsset def = DefaultMixer();
        check(ValidateMixer(def, &err), "mixer: the default mixer is a valid topology");
        check(FindMixerBus(def, "se") == 2 && FindMixerBus(def, "nope") < 0,
              "mixer: bus lookup ignores case and reports misses");
        const std::vector<int> depth = MixerBusDepths(def);
        check(depth.size() == 4 && depth[0] == 0 && depth[1] == 1 && depth[3] == 1,
              "mixer: depths are measured from the root");

        // 検証で弾かれるべき形
        MixerAsset dup = def;
        dup.buses.push_back(dup.buses[1]); // 同名 (BGM) を 2 本
        check(!ValidateMixer(dup, &err), "mixer: duplicate bus names are rejected");
        MixerAsset orphan = def;
        orphan.buses[1].parent = "Ghost";
        check(!ValidateMixer(orphan, &err), "mixer: an unknown parent is rejected");
        MixerAsset twoRoots = def;
        twoRoots.buses[1].parent.clear();
        check(!ValidateMixer(twoRoots, &err), "mixer: a second root bus is rejected");
        MixerAsset cycle = def;
        cycle.buses[0].parent = "UI"; // Master -> UI -> Master
        check(!ValidateMixer(cycle, &err), "mixer: a parent cycle is rejected");
        MixerAsset noName = def;
        noName.buses[2].name.clear();
        check(!ValidateMixer(noName, &err), "mixer: an empty bus name is rejected");

        // ソロ: 「ソロバス + 祖先 + 子孫」だけが残る
        MixerAsset solo = def;
        solo.buses.push_back(MixerBus{ "Voice", "BGM", 0.0f, false, false, 0.0f });
        solo.buses[1].solo = true; // BGM
        const std::vector<uint8_t> m = MixerEffectiveMutes(solo);
        check(m.size() == 5 && m[0] == 0, "mixer: solo keeps the ancestors audible");
        check(m[1] == 0 && m[4] == 0, "mixer: solo keeps the soloed bus and its children");
        check(m[2] == 1 && m[3] == 1, "mixer: solo silences everything else");
        MixerAsset muteOnly = def;
        muteOnly.buses[2].mute = true;
        const std::vector<uint8_t> mm = MixerEffectiveMutes(muteOnly);
        check(mm[0] == 0 && mm[1] == 0 && mm[2] == 1 && mm[3] == 0,
              "mixer: without solo only the muted bus is silenced");

        // JSON ラウンドトリップ + プリセット名
        MixerAsset src = def;
        src.buses[1].volumeDb = -7.5f;
        src.buses[2].mute = true;
        src.buses[3].reverbSend = 0.4f;
        src.reverbPreset = "Cave";
        src.reverbWetDryMix = 80.0f;
        MixerAsset back;
        const bool ok = MixerLibrary::FromJson(MixerLibrary::ToJson(src), back);
        check(ok && back.buses.size() == 4 && back.buses[1].volumeDb == -7.5f && back.buses[2].mute
                  && back.buses[3].reverbSend == 0.4f && back.buses[1].parent == "Master",
              "mixer: bus fields round trip through JSON");
        check(ok && back.reverbPreset == "Cave" && back.reverbWetDryMix == 80.0f,
              "mixer: reverb settings round trip");
        bool presetsOk = true;
        for (int i = 0; i < kReverbPresetCount; ++i) {
            presetsOk = presetsOk && ReverbPresetIndex(ReverbPresetName(i)) == i;
        }
        check(presetsOk, "mixer: reverb preset names resolve back to their index");
        check(ReverbPresetIndex("no such preset") == 0,
              "mixer: an unknown preset name falls back to Default");
    }

    // ---- (15) 3D: 距離減衰カーブ + tick 基準の速度推定 (M45e) ----
    {
        // X3DAudio へ渡すカーブの形式的な要求: 先頭 0.0 / 末尾 1.0 / 距離は狭義単調増加
        for (int rolloff = 0; rolloff <= 2; ++rolloff) {
            AudioCurvePoint pts[kMaxRolloffCurvePoints];
            const int n = BuildRolloffCurve(rolloff, 1.0f, 50.0f, pts, kMaxRolloffCurvePoints);
            bool shapeOk = n >= 2 && pts[0].distance == 0.0f && pts[0].dsp == 1.0f
                        && pts[n - 1].distance == 1.0f && pts[n - 1].dsp == 0.0f;
            for (int i = 1; i < n; ++i) {
                shapeOk = shapeOk && pts[i].distance > pts[i - 1].distance;
                shapeOk = shapeOk && pts[i].dsp <= pts[i - 1].dsp + 1e-6f; // 単調非増加
            }
            check(shapeOk, "spatial: the rolloff curve is a valid descending 0..1 X3DAudio curve");
        }
        // 退化した入力でも X3DAudio の要求 (2 点以上・0.0/1.0 端点) を割らない
        {
            AudioCurvePoint tiny[2];
            const int n2 = BuildRolloffCurve(0, 1.0f, 1.0f, tiny, 2); // min == max
            check(n2 == 2 && tiny[0].distance == 0.0f && tiny[1].distance == 1.0f,
                  "spatial: a degenerate min/max still produces a two-point curve");
            AudioCurvePoint wide[kMaxRolloffCurvePoints];
            const int n3 = BuildRolloffCurve(0, 0.0f, 1e6f, wide, kMaxRolloffCurvePoints);
            bool inc = n3 >= 2;
            for (int i = 1; i < n3; ++i) {
                inc = inc && wide[i].distance > wide[i - 1].distance;
            }
            check(inc, "spatial: an extreme min/max ratio stays strictly increasing");
        }

        // 減衰値そのもの
        check(RolloffGain(0, 2.0f, 40.0f, 1.0f) == 1.0f && RolloffGain(0, 2.0f, 40.0f, 2.0f) == 1.0f,
              "spatial: no attenuation inside min distance");
        check(RolloffGain(0, 2.0f, 40.0f, 41.0f) == 0.0f,
              "spatial: silent beyond max distance (the curve's last point is 0)");
        check(std::fabs(RolloffGain(0, 2.0f, 40.0f, 4.0f) - 0.5f) < 1e-5f,
              "spatial: logarithmic rolloff halves at twice the min distance");
        check(std::fabs(RolloffGain(2, 2.0f, 40.0f, 4.0f) - 0.25f) < 1e-5f,
              "spatial: inverse rolloff is the square of the logarithmic one");
        check(std::fabs(RolloffGain(1, 0.0f, 10.0f, 5.0f) - 0.5f) < 1e-3f,
              "spatial: linear rolloff is half way at half the max distance");

        // 速度推定: 立ち上がり / 0-tick フレーム / テレポート
        constexpr float kDt = 1.0f / 60.0f;
        VelocitySample vs;
        UpdateVelocitySample(vs, AudioVec3{ 0.0f, 0.0f, 0.0f }, 100, kDt);
        check(vs.valid && vs.velocity.x == 0.0f, "spatial: the first sample starts at rest");
        UpdateVelocitySample(vs, AudioVec3{ 1.0f, 0.0f, 0.0f }, 101, kDt);
        const float afterOne = vs.velocity.x;
        check(afterOne > 0.0f, "spatial: velocity picks up once the tick advances");
        UpdateVelocitySample(vs, AudioVec3{ 9.0f, 0.0f, 0.0f }, 101, kDt); // 同 tick
        check(vs.velocity.x == afterOne && vs.position.x == 1.0f,
              "spatial: a 0-tick frame leaves the estimate untouched");

        // 等速運動なら真の速度へ収束する (1 tick で 0.05m = 3 m/s)
        VelocitySample run;
        UpdateVelocitySample(run, AudioVec3{}, 0, kDt);
        for (uint64_t t = 1; t <= 60; ++t) {
            UpdateVelocitySample(run, AudioVec3{ 0.05f * static_cast<float>(t), 0.0f, 0.0f }, t, kDt);
        }
        check(std::fabs(run.velocity.x - 3.0f) < 0.05f,
              "spatial: constant motion converges to the true velocity");

        // ★フレームレート非依存: 毎 tick 呼んでも 3 tick に 1 回まとめて呼んでも、
        //   同じ tick 数を進めば同じ速度になる (これが tick 基準サンプリングの存在理由)
        VelocitySample coarse;
        UpdateVelocitySample(coarse, AudioVec3{}, 0, kDt);
        for (uint64_t t = 3; t <= 60; t += 3) {
            UpdateVelocitySample(coarse, AudioVec3{ 0.05f * static_cast<float>(t), 0.0f, 0.0f }, t,
                                 kDt);
        }
        check(std::fabs(coarse.velocity.x - run.velocity.x) < 0.05f,
              "spatial: multi-tick catch-up frames agree with per-tick sampling");

        // テレポート (1 tick で 5m 超) は速度を積まない
        VelocitySample tp;
        UpdateVelocitySample(tp, AudioVec3{}, 0, kDt);
        UpdateVelocitySample(tp, AudioVec3{ 100.0f, 0.0f, 0.0f }, 1, kDt);
        check(tp.velocity.x == 0.0f && tp.position.x == 100.0f,
              "spatial: a teleport is not turned into a huge velocity");
        // tick の巻き戻し (シーン再ロード / リプレイ開始) は仕切り直し
        VelocitySample rew = run;
        UpdateVelocitySample(rew, AudioVec3{ 7.0f, 0.0f, 0.0f }, 1, kDt);
        check(rew.velocity.x == 0.0f, "spatial: a rewound tick resets the estimate");
    }

    // ---- (16) AudioSource の上書き規則 (M45e) ----
    {
        const AudioSystem audio; // Init を呼ばない = デバイス非依存 (13 と同じ手)
        SoundAsset asset;
        asset.variations.push_back({ 42, {}, 1 });
        asset.bus = "BGM";
        asset.volume = 0.8f;
        asset.pitch = 1.0f;
        asset.loop = true;
        asset.priority = 60;
        asset.spatialBlend = 0.25f;
        asset.minDistance = 3.0f;
        asset.maxDistance = 70.0f;
        asset.rolloff = SoundRolloff::Linear;
        asset.dopplerScale = 2.0f;
        asset.reverbSend = 0.6f;

        AudioSourceComponent src; // 既定 = 何も上書きしない
        PlayDesc d;
        AudioSpatial sp;
        MakeSourcePlay(asset, src, audio, 0, 0.0f, 0.0f, d, sp);
        check(d.clip.value == 42 && d.bus == AudioSystem::kBusBgm && d.loop && d.priority == 60,
              "source: defaults pass the asset's own settings through");
        check(std::fabs(d.volume - 0.8f) < 1e-5f && std::fabs(d.pitch - 1.0f) < 1e-5f,
              "source: unity volume/pitch multipliers leave the asset values alone");
        check(sp.spatialBlend == 0.25f && sp.minDistance == 3.0f && sp.maxDistance == 70.0f
                  && sp.rolloff == 1 && sp.dopplerScale == 2.0f && sp.reverbSend == 0.6f,
              "source: 3D values come from the asset while overrideAttenuation is off");

        // 2D 側の上書き
        src.volume = 0.5f;
        src.pitch = 2.0f;
        src.loop = 0;
        src.priority = 200;
        std::strncpy(src.bus, "ui", sizeof(src.bus) - 1);
        MakeSourcePlay(asset, src, audio, 0, 0.0f, 0.0f, d, sp);
        check(std::fabs(d.volume - 0.4f) < 1e-5f, "source: volume multiplies the asset value");
        check(std::fabs(d.pitch - 2.0f) < 1e-5f, "source: pitch multiplies the asset value");
        check(!d.loop && d.priority == 200 && d.bus == AudioSystem::kBusUi,
              "source: loop / priority / bus overrides win over the asset");
        check(sp.pitch == d.pitch, "source: doppler rides on the final pitch ratio");

        // 解決できないバス名はアセット既定に留まる (黙って無音のバスへ流さない)
        AudioSourceComponent ghost;
        std::strncpy(ghost.bus, "NoSuchBus", sizeof(ghost.bus) - 1);
        MakeSourcePlay(asset, ghost, audio, 0, 0.0f, 0.0f, d, sp);
        check(d.bus == AudioSystem::kBusBgm, "source: an unknown bus name keeps the asset's bus");

        // mute と 3D 上書き
        AudioSourceComponent over;
        over.mute = 1;
        over.overrideAttenuation = 1;
        over.spatialBlend = 1.0f;
        over.minDistance = 5.0f;
        over.maxDistance = 25.0f;
        over.rolloff = 2;
        over.dopplerScale = 0.0f;
        over.reverbSend = 0.1f;
        MakeSourcePlay(asset, over, audio, 0, 0.0f, 0.0f, d, sp);
        check(d.volume == 0.0f, "source: mute silences the voice");
        check(sp.spatialBlend == 1.0f && sp.minDistance == 5.0f && sp.maxDistance == 25.0f
                  && sp.rolloff == 2 && sp.dopplerScale == 0.0f && sp.reverbSend == 0.1f,
              "source: overrideAttenuation switches every 3D value to the component");
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
