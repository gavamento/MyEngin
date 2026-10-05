//====================================================================================
//                          NavFilterLibrary.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          エリアのフィルタ資産の読み書き
//====================================================================================
#include "Engine/Engine/Navigation/NavFilterLibrary.h"

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

constexpr float kMaxAreaCost = 1000.0f; // Surface の areaCosts のインスペクタ上限と同じ
constexpr uint32_t kAllAreasMask = (1u << NavAreaFilter::kAreaCount) - 1u;

std::string NameFromPath(const std::wstring& path)
{
    std::string name = WideToUtf8(fs::path(path).stem().wstring()); // "X.navfilter.json" -> "X.navfilter"
    const std::string suf = ".navfilter";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        name.resize(name.size() - suf.size());
    }
    return name;
}

} // namespace

uint64_t NavFilterLibrary::HashForPath(const std::wstring& path)
{
    // 移動 / リネーム済みの資産は .meta の GUID がキー (PhysMatLibrary と同じ)
    return assetkey::Resolve(NormalizePathKey(path));
}

uint64_t NavFilterLibrary::Register(const std::wstring& path, NavAreaFilter filter)
{
    const uint64_t hash = HashForPath(path);
    filter.hash = hash;
    filter.path = path;
    if (filter.name.empty()) {
        filter.name = NameFromPath(path);
    }
    filters_[hash] = std::move(filter);
    return hash;
}

uint64_t NavFilterLibrary::LoadFromFile(const std::wstring& path)
{
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) {
        return 0;
    }
    json j;
    try {
        f >> j;
    } catch (const json::exception&) {
        MYE_LOG_WARN("[navfilter] JSON parse failed: %s", WideToUtf8(path).c_str());
        return 0;
    }
    NavAreaFilter filter;
    if (!FromJson(j, filter)) {
        MYE_LOG_WARN("[navfilter] not a navigation area filter: %s", WideToUtf8(path).c_str());
        return 0;
    }
    filter.name = NameFromPath(path);
    return Register(path, std::move(filter));
}

const NavAreaFilter* NavFilterLibrary::Get(uint64_t hash) const
{
    const auto it = filters_.find(hash);
    return it != filters_.end() ? &it->second : nullptr;
}

std::vector<NavFilterEntry> NavFilterLibrary::Enumerate() const
{
    std::vector<NavFilterEntry> out;
    out.reserve(filters_.size());
    for (auto it = filters_.begin(); it != filters_.end(); ++it) {
        out.push_back(NavFilterEntry{ it->first, it->second.name });
    }
    std::sort(out.begin(), out.end(), [](const NavFilterEntry& a, const NavFilterEntry& b) {
        return a.name != b.name ? a.name < b.name : a.hash < b.hash;
    });
    return out;
}

json NavFilterLibrary::ToJson(const NavAreaFilter& f)
{
    json j;
    j["engine"] = "MyEngine";
    j["navfilter"] = 1;
    j["name"] = f.name;
    json costs = json::array();
    for (int i = 0; i < NavAreaFilter::kAreaCount; ++i) {
        costs.push_back(f.areaCosts[i]);
    }
    j["areaCosts"] = costs;
    json excluded = json::array();
    for (int i = 0; i < NavAreaFilter::kAreaCount; ++i) {
        if ((f.excludedAreas >> i) & 1u) {
            excluded.push_back(i);
        }
    }
    j["excludedAreas"] = excluded;
    return j;
}

bool NavFilterLibrary::FromJson(const json& j, NavAreaFilter& out)
{
    if (!j.is_object() || !j.contains("navfilter")) {
        return false; // 種別キー必須 (別の JSON 資産を黙ってフィルタに読まない)
    }
    if (j.contains("name") && j["name"].is_string()) {
        out.name = j["name"].get<std::string>();
    }
    if (j.contains("areaCosts") && j["areaCosts"].is_array()) {
        const json& costs = j["areaCosts"];
        for (int i = 0; i < NavAreaFilter::kAreaCount && i < static_cast<int>(costs.size()); ++i) {
            out.areaCosts[i] = costs[i].is_number() ? costs[i].get<float>() : 0.0f;
        }
    }
    out.excludedAreas = 0;
    if (j.contains("excludedAreas") && j["excludedAreas"].is_array()) {
        for (const json& area : j["excludedAreas"]) {
            if (area.is_number_integer()) {
                const int i = area.get<int>();
                if (i >= 0 && i < NavAreaFilter::kAreaCount) {
                    out.excludedAreas |= 1u << i;
                }
            }
        }
    }
    Sanitize(out);
    return true;
}

void NavFilterLibrary::Sanitize(NavAreaFilter& f)
{
    for (float& cost : f.areaCosts) {
        // 0 以下・非有限は「上書きしない」。正の値は Surface と同じく 1 以上 (Detour の A* はコスト 1 未満を想定しない)
        cost = std::isfinite(cost) && cost > 0.0f ? std::clamp(cost, 1.0f, kMaxAreaCost) : 0.0f;
    }
    f.excludedAreas &= kAllAreasMask;
}

namespace navfilter {

namespace {
NavFilterLibrary* sLibrary = nullptr;
} // namespace

void Install(NavFilterLibrary* lib)
{
    sLibrary = lib;
}

NavFilterLibrary* Library()
{
    return sLibrary;
}

const NavAreaFilter* Resolve(uint64_t guid)
{
    if (sLibrary == nullptr || guid == 0) {
        return nullptr;
    }
    return sLibrary->Get(guid);
}

const NavAreaFilter* Resolve(AssetID id)
{
    return Resolve(id.value);
}

} // namespace navfilter

} // namespace mye
