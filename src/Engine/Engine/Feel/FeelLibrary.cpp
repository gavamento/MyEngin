//====================================================================================
//                          FeelLibrary.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りのプリセット資産 (.feel.json、M94d、ADR-031) の読み書きと登録の実装
//====================================================================================
#include "Engine/Engine/Feel/FeelLibrary.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Hash.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

constexpr int kFeelFileVersion = 1;

float ReadFloat(const json& j, const char* key, float def)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<float>();
}

int32_t ReadInt(const json& j, const char* key, int32_t def)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_number()) {
        return def;
    }
    return j[key].get<int32_t>();
}

int32_t ReadEase(const json& j, const char* key, int32_t def)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) {
        return def;
    }
    const std::string name = j[key].get<std::string>();
    const int32_t e = MyeEaseFromName(name.c_str());
    if (e < 0) {
        MYE_LOG_WARN("[feel] unknown ease '%s' (Linear is used)", name.c_str());
        return MYE_EASE_LINEAR;
    }
    return e;
}

const json* Section(const json& j, const char* key)
{
    if (!j.contains(key) || !j[key].is_object()) {
        return nullptr;
    }
    return &j[key];
}

void ReadFloats(const json& j, const char* key, float* out, int n)
{
    if (!j.is_object() || !j.contains(key) || !j[key].is_array()) {
        return;
    }
    const json& a = j[key];
    for (int i = 0; i < n && i < static_cast<int>(a.size()); ++i) {
        if (a[i].is_number()) {
            out[i] = a[i].get<float>();
        }
    }
}

} // namespace

uint64_t FeelLibrary::HashForPath(const std::wstring& path)
{
    // PhysMatLibrary::HashForPath と同じ: .meta の GUID (未移動なら正規化パスのハッシュ)
    return assetkey::Resolve(NormalizePathKey(path));
}

std::string FeelLibrary::NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.feel.json" → "X.feel"
    const std::string suf = ".feel";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

uint64_t FeelLibrary::Register(const std::wstring& path, const MyeFeelDesc& desc)
{
    const uint64_t hash = HashForPath(path);
    FeelPreset p;
    p.hash = hash;
    p.name = NameFromPath(path);
    p.nameHash = HashStr(p.name);
    p.path = path;
    p.desc = desc;
    SanitizeFeelDesc(p.desc);
    presets_[hash] = std::move(p);
    return hash;
}

uint64_t FeelLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[feel] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    MyeFeelDesc d;
    if (!FromJson(j, d)) {
        MYE_LOG_WARN("[feel] not a feel preset: %s", WideToUtf8(path).c_str());
        return 0;
    }
    return Register(path, d);
}

const FeelPreset* FeelLibrary::Get(uint64_t hash) const
{
    const auto it = presets_.find(hash);
    return it != presets_.end() ? &it->second : nullptr;
}

const FeelPreset* FeelLibrary::FindByName(uint64_t nameHash) const
{
    const FeelPreset* best = nullptr;
    for (auto it = presets_.begin(); it != presets_.end(); ++it) {
        const FeelPreset& p = it->second;
        // ★走査順 (unordered_map) を結果に出さない: 同名ならパスの序数比較で最小を選ぶ (規則 7)
        if (p.nameHash == nameHash && (best == nullptr || p.path < best->path)) {
            best = &p;
        }
    }
    return best;
}

std::vector<FeelPresetEntry> FeelLibrary::Enumerate() const
{
    std::vector<FeelPresetEntry> out;
    out.reserve(presets_.size());
    for (auto it = presets_.begin(); it != presets_.end(); ++it) {
        out.push_back(FeelPresetEntry{ it->first, it->second.name });
    }
    std::sort(out.begin(), out.end(), [](const FeelPresetEntry& a, const FeelPresetEntry& b) {
        if (a.name != b.name) {
            return a.name < b.name;
        }
        return a.hash < b.hash;
    });
    return out;
}

json FeelLibrary::CurveToJson(const MyeCurve& c)
{
    json j;
    if (c.mode == MYE_CURVE_CUSTOM) {
        j["mode"] = "custom";
        json keys = json::array();
        for (int32_t i = 0; i < c.keyCount && i < MYE_CURVE_MAX_KEYS; ++i) {
            const MyeCurveKey& k = c.keys[i];
            keys.push_back(json::array({ k.t, k.value, k.inTangent, k.outTangent }));
        }
        j["keys"] = keys;
    } else {
        j["mode"] = "preset";
        j["ease"] = MyeEaseName(c.ease);
        j["from"] = c.from;
        j["to"] = c.to;
        if (c.param != 0.0f) {
            j["param"] = c.param;
        }
    }
    return j;
}

void FeelLibrary::CurveFromJson(const json& j, MyeCurve& out)
{
    if (!j.is_object()) {
        return;
    }
    const std::string mode = (j.contains("mode") && j["mode"].is_string()) ? j["mode"].get<std::string>() : "preset";
    if (mode == "custom") {
        out.mode = MYE_CURVE_CUSTOM;
        out.keyCount = 0;
        if (j.contains("keys") && j["keys"].is_array()) {
            for (const json& k : j["keys"]) {
                if (out.keyCount >= MYE_CURVE_MAX_KEYS) {
                    MYE_LOG_WARN("[feel] a curve has more than %d keys (the rest are ignored)", MYE_CURVE_MAX_KEYS);
                    break;
                }
                if (!k.is_array() || k.size() < 2) {
                    continue;
                }
                MyeCurveKey key;
                float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                for (size_t i = 0; i < 4 && i < k.size(); ++i) {
                    v[i] = k[i].is_number() ? k[i].get<float>() : 0.0f;
                }
                key.t = v[0];
                key.value = v[1];
                key.inTangent = v[2];
                key.outTangent = v[3];
                out.keys[out.keyCount++] = key;
            }
        }
        return;
    }
    out.mode = MYE_CURVE_PRESET;
    out.ease = ReadEase(j, "ease", out.ease);
    out.from = ReadFloat(j, "from", out.from);
    out.to = ReadFloat(j, "to", out.to);
    out.param = ReadFloat(j, "param", out.param);
}

json FeelLibrary::ToJson(const MyeFeelDesc& d)
{
    json j;
    j["engine"] = "MyEngine";
    j["feel"] = kFeelFileVersion;
    j["hitStop"] = { { "ticks", d.hitStopTicks } };
    j["slow"] = { { "percent", d.slowPercent },
                  { "holdTicks", d.slowHoldTicks },
                  { "recoverTicks", d.slowRecoverTicks },
                  { "recoverEase", MyeEaseName(d.slowRecoverEase) } };
    j["shake"] = { { "ticks", d.shakeTicks },
                   { "posAmp", d.shakePosAmp },
                   { "rotAmpDeg", d.shakeRotAmpDeg },
                   { "frequencyHz", d.shakeFrequencyHz },
                   { "envelope", CurveToJson(d.shakeEnvelope) } };
    j["kick"] = { { "ticks", d.kickTicks },
                  { "offset", json::array({ d.kickOffset.x, d.kickOffset.y, d.kickOffset.z }) },
                  { "pitchDeg", d.kickPitchDeg },
                  { "fovDeg", d.kickFovDeg },
                  { "curve", CurveToJson(d.kickCurve) } };
    j["rumble"] = { { "ticks", d.rumbleTicks },
                    { "low", d.rumbleLow },
                    { "high", d.rumbleHigh },
                    { "curve", CurveToJson(d.rumbleCurve) } };
    j["flash"] = { { "ticks", d.flashTicks },
                   { "vignette", d.flashVignette },
                   { "chromAberration", d.flashChromAberration },
                   { "exposure", d.flashExposure },
                   { "saturation", d.flashSaturation },
                   { "color", json::array({ d.flashColor.r, d.flashColor.g, d.flashColor.b, d.flashColor.a }) },
                   { "curve", CurveToJson(d.flashCurve) } };
    return j;
}

bool FeelLibrary::FromJson(const json& j, MyeFeelDesc& out)
{
    if (!j.is_object() || !j.contains("feel")) {
        return false;
    }
    MyeFeelDesc d;
    if (const json* s = Section(j, "hitStop")) {
        d.hitStopTicks = ReadInt(*s, "ticks", d.hitStopTicks);
    }
    if (const json* s = Section(j, "slow")) {
        d.slowPercent = ReadInt(*s, "percent", d.slowPercent);
        d.slowHoldTicks = ReadInt(*s, "holdTicks", d.slowHoldTicks);
        d.slowRecoverTicks = ReadInt(*s, "recoverTicks", d.slowRecoverTicks);
        d.slowRecoverEase = ReadEase(*s, "recoverEase", d.slowRecoverEase);
    }
    if (const json* s = Section(j, "shake")) {
        d.shakeTicks = ReadInt(*s, "ticks", d.shakeTicks);
        d.shakePosAmp = ReadFloat(*s, "posAmp", d.shakePosAmp);
        d.shakeRotAmpDeg = ReadFloat(*s, "rotAmpDeg", d.shakeRotAmpDeg);
        d.shakeFrequencyHz = ReadFloat(*s, "frequencyHz", d.shakeFrequencyHz);
        if (s->contains("envelope")) {
            CurveFromJson((*s)["envelope"], d.shakeEnvelope);
        }
    }
    if (const json* s = Section(j, "kick")) {
        d.kickTicks = ReadInt(*s, "ticks", d.kickTicks);
        float off[3] = { d.kickOffset.x, d.kickOffset.y, d.kickOffset.z };
        ReadFloats(*s, "offset", off, 3);
        d.kickOffset = { off[0], off[1], off[2] };
        d.kickPitchDeg = ReadFloat(*s, "pitchDeg", d.kickPitchDeg);
        d.kickFovDeg = ReadFloat(*s, "fovDeg", d.kickFovDeg);
        if (s->contains("curve")) {
            CurveFromJson((*s)["curve"], d.kickCurve);
        }
    }
    if (const json* s = Section(j, "rumble")) {
        d.rumbleTicks = ReadInt(*s, "ticks", d.rumbleTicks);
        d.rumbleLow = ReadFloat(*s, "low", d.rumbleLow);
        d.rumbleHigh = ReadFloat(*s, "high", d.rumbleHigh);
        if (s->contains("curve")) {
            CurveFromJson((*s)["curve"], d.rumbleCurve);
        }
    }
    if (const json* s = Section(j, "flash")) {
        d.flashTicks = ReadInt(*s, "ticks", d.flashTicks);
        d.flashVignette = ReadFloat(*s, "vignette", d.flashVignette);
        d.flashChromAberration = ReadFloat(*s, "chromAberration", d.flashChromAberration);
        d.flashExposure = ReadFloat(*s, "exposure", d.flashExposure);
        d.flashSaturation = ReadFloat(*s, "saturation", d.flashSaturation);
        float c[4] = { d.flashColor.r, d.flashColor.g, d.flashColor.b, d.flashColor.a };
        ReadFloats(*s, "color", c, 4);
        d.flashColor = { c[0], c[1], c[2], c[3] };
        if (s->contains("curve")) {
            CurveFromJson((*s)["curve"], d.flashCurve);
        }
    }
    SanitizeFeelDesc(d);
    out = d;
    return true;
}

// ==== feellib:: モジュール注入 ====

namespace feellib {

namespace {
FeelLibrary* sLibrary = nullptr;
} // namespace

void Install(FeelLibrary* lib)
{
    sLibrary = lib;
}

FeelLibrary* Library()
{
    return sLibrary;
}

const FeelPreset* Resolve(AssetID id)
{
    if (sLibrary == nullptr || id.IsNull()) {
        return nullptr;
    }
    return sLibrary->Get(id.value);
}

const FeelPreset* ResolveName(uint64_t nameHash)
{
    return sLibrary != nullptr ? sLibrary->FindByName(nameHash) : nullptr;
}

} // namespace feellib

} // namespace mye
