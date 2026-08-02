#include "Engine/Engine/Audio/AudioMixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "Engine/Core/AssetKeyResolver.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

// I3DL2 プリセット名。**AudioSystem.cpp の実パラメータ表と同じ並び**でなければならない
// (向こうに static_assert を置いて件数のズレを検出している)
const char* const kReverbPresetNames[kReverbPresetCount] = {
    "Default", "Generic", "Room",      "SmallRoom", "MediumRoom", "LargeRoom",
    "Hall",    "Cave",    "Underwater", "Arena",    "Plate",
};

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.mixer.json" → "X.mixer"
    const std::string suf = ".mixer";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

char LowerChar(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(const std::string& a, const char* b)
{
    if (b == nullptr) {
        return false;
    }
    size_t i = 0;
    for (; i < a.size(); ++i) {
        if (b[i] == '\0' || LowerChar(a[i]) != LowerChar(b[i])) {
            return false;
        }
    }
    return b[i] == '\0';
}

// 手編集された .mixer.json で true/false が 1/0 と書かれていても拾う
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

// ==== プリセット名 ====

const char* ReverbPresetName(int index)
{
    if (index < 0 || index >= kReverbPresetCount) {
        return kReverbPresetNames[0];
    }
    return kReverbPresetNames[index];
}

int ReverbPresetIndex(const char* name)
{
    if (name == nullptr) {
        return 0;
    }
    for (int i = 0; i < kReverbPresetCount; ++i) {
        if (EqualNoCase(std::string(kReverbPresetNames[i]), name)) {
            return i;
        }
    }
    return 0;
}

// ==== MixerLibrary ====

uint64_t MixerLibrary::HashForPath(const std::wstring& path)
{
    // M30c: 移動/リネーム済みアセットは .meta の GUID がキーになる (未移動は path-hash と同値)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t MixerLibrary::Register(const std::wstring& path, MixerAsset asset)
{
    const uint64_t hash = HashForPath(path);
    asset.hash = hash;
    asset.path = path;
    if (asset.name.empty()) {
        asset.name = NameFromPath(path);
    }
    mixers_[hash] = std::move(asset);
    return hash;
}

uint64_t MixerLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[mixer] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    MixerAsset m;
    if (!FromJson(j, m)) {
        return 0;
    }
    m.name = NameFromPath(path);
    // 壊れたトポロジは**読み込み自体を拒否する** — 中途半端に適用すると
    // 「どのバスに音が流れているのか」が説明できない状態になる
    std::string err;
    if (!ValidateMixer(m, &err)) {
        MYE_LOG_WARN("[mixer] invalid topology in %s: %s", WideToUtf8(path).c_str(), err.c_str());
        return 0;
    }
    return Register(path, std::move(m));
}

bool MixerLibrary::SaveToFile(uint64_t hash) const
{
    const MixerAsset* m = Get(hash);
    if (m == nullptr) {
        return false;
    }
    std::ofstream f(fs::path(m->path), std::ios::binary);
    if (!f) {
        return false;
    }
    f << ToJson(*m).dump(2);
    return true;
}

const MixerAsset* MixerLibrary::Get(uint64_t hash) const
{
    auto it = mixers_.find(hash);
    return it == mixers_.end() ? nullptr : &it->second;
}

MixerAsset* MixerLibrary::GetMutable(uint64_t hash)
{
    auto it = mixers_.find(hash);
    return it == mixers_.end() ? nullptr : &it->second;
}

std::vector<MixerEntry> MixerLibrary::Enumerate() const
{
    std::vector<MixerEntry> out;
    out.reserve(mixers_.size());
    for (auto it = mixers_.begin(); it != mixers_.end(); ++it) {
        out.push_back(MixerEntry{ it->first, it->second.name });
    }
    // 明示キーで並べる (spec 11.2 規則 7: ハッシュの反復順を表に出さない)
    std::sort(out.begin(), out.end(), [](const MixerEntry& a, const MixerEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

uint64_t MixerLibrary::PickStartupMixer() const
{
    const std::vector<MixerEntry> all = Enumerate();
    for (const MixerEntry& e : all) {
        if (EqualNoCase(e.name, "default")) {
            return e.hash;
        }
    }
    return all.empty() ? 0 : all.front().hash;
}

json MixerLibrary::ToJson(const MixerAsset& m)
{
    json root;
    root["engine"] = "MyEngine";
    root["mixer"] = 1;
    root["name"] = m.name;
    json buses = json::array();
    for (const MixerBus& b : m.buses) {
        buses.push_back({ { "name", b.name },
                          { "parent", b.parent },
                          { "volumeDb", b.volumeDb },
                          { "mute", b.mute },
                          { "solo", b.solo },
                          { "reverbSend", b.reverbSend } });
    }
    root["buses"] = std::move(buses);
    root["reverbPreset"] = m.reverbPreset;
    root["reverbWetDryMix"] = m.reverbWetDryMix;
    return root;
}

bool MixerLibrary::FromJson(const json& j, MixerAsset& out)
{
    if (!j.is_object()) {
        return false;
    }
    out.buses.clear();
    if (j.contains("buses") && j["buses"].is_array()) {
        for (const json& bj : j["buses"]) {
            if (!bj.is_object()) {
                continue;
            }
            MixerBus b;
            b.name = bj.value("name", std::string());
            b.parent = bj.value("parent", std::string());
            b.volumeDb = std::clamp(ReadFloat(bj, "volumeDb", 0.0f), kMinDb, kMaxBusDb);
            b.mute = ReadBool(bj, "mute", false);
            b.solo = ReadBool(bj, "solo", false);
            b.reverbSend = std::clamp(ReadFloat(bj, "reverbSend", 0.0f), 0.0f, 1.0f);
            out.buses.push_back(std::move(b));
        }
    }
    out.name = j.value("name", std::string());
    out.reverbPreset = j.value("reverbPreset", std::string("Default"));
    out.reverbWetDryMix = std::clamp(ReadFloat(j, "reverbWetDryMix", 100.0f), 0.0f, 100.0f);
    return true;
}

// ==== 純関数 ====

int FindMixerBus(const MixerAsset& m, const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < m.buses.size(); ++i) {
        if (EqualNoCase(m.buses[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<int> MixerBusParents(const MixerAsset& m)
{
    std::vector<int> parents(m.buses.size(), -1);
    for (size_t i = 0; i < m.buses.size(); ++i) {
        const std::string& p = m.buses[i].parent;
        if (p.empty()) {
            continue; // ルート
        }
        const int idx = FindMixerBus(m, p.c_str());
        // 自分を親に指定した場合も「解決できない」扱い (深さ計算の循環判定へ回さない)
        parents[i] = (idx < 0 || idx == static_cast<int>(i)) ? -2 : idx;
    }
    return parents;
}

std::vector<int> MixerBusDepths(const MixerAsset& m)
{
    const std::vector<int> parents = MixerBusParents(m);
    const size_t n = m.buses.size();
    std::vector<int> depth(n, -1);
    for (size_t i = 0; i < n; ++i) {
        int cur = static_cast<int>(i);
        int d = 0;
        bool ok = true;
        while (parents[static_cast<size_t>(cur)] >= 0) {
            cur = parents[static_cast<size_t>(cur)];
            ++d;
            if (d > static_cast<int>(n)) { // 親を n 回超えて辿れたら循環している
                ok = false;
                break;
            }
        }
        if (!ok || parents[static_cast<size_t>(cur)] == -2) {
            continue; // 循環 or 孤児に辿り着いた → 深さ未定義
        }
        depth[i] = d;
    }
    return depth;
}

bool ValidateMixer(const MixerAsset& m, std::string* error)
{
    auto fail = [error](std::string msg) {
        if (error != nullptr) {
            *error = std::move(msg);
        }
        return false;
    };
    if (m.buses.empty()) {
        return fail("no buses");
    }
    int roots = 0;
    for (size_t i = 0; i < m.buses.size(); ++i) {
        const MixerBus& b = m.buses[i];
        if (b.name.empty()) {
            return fail("bus " + std::to_string(i) + " has an empty name");
        }
        if (FindMixerBus(m, b.name.c_str()) != static_cast<int>(i)) {
            return fail("duplicate bus name: " + b.name);
        }
        if (b.parent.empty()) {
            ++roots;
        }
    }
    if (roots != 1) {
        return fail("expected exactly one root bus, found " + std::to_string(roots));
    }
    const std::vector<int> parents = MixerBusParents(m);
    for (size_t i = 0; i < parents.size(); ++i) {
        if (parents[i] == -2) {
            return fail("bus '" + m.buses[i].name + "' has an unknown parent '"
                        + m.buses[i].parent + "'");
        }
    }
    const std::vector<int> depth = MixerBusDepths(m);
    for (size_t i = 0; i < depth.size(); ++i) {
        if (depth[i] < 0) {
            return fail("bus '" + m.buses[i].name + "' is part of a parent cycle");
        }
    }
    return true;
}

std::vector<uint8_t> SoloEffectiveMutes(const std::vector<int>& parents,
                                       const std::vector<uint8_t>& mute,
                                       const std::vector<uint8_t>& solo)
{
    const size_t n = parents.size();
    std::vector<uint8_t> mutes(n, 0);
    if (mute.size() != n || solo.size() != n) {
        return mutes; // 呼び出し側の組み立てミス。無音にはしない
    }
    bool anySolo = false;
    for (size_t i = 0; i < n; ++i) {
        anySolo = anySolo || solo[i] != 0;
    }
    // ソロ中に鳴らし続けるバス集合 = ソロバス自身 + 祖先 + 子孫
    std::vector<uint8_t> keep(n, anySolo ? 0u : 1u);
    if (anySolo) {
        for (size_t i = 0; i < n; ++i) {
            // 自分から根へ辿り、途中にソロがあれば「ソロの子孫」として残す
            int cur = static_cast<int>(i);
            for (size_t step = 0; step <= n; ++step) {
                if (solo[static_cast<size_t>(cur)] != 0) {
                    keep[i] = 1;
                    break;
                }
                if (parents[static_cast<size_t>(cur)] < 0) {
                    break;
                }
                cur = parents[static_cast<size_t>(cur)];
            }
            if (solo[i] == 0) {
                continue;
            }
            // 自分がソロなら祖先も残す (途中で切ると出力がマスターに届かない)
            cur = parents[i];
            for (size_t step = 0; cur >= 0 && step <= n; ++step) {
                keep[static_cast<size_t>(cur)] = 1;
                cur = parents[static_cast<size_t>(cur)];
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        mutes[i] = (mute[i] != 0 || keep[i] == 0) ? 1u : 0u;
    }
    return mutes;
}

std::vector<uint8_t> MixerEffectiveMutes(const MixerAsset& m)
{
    const size_t n = m.buses.size();
    std::vector<uint8_t> mute(n, 0);
    std::vector<uint8_t> solo(n, 0);
    for (size_t i = 0; i < n; ++i) {
        mute[i] = m.buses[i].mute ? 1u : 0u;
        solo[i] = m.buses[i].solo ? 1u : 0u;
    }
    return SoloEffectiveMutes(MixerBusParents(m), mute, solo);
}

MixerAsset DefaultMixer()
{
    MixerAsset m;
    m.name = "default";
    MixerBus master;
    master.name = "Master";
    m.buses.push_back(master);
    for (const char* child : { "BGM", "SE", "UI" }) {
        MixerBus b;
        b.name = child;
        b.parent = "Master";
        m.buses.push_back(std::move(b));
    }
    return m;
}

} // namespace mye
