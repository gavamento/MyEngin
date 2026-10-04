//====================================================================================
//                          NavAreaNames.cpp
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          NavMesh エリア名の読み書き
//====================================================================================
#include "Editor/Project/NavAreaNames.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Diagnostics/Log.h"

#include "nlohmann/json.hpp"

namespace mye {

using json = nlohmann::json;

namespace {

void SetDefaults(char (&names)[NavAreaNames::kCount][NavAreaNames::kNameCapacity])
{
    std::snprintf(names[0], sizeof(names[0]), "Walkable");
    std::snprintf(names[1], sizeof(names[1]), "Not Walkable");
    std::snprintf(names[2], sizeof(names[2]), "Jump");
    for (int i = NavAreaNames::kFixedCount; i < NavAreaNames::kCount; ++i) {
        std::snprintf(names[i], sizeof(names[i]), "Area %d", i);
    }
}

} // namespace

NavAreaNames::NavAreaNames()
{
    SetDefaults(names_);
}

NavAreaNames& NavAreaNames::Get()
{
    static NavAreaNames instance;
    return instance;
}

void NavAreaNames::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    SetDefaults(names_);
    std::ifstream f(std::filesystem::path(assetsRoot + L"\\project_settings.json"));
    if (!f) {
        return;
    }
    try {
        json j;
        f >> j;
        if (j.contains("navAreas") && j["navAreas"].is_array()) {
            const auto& arr = j["navAreas"];
            for (int i = kFixedCount; i < kCount && i < static_cast<int>(arr.size()); ++i) {
                if (arr[i].is_string()) {
                    const std::string s = arr[i].get<std::string>();
                    if (!s.empty()) {
                        std::snprintf(names_[i], sizeof(names_[i]), "%s", s.c_str());
                    }
                }
            }
        }
    } catch (const json::exception& e) {
        MYE_LOG_WARN("[nav] project_settings.json parse failed: %s", e.what());
    }
}

bool NavAreaNames::Save(const std::wstring& assetsRoot) const
{
    const std::filesystem::path path(assetsRoot + L"\\project_settings.json");
    json j = json::object();
    {
        // 既存キー (物理レイヤー名・粒子設定等) を保存で破壊しない read-modify-write
        std::ifstream f(path);
        if (f) {
            try {
                f >> j;
            } catch (const json::exception&) {
                j = json::object();
            }
        }
    }
    json arr = json::array();
    for (int i = 0; i < kCount; ++i) {
        arr.push_back(std::string(names_[i]));
    }
    j["navAreas"] = arr;
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

bool NavAreaNames::DiffersFromDisk() const
{
    if (loadedRoot_.empty()) {
        return false;
    }
    NavAreaNames onDisk;
    onDisk.Load(loadedRoot_, true);
    for (int i = kFixedCount; i < kCount; ++i) {
        if (std::strcmp(names_[i], onDisk.names_[i]) != 0) {
            return true;
        }
    }
    return false;
}

const char* NavAreaNames::Name(int i) const
{
    if (i < 0 || i >= kCount) {
        return "(out of range)";
    }
    return names_[i];
}

} // namespace mye
