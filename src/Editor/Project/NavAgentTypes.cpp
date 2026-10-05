//====================================================================================
//                          NavAgentTypes.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          NavMesh の Agent Type の表の読み書き
//====================================================================================
#include "Editor/Project/NavAgentTypes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"

#include "nlohmann/json.hpp"

namespace mye {

using json = nlohmann::json;

namespace {

// Surface の既定値 (Components.h) と揃える
NavAgentType MakeHumanoid()
{
    NavAgentType t;
    t.id = 0;
    std::snprintf(t.name, sizeof(t.name), "Humanoid");
    return t;
}

// 寸法を Inspector の範囲 (Components.cpp の NavMeshSurface 登録) に収める
void ClampType(NavAgentType& t)
{
    t.radius = std::clamp(t.radius, 0.05f, 5.0f);
    t.height = std::clamp(t.height, 0.2f, 10.0f);
    t.maxClimb = std::clamp(t.maxClimb, 0.0f, 5.0f);
    t.maxSlopeDeg = std::clamp(t.maxSlopeDeg, 0.0f, 89.0f);
}

bool SameType(const NavAgentType& a, const NavAgentType& b)
{
    return a.id == b.id && std::strcmp(a.name, b.name) == 0 && a.radius == b.radius && a.height == b.height
        && a.maxClimb == b.maxClimb && a.maxSlopeDeg == b.maxSlopeDeg;
}

} // namespace

NavAgentTypes::NavAgentTypes()
{
    types_.push_back(MakeHumanoid());
}

NavAgentTypes& NavAgentTypes::Get()
{
    static NavAgentTypes instance;
    return instance;
}

void NavAgentTypes::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    types_.clear();
    std::ifstream f(std::filesystem::path(assetsRoot + L"\\project_settings.json"));
    if (f) {
        try {
            json j;
            f >> j;
            if (j.contains("navAgentTypes") && j["navAgentTypes"].is_array()) {
                for (const auto& e : j["navAgentTypes"]) {
                    if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer()
                        || static_cast<int>(types_.size()) >= kMaxTypes) {
                        continue;
                    }
                    NavAgentType t;
                    t.id = e["id"].get<int>();
                    if (t.id < 0 || IndexOf(t.id) >= 0) {
                        continue; // 負・重複の id は捨てる (先勝ち)
                    }
                    const std::string name = e.value("name", std::string());
                    std::snprintf(t.name, sizeof(t.name), "%s", name.c_str());
                    t.radius = e.value("radius", t.radius);
                    t.height = e.value("height", t.height);
                    t.maxClimb = e.value("maxClimb", t.maxClimb);
                    t.maxSlopeDeg = e.value("maxSlopeDeg", t.maxSlopeDeg);
                    ClampType(t);
                    types_.push_back(t);
                }
            }
        } catch (const json::exception& e) {
            MYE_LOG_WARN("[nav] project_settings.json parse failed: %s", e.what());
            types_.clear();
        }
    }
    if (IndexOf(0) < 0) {
        types_.insert(types_.begin(), MakeHumanoid());
        if (static_cast<int>(types_.size()) > kMaxTypes) {
            types_.resize(kMaxTypes);
        }
    }
}

bool NavAgentTypes::Save(const std::wstring& assetsRoot) const
{
    const std::filesystem::path path(assetsRoot + L"\\project_settings.json");
    json j = json::object();
    {
        // 既存キー (物理レイヤー名・エリア名等) を保存で破壊しない read-modify-write
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
    for (const NavAgentType& t : types_) {
        arr.push_back({ { "id", t.id },
                        { "name", std::string(t.name) },
                        { "radius", t.radius },
                        { "height", t.height },
                        { "maxClimb", t.maxClimb },
                        { "maxSlopeDeg", t.maxSlopeDeg } });
    }
    j["navAgentTypes"] = arr;
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

bool NavAgentTypes::DiffersFromDisk() const
{
    if (loadedRoot_.empty()) {
        return false;
    }
    NavAgentTypes onDisk;
    onDisk.Load(loadedRoot_, true);
    if (onDisk.types_.size() != types_.size()) {
        return true;
    }
    for (size_t i = 0; i < types_.size(); ++i) {
        // JSON の往復で値が変わらないよう、比べる前に同じ丸めを通す
        NavAgentType edited = types_[i];
        ClampType(edited);
        if (!SameType(edited, onDisk.types_[i])) {
            return true;
        }
    }
    return false;
}

const NavAgentType* NavAgentTypes::Find(int id) const
{
    const int index = IndexOf(id);
    return index >= 0 ? &types_[index] : nullptr;
}

int NavAgentTypes::IndexOf(int id) const
{
    for (size_t i = 0; i < types_.size(); ++i) {
        if (types_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int NavAgentTypes::Add()
{
    if (static_cast<int>(types_.size()) >= kMaxTypes) {
        return -1;
    }
    int maxId = 0;
    for (const NavAgentType& t : types_) {
        maxId = (std::max)(maxId, t.id);
    }
    NavAgentType t;
    t.id = maxId + 1;
    std::snprintf(t.name, sizeof(t.name), "Agent %d", t.id);
    types_.push_back(t);
    return static_cast<int>(types_.size()) - 1;
}

bool NavAgentTypes::Remove(int index)
{
    if (index < 0 || index >= static_cast<int>(types_.size()) || types_[index].id == 0) {
        return false;
    }
    types_.erase(types_.begin() + index);
    return true;
}

bool NavSurfaceMatchesAgentType(const NavMeshSurfaceComponent& surface, const NavAgentType& type)
{
    return surface.agentRadius == type.radius && surface.agentHeight == type.height
        && surface.maxClimb == type.maxClimb && surface.maxSlopeDeg == type.maxSlopeDeg;
}

void NavApplyAgentType(NavMeshSurfaceComponent& surface, const NavAgentType& type)
{
    surface.agentRadius = type.radius;
    surface.agentHeight = type.height;
    surface.maxClimb = type.maxClimb;
    surface.maxSlopeDeg = type.maxSlopeDeg;
}

} // namespace mye
