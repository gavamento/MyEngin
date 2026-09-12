//====================================================================================
//                          ImpactSoundAsset.cpp
//  MyEngine/ 秋田蓮音                                                      09/12/2026
//                                          .impact.json の読込と手続きクリップの登録
//====================================================================================
#include "Engine/Engine/Audio/ImpactSoundAsset.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

float ReadFloatOr(const json& j, const char* key, float def)
{
    if (!j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<float>();
}

// "glass_break.impact.json" → "glass_break"
std::string NameFromImpactPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.impact"
    const std::string suf = ".impact";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

} // namespace

bool ParseImpactSound(const json& j, ImpactSoundDesc& out)
{
    if (!j.is_object() || !j.contains("impactSound")) {
        return false;
    }
    ImpactSoundDesc d;
    if (j.contains("name") && j["name"].is_string()) {
        d.name = j["name"].get<std::string>();
    }
    if (j.contains("kind")) {
        if (!j["kind"].is_string() || !ParseImpactSoundKind(j["kind"].get<std::string>(), d.synth.kind)) {
            MYE_LOG_WARN("[impact] unknown kind in %s", d.name.c_str());
            return false;
        }
    }
    if (j.contains("materialA")) {
        if (!j["materialA"].is_string()
            || !ParseAcousticMaterial(j["materialA"].get<std::string>(), d.synth.materialA)) {
            MYE_LOG_WARN("[impact] unknown materialA in %s", d.name.c_str());
            return false;
        }
    }
    if (j.contains("materialB")) {
        if (!j["materialB"].is_string()
            || !ParseAcousticMaterial(j["materialB"].get<std::string>(), d.synth.materialB)) {
            MYE_LOG_WARN("[impact] unknown materialB in %s", d.name.c_str());
            return false;
        }
    }
    const ImpactSynthParams def;
    d.synth.strength = ReadFloatOr(j, "strength", def.strength);
    d.synth.size = ReadFloatOr(j, "size", def.size);
    d.synth.brightness = ReadFloatOr(j, "brightness", def.brightness);
    d.synth.durationSec = std::clamp(ReadFloatOr(j, "durationSec", def.durationSec), 0.02f, 5.0f);
    if (j.contains("sampleRate") && j["sampleRate"].is_number_unsigned()) {
        d.synth.sampleRate = j["sampleRate"].get<uint32_t>();
    }
    if (j.contains("seed") && j["seed"].is_number_unsigned()) {
        d.synth.seed = j["seed"].get<uint64_t>();
    }
    if (j.contains("variations") && j["variations"].is_number_integer()) {
        d.variations = std::clamp(j["variations"].get<int32_t>(), 1, 16);
    }
    if (j.contains("sound") && j["sound"].is_object()) {
        SoundLibrary::FromJson(j["sound"], d.sound); // 無いキーは .sound.json と同じ既定値
    }
    // 一発再生の資産なので loop / stream は意味を持たせない (BGM レーンへ迷い込ませない)
    d.sound.loop = false;
    d.sound.stream = false;
    d.sound.variations.clear();
    out = std::move(d);
    return true;
}

std::string ImpactClipName(const std::string& name, int32_t variation)
{
    return "synth://" + name + "#" + std::to_string(variation);
}

AssetID ImpactClipId(const std::string& name, int32_t variation)
{
    return AssetID{ HashStr(ImpactClipName(name, variation)) };
}

uint64_t RegisterImpactSound(AudioSystem& audio, SoundLibrary& sounds, const ImpactSoundDesc& desc)
{
    if (desc.name.empty()) {
        MYE_LOG_WARN("[impact] a procedural sound needs a name");
        return 0;
    }
    const auto t0 = std::chrono::steady_clock::now();
    SoundAsset asset = desc.sound;
    asset.name = desc.name;
    asset.variations.clear();
    const int32_t n = std::clamp(desc.variations, 1, 16);
    for (int32_t i = 0; i < n; ++i) {
        ImpactSynthParams p = desc.synth;
        p.seed = desc.synth.seed + static_cast<uint64_t>(i); // 計画 §19: seed だけ変える
        AudioClip clip;
        ImpactSynthRender(p, clip);
        if (clip.Empty()) {
            continue;
        }
        const AssetID id = ImpactClipId(desc.name, i);
        if (audio.RegisterClip(id, std::move(clip), ImpactClipName(desc.name, i)).IsNull()) {
            continue;
        }
        SoundVariation v;
        v.clip = id.value;
        v.weight = 1;
        asset.variations.push_back(v);
    }
    if (asset.variations.empty()) {
        MYE_LOG_WARN("[impact] %s: no variation rendered", desc.name.c_str());
        return 0;
    }
    // 仮想パス: 実ファイルを指さない (ヘッダの注記)。名前キーは Register が張る
    const std::wstring virtualPath = L"synth://" + Utf8ToWide(desc.name);
    const uint64_t hash = sounds.Register(virtualPath, std::move(asset));
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MYE_LOG_INFO("[impact] %s: %d variations (%s %s/%s) %.1f ms", desc.name.c_str(), n,
                 ImpactSoundKindName(desc.synth.kind), AcousticMaterialName(desc.synth.materialA),
                 AcousticMaterialName(desc.synth.materialB), ms);
    return hash;
}

uint64_t LoadImpactSoundFile(AudioSystem& audio, SoundLibrary& sounds, const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[impact] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    ImpactSoundDesc d;
    if (!ParseImpactSound(j, d)) {
        MYE_LOG_WARN("[impact] not an impact sound: %s", WideToUtf8(path).c_str());
        return 0;
    }
    d.name = NameFromImpactPath(path); // .sound.json と同じく名前はファイル名が正本
    return RegisterImpactSound(audio, sounds, d);
}

} // namespace mye
