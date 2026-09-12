//====================================================================================
//                          ImpactSynthSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      09/12/2026
//                                          ImpactSynth / ImpactSoundAsset のヘッドレス検査
//====================================================================================
#include "Engine/Engine/Audio/ImpactSynthSelfTest.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/ImpactSoundAsset.h"
#include "Engine/Engine/Audio/ImpactSynth.h"
#include "Engine/Engine/Audio/SoundAsset.h"
#include "Engine/Engine/Audio/SynthCore.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

int16_t PeakOf(const AudioClip& c)
{
    int32_t peak = 0;
    for (int16_t s : c.samples) {
        const int32_t a = s < 0 ? -static_cast<int32_t>(s) : s;
        peak = (std::max)(peak, a);
    }
    return static_cast<int16_t>((std::min)(peak, 32767));
}

// [t0, t1) 秒の RMS (0..1)
double RmsIn(const AudioClip& c, double t0, double t1)
{
    const size_t a = static_cast<size_t>(t0 * c.sampleRate);
    const size_t b = (std::min)(c.samples.size(), static_cast<size_t>(t1 * c.sampleRate));
    if (b <= a) {
        return 0.0;
    }
    double acc = 0.0;
    for (size_t i = a; i < b; ++i) {
        const double v = c.samples[i] / 32767.0;
        acc += v * v;
    }
    return std::sqrt(acc / static_cast<double>(b - a));
}

// 試聴用のダンプ表。★三校 (Shadow_Sound) の assets\audio\impact\*.impact.json と同じ値 —
//   こちらを変えたらあちらも直す (耳で確認した音と実機の音が同じでなければ意味が無い)
struct DumpPreset {
    const char* name;
    ImpactSoundKind kind;
    AcousticMaterial a;
    AcousticMaterial b;
    float strength;
    float size;
    float duration;
};
// 三校の 14 本は brightness を揃えて持っている (2026-09-12 試聴 2 回目: 1.0 → 0.7 「まだ少し高い」)
constexpr float kDumpBrightness = 0.7f;
constexpr DumpPreset kDumps[] = {
    { "footstep_carpet", ImpactSoundKind::Footstep, AcousticMaterial::Grass, AcousticMaterial::Grass, 0.6f, 1.2f, 0.18f },
    { "footstep_rubber", ImpactSoundKind::Footstep, AcousticMaterial::Plastic, AcousticMaterial::Plastic, 0.6f, 1.0f, 0.20f },
    { "footstep_wood", ImpactSoundKind::Footstep, AcousticMaterial::Wood, AcousticMaterial::Wood, 0.9f, 1.0f, 0.25f },
    { "footstep_tile", ImpactSoundKind::Footstep, AcousticMaterial::Concrete, AcousticMaterial::Concrete, 1.0f, 1.0f, 0.20f },
    { "footstep_gravel", ImpactSoundKind::Footstep, AcousticMaterial::Gravel, AcousticMaterial::Gravel, 1.0f, 1.0f, 0.22f },
    { "footstep_water", ImpactSoundKind::Footstep, AcousticMaterial::Grass, AcousticMaterial::Grass, 1.0f, 2.0f, 0.30f },
    { "footstep_metal", ImpactSoundKind::Footstep, AcousticMaterial::Metal, AcousticMaterial::Metal, 1.0f, 1.0f, 0.45f },
    { "footstep_glass", ImpactSoundKind::Footstep, AcousticMaterial::Glass, AcousticMaterial::Glass, 1.0f, 0.8f, 0.40f },
    { "stone_impact", ImpactSoundKind::Impact, AcousticMaterial::Concrete, AcousticMaterial::Concrete, 0.8f, 0.5f, 0.25f },
    { "glass_impact", ImpactSoundKind::Impact, AcousticMaterial::Glass, AcousticMaterial::Concrete, 1.0f, 1.0f, 0.35f },
    { "glass_break", ImpactSoundKind::GlassBreak, AcousticMaterial::Glass, AcousticMaterial::Concrete, 1.0f, 1.0f, 0.50f },
    { "enemy_voice", ImpactSoundKind::Impact, AcousticMaterial::Plastic, AcousticMaterial::Wood, 0.5f, 0.6f, 0.12f },
    { "core_ping", ImpactSoundKind::Impact, AcousticMaterial::Metal, AcousticMaterial::Metal, 0.4f, 2.5f, 0.80f },
    { "pump_thud", ImpactSoundKind::Impact, AcousticMaterial::Metal, AcousticMaterial::Concrete, 1.0f, 4.0f, 1.20f },
};

} // namespace

bool RunImpactSynthSelfTest()
{
    MYE_LOG_INFO("==== ImpactSynth self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (1) 決定論: 同じ params はビット同一、seed 違いは別の音 ----
    {
        ImpactSynthParams p;
        p.kind = ImpactSoundKind::GlassBreak;
        p.materialA = AcousticMaterial::Glass;
        p.seed = 77;
        AudioClip a, b;
        ImpactSynthRender(p, a);
        ImpactSynthRender(p, b);
        check(!a.Empty() && a.samples == b.samples && a.channels == 1 && a.sampleRate == 44100,
              "same params render bit-identical mono PCM");
        p.seed = 78;
        AudioClip c;
        ImpactSynthRender(p, c);
        check(c.samples != a.samples, "a different seed renders different PCM");
        ImpactSynthParams zero;
        zero.durationSec = 0.0f;
        AudioClip e;
        ImpactSynthRender(zero, e);
        check(e.Empty(), "zero duration renders an empty clip (no crash)");
    }

    // ---- (2) 全 kind x 全材質: 長さが合い、ピークが正規化されている ----
    {
        bool lengths = true;
        bool peaks = true;
        for (int32_t k = 0; k < 3; ++k) {
            for (int32_t m = 0; m < kAcousticMaterialCount; ++m) {
                ImpactSynthParams p;
                p.kind = static_cast<ImpactSoundKind>(k);
                p.materialA = static_cast<AcousticMaterial>(m);
                p.materialB = static_cast<AcousticMaterial>((m + 2) % kAcousticMaterialCount);
                p.durationSec = 0.3f;
                p.seed = 1000 + static_cast<uint64_t>(k * 16 + m);
                AudioClip c;
                ImpactSynthRender(p, c);
                lengths = lengths && c.samples.size() == static_cast<size_t>(0.3f * 44100.0f + 0.5f);
                const int16_t peak = PeakOf(c);
                peaks = peaks && peak >= 16000 && peak <= 32767; // 約 -1 dBFS (0.89) に揃う
            }
        }
        check(lengths, "every kind x material renders durationSec * sampleRate frames");
        check(peaks, "every kind x material is peak-normalised (>= -6 dBFS, never clipped)");
    }

    // ---- (3) GlassBreak は破片で尾が長い (Impact より 150-300ms の RMS が大きい) ----
    {
        ImpactSynthParams p;
        p.materialA = AcousticMaterial::Glass;
        p.materialB = AcousticMaterial::Concrete;
        p.durationSec = 0.5f;
        p.seed = 5;
        p.kind = ImpactSoundKind::Impact;
        AudioClip impact;
        ImpactSynthRender(p, impact);
        p.kind = ImpactSoundKind::GlassBreak;
        AudioClip broken;
        ImpactSynthRender(p, broken);
        const double tailImpact = RmsIn(impact, 0.15, 0.30);
        const double tailBreak = RmsIn(broken, 0.15, 0.30);
        check(tailBreak > tailImpact * 1.5,
              "glass break carries shard energy in the 150-300ms tail that a plain impact lacks");
        // 試聴 3 回目の異常を固定: crack が一番大きく、破片が後から積み上がって crack を越えない。
        // 割れない瓶は「カツン」= 共鳴が 100ms で消える (グラスの「ピーン」にしない)
        check(RmsIn(broken, 0.0, 0.05) > RmsIn(broken, 0.15, 0.25) * 1.5,
              "glass break: the crack (0-50ms) is louder than the shard cluster (150-250ms)");
        check(RmsIn(impact, 0.10, 0.35) < RmsIn(impact, 0.0, 0.05) * 0.2,
              "glass impact: the ring dies within 100ms (tail < 20% of the attack)");
        // 足音は短い: 100ms 以降はほぼ無音 (Concrete)
        ImpactSynthParams f;
        f.kind = ImpactSoundKind::Footstep;
        f.materialB = AcousticMaterial::Concrete;
        f.durationSec = 0.3f;
        AudioClip step;
        ImpactSynthRender(f, step);
        check(RmsIn(step, 0.15, 0.30) < RmsIn(step, 0.0, 0.05) * 0.2,
              "a concrete footstep is front-loaded (tail is < 20% of the attack)");
    }

    // ---- (3b) 音色の軸: brightness は高域量を単調に動かし、硬い床にも低域の胴鳴りがある ----
    {
        // 高域の指標 = 1 階差分の RMS / 信号の RMS (高い成分ほど隣接差が大きい)
        auto hfRatio = [](const AudioClip& c) {
            double num = 0.0, den = 0.0;
            for (size_t i = 1; i < c.samples.size(); ++i) {
                const double d = static_cast<double>(c.samples[i]) - c.samples[i - 1];
                num += d * d;
                den += static_cast<double>(c.samples[i]) * c.samples[i];
            }
            return den > 0.0 ? std::sqrt(num / den) : 0.0;
        };
        ImpactSynthParams p;
        p.kind = ImpactSoundKind::Impact;
        p.materialA = AcousticMaterial::Concrete;
        p.materialB = AcousticMaterial::Concrete;
        p.durationSec = 0.3f;
        p.seed = 3;
        AudioClip dark, mid, bright;
        p.brightness = 0.5f;
        ImpactSynthRender(p, dark);
        p.brightness = 1.0f;
        ImpactSynthRender(p, mid);
        p.brightness = 1.5f;
        ImpactSynthRender(p, bright);
        check(hfRatio(dark) < hfRatio(mid) && hfRatio(mid) < hfRatio(bright),
              "brightness 0.5 < 1.0 < 1.5 moves the high-frequency share monotonically");

        // 低域の指標 = 200 Hz の 2 極 LPF を通した RMS / 全体 RMS。胴鳴りが無いと 1 桁小さい
        auto lowShare = [](const AudioClip& c) {
            const double alpha = 1.0 - std::exp(-6.283185307179586 * 200.0 / c.sampleRate);
            double l1 = 0.0, l2 = 0.0, num = 0.0, den = 0.0;
            for (int16_t s : c.samples) {
                l1 += alpha * (s - l1);
                l2 += alpha * (l1 - l2);
                num += l2 * l2;
                den += static_cast<double>(s) * s;
            }
            return den > 0.0 ? std::sqrt(num / den) : 0.0;
        };
        ImpactSynthParams f;
        f.kind = ImpactSoundKind::Footstep;
        f.materialB = AcousticMaterial::Concrete;
        f.durationSec = 0.2f;
        f.seed = 1300;
        AudioClip tile;
        ImpactSynthRender(f, tile);
        check(lowShare(tile) > 0.15, "a concrete footstep carries a low body (> 15% of RMS below 200 Hz)");
    }

    // ---- (4) 名前 ⇄ 列挙 ----
    {
        bool ok = true;
        for (int32_t m = 0; m < kAcousticMaterialCount; ++m) {
            AcousticMaterial back = AcousticMaterial::Wood;
            ok = ok && ParseAcousticMaterial(AcousticMaterialName(static_cast<AcousticMaterial>(m)), back)
                && back == static_cast<AcousticMaterial>(m);
        }
        for (int32_t k = 0; k < 3; ++k) {
            ImpactSoundKind back = ImpactSoundKind::Impact;
            ok = ok && ParseImpactSoundKind(ImpactSoundKindName(static_cast<ImpactSoundKind>(k)), back)
                && back == static_cast<ImpactSoundKind>(k);
        }
        AcousticMaterial dummy;
        check(ok && !ParseAcousticMaterial("Marble", dummy), "kind / material names round-trip; unknown is rejected");
    }

    // ---- (5) .impact.json の解釈 ----
    {
        const nlohmann::json j = nlohmann::json::parse(R"({
            "engine":"MyEngine", "impactSound":1, "name":"t_break",
            "kind":"GlassBreak", "materialA":"Glass", "materialB":"Metal",
            "strength":1.5, "size":0.5, "brightness":0.7, "durationSec":0.45, "seed":9000, "variations":3,
            "sound": { "volume":0.8, "volumeRandom":0.1, "pitchRandom":0.05, "bus":"SE", "priority":180,
                       "loop": true, "stream": true }
        })");
        ImpactSoundDesc d;
        const bool ok = ParseImpactSound(j, d);
        check(ok && d.name == "t_break" && d.synth.kind == ImpactSoundKind::GlassBreak
                  && d.synth.materialA == AcousticMaterial::Glass
                  && d.synth.materialB == AcousticMaterial::Metal && d.synth.strength == 1.5f
                  && d.synth.size == 0.5f && d.synth.brightness == 0.7f
                  && d.synth.durationSec == 0.45f && d.synth.seed == 9000 && d.variations == 3,
              "impact json: synth fields are read");
        check(ok && d.sound.volume == 0.8f && d.sound.volumeRandom == 0.1f
                  && d.sound.pitchRandom == 0.05f && d.sound.priority == 180 && d.sound.bus == "SE"
                  && !d.sound.loop && !d.sound.stream && d.sound.variations.empty(),
              "impact json: sound block is read; loop/stream are forced off");

        ImpactSoundDesc defaults;
        check(ParseImpactSound(nlohmann::json::parse(R"({"impactSound":1})"), defaults)
                  && defaults.variations == 4 && defaults.synth.kind == ImpactSoundKind::Impact
                  && defaults.sound.volume == 1.0f,
              "impact json: a bare file falls back to the struct defaults");
        ImpactSoundDesc bad;
        check(!ParseImpactSound(nlohmann::json::parse(R"({"sound":1,"name":"x"})"), bad),
              "impact json: a file without the impactSound marker is rejected");
        check(!ParseImpactSound(nlohmann::json::parse(R"({"impactSound":1,"kind":"Explosion"})"), bad),
              "impact json: an unknown kind is rejected");
    }

    // ---- (6) クリップ ID ----
    {
        check(ImpactClipId("glass_break", 0).value == HashStr("synth://glass_break#0")
                  && ImpactClipId("glass_break", 1) != ImpactClipId("glass_break", 0)
                  && ImpactClipId("glass_impact", 0) != ImpactClipId("glass_break", 0),
              "clip ids are HashStr of the synth:// registration name, distinct per variation");
    }

    // ---- (7) 登録経路: デバイス無しの AudioSystem + SoundLibrary ----
    {
        AudioSystem audio; // Init を呼ばない = デバイス非依存 (RegisterClip は通る)
        SoundLibrary lib;
        ImpactSoundDesc d;
        d.name = "t_reg";
        d.synth.kind = ImpactSoundKind::Impact;
        d.synth.durationSec = 0.1f;
        d.variations = 3;
        d.sound.priority = 150;
        const uint64_t hash = RegisterImpactSound(audio, lib, d);
        check(hash != 0 && lib.Get(hash) != nullptr && lib.Get(hash)->variations.size() == 3
                  && lib.Get(hash)->priority == 150 && lib.Get(hash)->name == "t_reg",
              "register: sound asset with 3 variations lands in the library");
        bool clips = true;
        for (int32_t i = 0; i < 3; ++i) {
            clips = clips && audio.HasClip(ImpactClipId("t_reg", i));
        }
        check(clips && !audio.HasClip(ImpactClipId("t_reg", 3)), "register: exactly 3 clips are registered");
        const ResolvedSound rs = ResolveSoundKey(audio, lib, HashStr("t_reg"));
        check(rs.asset != nullptr && rs.asset->hash == hash, "register: resolves by name key like a .sound.json");
        check(PickVariationIndex(*rs.asset, 7u) >= 0, "register: variations are pickable");
        // 差し替え: 同じ名前で再登録しても増えない (ホットリロードの形)
        d.variations = 2;
        const uint64_t again = RegisterImpactSound(audio, lib, d);
        check(again == hash && lib.Get(hash)->variations.size() == 2,
              "register: re-registering the same name replaces the asset in place");
    }

    // ---- (8) 試聴用 WAV ダンプ (消さない) ----
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / L"mye_impact_synth";
        std::filesystem::create_directories(dir, ec);
        bool wrote = true;
        for (const DumpPreset& d : kDumps) {
            ImpactSynthParams p;
            p.kind = d.kind;
            p.materialA = d.a;
            p.materialB = d.b;
            p.strength = d.strength;
            p.size = d.size;
            p.brightness = kDumpBrightness;
            p.durationSec = d.duration;
            p.seed = 1000;
            AudioClip c;
            ImpactSynthRender(p, c);
            const std::filesystem::path file = dir / (Utf8ToWide(d.name) + L".wav");
            wrote = WriteWavToFile(c, file.wstring()) && wrote;
        }
        check(wrote, "listening dump: every preset was written as a wav");
        MYE_LOG_INFO("[impact] listening dump: %s", WideToUtf8(dir.wstring()).c_str());
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== ImpactSynth self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== ImpactSynth self test: %d FAILED ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
