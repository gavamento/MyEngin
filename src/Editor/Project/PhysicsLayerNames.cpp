#include "Editor/Project/PhysicsLayerNames.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"

#include "nlohmann/json.hpp"

namespace mye {

using json = nlohmann::json;

PhysicsLayerNames& PhysicsLayerNames::Get()
{
    static PhysicsLayerNames instance;
    return instance;
}

void PhysicsLayerNames::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    for (int i = 0; i < kCount; ++i) {
        std::snprintf(names_[i], sizeof(names_[i]), "Layer %d", i);
    }
    std::ifstream f(std::filesystem::path(assetsRoot + L"\\project_settings.json"));
    if (!f) {
        return;
    }
    try {
        json j;
        f >> j;
        if (j.contains("physicsLayers") && j["physicsLayers"].is_array()) {
            const auto& arr = j["physicsLayers"];
            for (int i = 0; i < kCount && i < static_cast<int>(arr.size()); ++i) {
                if (arr[i].is_string()) {
                    const std::string s = arr[i].get<std::string>();
                    if (!s.empty()) {
                        std::snprintf(names_[i], sizeof(names_[i]), "%s", s.c_str());
                    }
                }
            }
        }
    } catch (const json::exception& e) {
        MYE_LOG_WARN("[layers] project_settings.json parse failed: %s", e.what());
    }
}

bool PhysicsLayerNames::Save(const std::wstring& assetsRoot) const
{
    json arr = json::array();
    for (int i = 0; i < kCount; ++i) {
        arr.push_back(std::string(names_[i]));
    }
    return UpdateProjectSettingsFile(ProjectSettingsPath(assetsRoot),
                                     [&arr](json& j) { j["physicsLayers"] = std::move(arr); });
}

bool PhysicsLayerNames::DiffersFromDisk() const
{
    if (loadedRoot_.empty()) {
        return false; // 一度も読んでいない = この窓では何も編集していない
    }
    PhysicsLayerNames onDisk;
    onDisk.Load(loadedRoot_, true);
    for (int i = 0; i < kCount; ++i) {
        if (std::strcmp(names_[i], onDisk.names_[i]) != 0) {
            return true;
        }
    }
    return false;
}

const char* PhysicsLayerNames::Name(int i) const
{
    if (i < 0 || i >= kCount) {
        return "(out of range)";
    }
    return names_[i];
}

void PhysicsLayerNames::BuildComboLabels(const char* out[kCount]) const
{
    for (int i = 0; i < kCount; ++i) {
        out[i] = names_[i];
    }
}

} // namespace mye
