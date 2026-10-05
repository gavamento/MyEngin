//====================================================================================
//                          BlackboardLibrary.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ブラックボード資産の読み書き
//====================================================================================
#include "Engine/Engine/AI/BlackboardLibrary.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Asset/AssetKeyResolver.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace fs = std::filesystem;

namespace mye {

using nlohmann::json;

namespace {

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.bb.json" -> "X.bb"
    const std::string suf = ".bb";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

// 初期値を型に合わせて読む。型に合わなければ「初期値なし」
BbValue ReadInitial(BbType type, const json& j)
{
    BbValue value;
    switch (type) {
    case BbType::Bool:
        if (j.is_boolean()) {
            value.isSet = 1;
            value.i = j.get<bool>() ? 1 : 0;
        }
        break;
    case BbType::Int:
        if (j.is_number_integer()) {
            const int64_t v = j.get<int64_t>();
            value.isSet = 1;
            value.i = static_cast<int32_t>((std::clamp)(v, static_cast<int64_t>(INT32_MIN), static_cast<int64_t>(INT32_MAX)));
        }
        break;
    case BbType::Float:
        if (j.is_number() && std::isfinite(j.get<float>())) {
            value.isSet = 1;
            value.f = j.get<float>();
        }
        break;
    case BbType::Vector:
        if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number()) {
            const float x = j[0].get<float>();
            const float y = j[1].get<float>();
            const float z = j[2].get<float>();
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
                value.isSet = 1;
                value.v[0] = x;
                value.v[1] = y;
                value.v[2] = z;
            }
        }
        break;
    case BbType::Entity:
        break; // エンティティはアセットから指せない
    }
    return value;
}

} // namespace

const char* BbTypeName(BbType type)
{
    switch (type) {
    case BbType::Bool: return "Bool";
    case BbType::Int: return "Int";
    case BbType::Float: return "Float";
    case BbType::Vector: return "Vector";
    case BbType::Entity: return "Entity";
    }
    return "Bool";
}

bool BbTypeFromName(const std::string& name, BbType& out)
{
    constexpr BbType kAll[] = { BbType::Bool, BbType::Int, BbType::Float, BbType::Vector, BbType::Entity };
    for (const BbType type : kAll) {
        if (name == BbTypeName(type)) {
            out = type;
            return true;
        }
    }
    return false;
}

int BlackboardAsset::FindKey(const std::string& keyName) const
{
    for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].name == keyName) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint64_t BlackboardLibrary::HashForPath(const std::wstring& path)
{
    // 移動 / リネーム済みの資産は .meta の GUID がキー (NavFilterLibrary と同じ)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t BlackboardLibrary::Register(const std::wstring& path, BlackboardAsset asset)
{
    const uint64_t hash = HashForPath(path);
    asset.hash = hash;
    asset.path = path;
    if (asset.name.empty()) {
        asset.name = NameFromPath(path);
    }
    assets_[hash] = std::make_shared<const BlackboardAsset>(std::move(asset));
    return hash;
}

uint64_t BlackboardLibrary::LoadFromFile(const std::wstring& path, bool* outUnchanged)
{
    if (outUnchanged != nullptr) {
        *outUnchanged = false;
    }
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[blackboard] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    BlackboardAsset asset;
    if (!FromJson(j, asset)) {
        MYE_LOG_WARN("[blackboard] not a valid blackboard: %s", WideToUtf8(path).c_str());
        return 0;
    }
    asset.name = NameFromPath(path);
    if (outUnchanged != nullptr) {
        const uint64_t hash = HashForPath(path);
        const auto it = assets_.find(hash);
        if (it != assets_.end() && NormalizePathKey(it->second->path) == NormalizePathKey(path)) {
            // ReloadHub は正規化 (小文字) 済みのパスで読むので、名前の大文字小文字の違いは同じ内容とみなす
            nlohmann::json existing = ToJson(*it->second);
            nlohmann::json fresh = ToJson(asset);
            existing["name"] = fresh["name"];
            if (existing == fresh) {
                *outUnchanged = true;
                return hash;
            }
        }
    }
    return Register(path, std::move(asset));
}

std::shared_ptr<const BlackboardAsset> BlackboardLibrary::GetShared(uint64_t hash) const
{
    const auto it = assets_.find(hash);
    return it != assets_.end() ? it->second : nullptr;
}

const BlackboardAsset* BlackboardLibrary::Get(uint64_t hash) const
{
    const auto it = assets_.find(hash);
    return it != assets_.end() ? it->second.get() : nullptr;
}

std::vector<BlackboardEntry> BlackboardLibrary::Enumerate() const
{
    std::vector<BlackboardEntry> out;
    out.reserve(assets_.size());
    for (auto it = assets_.begin(); it != assets_.end(); ++it) {
        out.push_back(BlackboardEntry{ it->first, it->second->name });
    }
    std::sort(out.begin(), out.end(), [](const BlackboardEntry& a, const BlackboardEntry& b) {
        return a.name != b.name ? a.name < b.name : a.hash < b.hash;
    });
    return out;
}

json BlackboardLibrary::ToJson(const BlackboardAsset& asset)
{
    json j;
    j["engine"] = "MyEngine";
    j["blackboard"] = 1;
    j["name"] = asset.name;
    json keys = json::array();
    for (const BbKeyDef& key : asset.keys) {
        json k;
        k["name"] = key.name;
        k["type"] = BbTypeName(key.type);
        if (key.initial.isSet != 0) {
            switch (key.type) {
            case BbType::Bool: k["initial"] = key.initial.i != 0; break;
            case BbType::Int: k["initial"] = key.initial.i; break;
            case BbType::Float: k["initial"] = key.initial.f; break;
            case BbType::Vector: k["initial"] = json::array({ key.initial.v[0], key.initial.v[1], key.initial.v[2] }); break;
            case BbType::Entity: break;
            }
        }
        if (!key.eventName.empty()) {
            k["eventName"] = key.eventName;
        }
        keys.push_back(std::move(k));
    }
    j["keys"] = std::move(keys);
    return j;
}

bool BlackboardLibrary::FromJson(const json& j, BlackboardAsset& out)
{
    if (!j.is_object() || !j.contains("blackboard")) {
        return false; // 種別キー必須 (別の JSON 資産を黙ってブラックボードに読まない)
    }
    out.keys.clear();
    if (j.contains("name") && j["name"].is_string()) {
        out.name = j["name"].get<std::string>();
    }
    if (!j.contains("keys")) {
        return true;
    }
    const json& keys = j["keys"];
    if (!keys.is_array() || keys.size() > static_cast<size_t>(kBbMaxKeys)) {
        return false;
    }
    for (const json& k : keys) {
        if (!k.is_object() || !k.contains("name") || !k["name"].is_string() || !k.contains("type")
            || !k["type"].is_string()) {
            return false;
        }
        BbKeyDef def;
        def.name = k["name"].get<std::string>();
        if (def.name.empty() || def.name.size() > kBbMaxNameBytes || out.FindKey(def.name) >= 0) {
            return false; // 空・長すぎ・重複は ABI の名前ハッシュ引きが曖昧になる
        }
        if (!BbTypeFromName(k["type"].get<std::string>(), def.type)) {
            return false;
        }
        if (k.contains("initial")) {
            def.initial = ReadInitial(def.type, k["initial"]);
        }
        if (k.contains("eventName") && k["eventName"].is_string()) {
            def.eventName = k["eventName"].get<std::string>();
            if (def.eventName.size() > kBbMaxNameBytes) {
                return false;
            }
        }
        out.keys.push_back(std::move(def));
    }
    return true;
}

namespace blackboard {

namespace {
BlackboardLibrary* sLibrary = nullptr;
} // namespace

void Install(BlackboardLibrary* lib)
{
    sLibrary = lib;
}

BlackboardLibrary* Library()
{
    return sLibrary;
}

} // namespace blackboard

} // namespace mye
