#include "Engine/Engine/Audio/SoundAsset.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Engine/Core/AssetGuidResolver.h"
#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.sound.json" → "X.sound"
    const std::string suf = ".sound";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

const char* RolloffToStr(SoundRolloff r)
{
    switch (r) {
    case SoundRolloff::Linear: return "linear";
    case SoundRolloff::Inverse: return "inverse";
    case SoundRolloff::Logarithmic:
    default: return "logarithmic";
    }
}

SoundRolloff StrToRolloff(const std::string& s)
{
    if (s == "linear") {
        return SoundRolloff::Linear;
    }
    if (s == "inverse") {
        return SoundRolloff::Inverse;
    }
    return SoundRolloff::Logarithmic;
}

// 手編集された .sound.json で true/false が 1/0 と書かれていても拾う
// (nlohmann の value<bool> は型が違うと throw するので自前で分岐する)
bool ReadBool(const json& j, const char* key, bool def)
{
    if (!j.contains(key)) {
        return def;
    }
    const json& v = j[key];
    if (v.is_boolean()) {
        return v.get<bool>();
    }
    if (v.is_number()) {
        return v.get<double>() != 0.0;
    }
    return def;
}

float ReadFloat(const json& j, const char* key, float def)
{
    if (!j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<float>();
}

} // namespace

// ==== SoundLibrary ====

uint64_t SoundLibrary::HashForPath(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t SoundLibrary::Register(const std::wstring& path, SoundAsset asset)
{
    const uint64_t hash = HashForPath(path);
    asset.hash = hash;
    asset.path = path;
    if (asset.name.empty()) {
        asset.name = NameFromPath(path);
    }
    sounds_[hash] = std::move(asset);
    return hash;
}

uint64_t SoundLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[sound] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    SoundAsset a;
    if (!FromJson(j, a)) {
        return 0;
    }
    a.name = NameFromPath(path);
    // 旧形式の clipPath (文字列参照) は .sound.json のディレクトリからの相対で解決する。
    // GUID 参照は FromJson が clip を直接埋めるのでここは素通り
    const fs::path dir = fs::path(path).parent_path();
    for (SoundVariation& v : a.variations) {
        if (v.clip == 0 && !v.clipPath.empty()) {
            const std::wstring full = (dir / Utf8ToWide(v.clipPath)).wstring();
            v.clip = AudioSystem::IdForFile(full).value;
        }
    }
    return Register(path, std::move(a));
}

bool SoundLibrary::SaveToFile(uint64_t hash) const
{
    const SoundAsset* s = Get(hash);
    if (s == nullptr) {
        return false;
    }
    std::ofstream f(fs::path(s->path), std::ios::binary);
    if (!f) {
        return false;
    }
    f << ToJson(*s).dump(2);
    return true;
}

const SoundAsset* SoundLibrary::Get(uint64_t hash) const
{
    auto it = sounds_.find(hash);
    return it == sounds_.end() ? nullptr : &it->second;
}

SoundAsset* SoundLibrary::GetMutable(uint64_t hash)
{
    auto it = sounds_.find(hash);
    return it == sounds_.end() ? nullptr : &it->second;
}

std::vector<SoundEntry> SoundLibrary::Enumerate() const
{
    std::vector<SoundEntry> out;
    out.reserve(sounds_.size());
    for (auto it = sounds_.begin(); it != sounds_.end(); ++it) {
        out.push_back(SoundEntry{ it->first, it->second.name });
    }
    // 明示キーで並べる (spec 11.2 規則 7: ハッシュの反復順を表に出さない)
    std::sort(out.begin(), out.end(), [](const SoundEntry& a, const SoundEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

ClipUsage SoundLibrary::UsageOfClip(uint64_t clip) const
{
    if (clip == 0) {
        return ClipUsage::None;
    }
    bool stream = false;
    // 反復順に依存しない (「1 つでも stream=false があれば Sampled」は順序無関係な述語)
    for (auto it = sounds_.begin(); it != sounds_.end(); ++it) {
        for (const SoundVariation& v : it->second.variations) {
            if (v.clip != clip) {
                continue;
            }
            if (!it->second.stream) {
                return ClipUsage::Sampled; // SE として使われている = 展開が要る
            }
            stream = true;
        }
    }
    return stream ? ClipUsage::StreamOnly : ClipUsage::None;
}

json SoundLibrary::ToJson(const SoundAsset& s)
{
    json root;
    root["engine"] = "MyEngine";
    root["sound"] = 1;
    root["name"] = s.name;
    json vars = json::array();
    for (const SoundVariation& v : s.variations) {
        // 解決済みは GUID (数値) で書く — クリップのリネーム/移動に追従する。
        // 未解決 (clip==0) は旧 clipPath 文字列を温存 (壊れた参照を黙って消さない)
        json clip;
        if (v.clip != 0) {
            clip = v.clip;
        } else {
            clip = v.clipPath;
        }
        vars.push_back({ { "clip", std::move(clip) }, { "weight", v.weight } });
    }
    root["variations"] = std::move(vars);
    root["volume"] = s.volume;
    root["volumeRandom"] = s.volumeRandom;
    root["pitch"] = s.pitch;
    root["pitchRandom"] = s.pitchRandom;
    root["loop"] = s.loop;
    root["stream"] = s.stream;
    root["bus"] = s.bus;
    root["priority"] = s.priority;
    root["maxInstances"] = s.maxInstances;
    root["spatialBlend"] = s.spatialBlend;
    root["minDistance"] = s.minDistance;
    root["maxDistance"] = s.maxDistance;
    root["rolloff"] = RolloffToStr(s.rolloff);
    root["dopplerScale"] = s.dopplerScale;
    root["reverbSend"] = s.reverbSend;
    root["loopStartSample"] = s.loopStartSample;
    root["loopEndSample"] = s.loopEndSample;
    return root;
}

bool SoundLibrary::FromJson(const json& j, SoundAsset& out)
{
    if (!j.is_object()) {
        return false;
    }
    // variations が無くても «空の音» として読む (Create 直後の雛形がこれ)
    out.variations.clear();
    if (j.contains("variations") && j["variations"].is_array()) {
        for (const json& vj : j["variations"]) {
            SoundVariation v;
            if (vj.contains("clip")) {
                const json& clip = vj["clip"];
                if (clip.is_number_unsigned() || clip.is_number_integer()) {
                    v.clip = clip.get<uint64_t>();
                } else if (clip.is_string()) {
                    v.clipPath = clip.get<std::string>();
                }
            }
            v.weight = vj.value("weight", 1);
            out.variations.push_back(std::move(v));
        }
    }
    out.name = j.value("name", std::string());
    out.volume = ReadFloat(j, "volume", 1.0f);
    out.volumeRandom = ReadFloat(j, "volumeRandom", 0.0f);
    out.pitch = ReadFloat(j, "pitch", 1.0f);
    out.pitchRandom = ReadFloat(j, "pitchRandom", 0.0f);
    out.loop = ReadBool(j, "loop", false);
    out.stream = ReadBool(j, "stream", false);
    out.bus = j.value("bus", std::string("SE"));
    out.priority = j.value("priority", 128);
    out.maxInstances = j.value("maxInstances", 0);
    out.spatialBlend = ReadFloat(j, "spatialBlend", 0.0f);
    out.minDistance = ReadFloat(j, "minDistance", 1.0f);
    out.maxDistance = ReadFloat(j, "maxDistance", 50.0f);
    out.rolloff = StrToRolloff(j.value("rolloff", std::string("logarithmic")));
    out.dopplerScale = ReadFloat(j, "dopplerScale", 1.0f);
    out.reverbSend = ReadFloat(j, "reverbSend", 0.0f);
    out.loopStartSample = j.value("loopStartSample", 0);
    out.loopEndSample = j.value("loopEndSample", 0);
    return true;
}

// ==== 純関数 ====

int PickVariationIndex(const SoundAsset& s, uint32_t roll)
{
    int64_t total = 0;
    for (const SoundVariation& v : s.variations) {
        if (v.clip != 0 && v.weight > 0) {
            total += v.weight;
        }
    }
    if (total <= 0) {
        return -1;
    }
    int64_t pick = static_cast<int64_t>(roll % static_cast<uint32_t>(total));
    for (size_t i = 0; i < s.variations.size(); ++i) {
        const SoundVariation& v = s.variations[i];
        if (v.clip == 0 || v.weight <= 0) {
            continue;
        }
        pick -= v.weight;
        if (pick < 0) {
            return static_cast<int>(i);
        }
    }
    return -1; // ここには来ない (total の走査と同じ条件で回している)
}

PlayDesc MakePlayDesc(const SoundAsset& s, int variationIndex, float volJitter, float pitchJitter,
                      const AudioSystem& audio)
{
    PlayDesc d;
    if (variationIndex >= 0 && static_cast<size_t>(variationIndex) < s.variations.size()) {
        d.clip = AssetID{ s.variations[static_cast<size_t>(variationIndex)].clip };
    }
    const int bus = audio.FindBus(s.bus.c_str());
    d.bus = (bus >= 0) ? bus : audio.DefaultBus();
    d.volume = std::clamp(s.volume + volJitter * s.volumeRandom, 0.0f, 1.0f);
    d.pitch = std::clamp(s.pitch + pitchJitter * s.pitchRandom, 1.0f / AudioSystem::kMaxFreqRatio,
                         AudioSystem::kMaxFreqRatio);
    d.loop = s.loop;
    d.priority = s.priority;
    return d;
}

namespace {

// 先頭の「クリップが割り当たっているバリエーション」。試聴と BGM が共有する規則
int FirstAssignedVariation(const SoundAsset& s)
{
    for (size_t i = 0; i < s.variations.size(); ++i) {
        if (s.variations[i].clip != 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

bool PlayMusicSound(AudioSystem& audio, const SoundAsset& s, float fadeSeconds)
{
    const int index = FirstAssignedVariation(s);
    if (index < 0) {
        MYE_LOG_WARN("[music] no clip assigned: %s", s.name.c_str());
        return false;
    }
    const uint64_t clip = s.variations[static_cast<size_t>(index)].clip;
    // ★BGM はクリップ表を経由しない。GUID から実ファイルを引いて、そこから直接読む
    const std::wstring path = assetguid::ResolvePath(clip);
    if (path.empty()) {
        MYE_LOG_WARN("[music] clip path not found (guid %016llx): %s",
                     static_cast<unsigned long long>(clip), s.name.c_str());
        return false;
    }

    MusicDesc d;
    d.path = path;
    // 同一判定はアセット単位 (同じ .sound.json を再指定しても頭出しし直さない)
    d.key = s.hash != 0 ? s.hash : clip;
    const int bus = audio.FindBus(s.bus.c_str());
    d.bus = (bus >= 0) ? bus : audio.DefaultBus();
    d.volume = std::clamp(s.volume, 0.0f, 1.0f); // BGM に音量の揺らぎは掛けない
    d.fadeSeconds = fadeSeconds;
    d.loop = s.loop;
    d.loopStartFrame = s.loopStartSample;
    d.loopEndFrame = s.loopEndSample;
    return audio.PlayMusic(d);
}

AudioHandle PreviewSound(AudioSystem& audio, const SoundAsset& s)
{
    // stream = BGM はボイスプールに載せずストリーミングレーンで鳴らす (M45f)。
    // 2 つ目の BGM アセットを続けて試聴すると、そのままクロスフェードが聴ける
    if (s.stream) {
        PlayMusicSound(audio, s, kMusicDefaultFadeSeconds);
        return {}; // BGM レーンにボイスハンドルは無い
    }
    // 試聴は「先頭バリエーション・揺らぎ無し」で固定する (押すたびに音が変わると
    // パラメータを詰めている最中に何を聴いているのか分からなくなる)
    const int index = FirstAssignedVariation(s);
    if (index < 0) {
        MYE_LOG_WARN("[sound] no clip assigned: %s", s.name.c_str());
        return {};
    }
    const AssetID clip{ s.variations[static_cast<size_t>(index)].clip };
    if (!audio.HasClip(clip)) {
        // 起動後に追加された .wav / .ogg はまだ表に無い。GUID からパスを引いてロードする
        const std::wstring path = assetguid::ResolvePath(clip.value);
        if (path.empty() || audio.LoadClipFile(path).IsNull()) {
            MYE_LOG_WARN("[sound] clip not loaded (guid %016llx): %s",
                         static_cast<unsigned long long>(clip.value), s.name.c_str());
            return {};
        }
    }
    return audio.Play(MakePlayDesc(s, index, 0.0f, 0.0f, audio));
}

} // namespace mye
