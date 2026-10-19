//====================================================================================
//                          SortingLayers.cpp
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          ソート層の表の読み書きと編集
//====================================================================================
#include "Engine/Engine/Scene/SortingLayers.h"

#include <cstdio>
#include <cstring>

#include "Engine/Engine/App/ProjectSettingsFile.h"

#include "nlohmann/json.hpp"

namespace mye {
namespace {

using json = nlohmann::json;

constexpr const char* kDefaultName = "Default";

SortingLayer MakeLayer(int32_t id, const char* name)
{
    SortingLayer l;
    l.id = id;
    std::snprintf(l.name, sizeof(l.name), "%s", name != nullptr ? name : "");
    return l;
}

} // namespace

SortingLayers::SortingLayers()
{
    layers_.push_back(MakeLayer(kDefaultId, kDefaultName));
}

SortingLayers& SortingLayers::Get()
{
    static SortingLayers instance;
    return instance;
}

void SortingLayers::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    layers_.clear();

    json j = json::object();
    if (ReadProjectSettingsFile(ProjectSettingsPath(assetsRoot), j) != ProjectSettingsRead::Ok) {
        j = json::object();
    }
    uint32_t usedMask = 0;
    if (j.contains("sortingLayers") && j["sortingLayers"].is_array()) {
        for (const json& e : j["sortingLayers"]) {
            if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer()) {
                continue;
            }
            const int64_t id = e["id"].get<int64_t>();
            // 範囲外・重複は捨てる (手で壊した設定で起動を止めない)
            if (id < 0 || id >= kMaxLayers || ((usedMask >> id) & 1u) != 0u
                || static_cast<int>(layers_.size()) >= kMaxLayers) {
                continue;
            }
            usedMask |= 1u << id;
            const std::string name = e.contains("name") && e["name"].is_string() ? e["name"].get<std::string>() : "";
            layers_.push_back(MakeLayer(static_cast<int32_t>(id), name.c_str()));
        }
    }
    if (((usedMask >> kDefaultId) & 1u) == 0u) {
        // Default が無い表は壊れている。先頭に補って他の層の順序を保つ (id 0 が無いので最大 31 件、溢れない)
        layers_.insert(layers_.begin(), MakeLayer(kDefaultId, kDefaultName));
    }
}

bool SortingLayers::Save(const std::wstring& assetsRoot) const
{
    json arr = json::array();
    for (const SortingLayer& l : layers_) {
        arr.push_back({ { "id", l.id }, { "name", std::string(l.name) } });
    }
    return UpdateProjectSettingsFile(ProjectSettingsPath(assetsRoot),
                                     [&](json& j) { j["sortingLayers"] = std::move(arr); });
}

bool SortingLayers::DiffersFromDisk() const
{
    if (loadedRoot_.empty()) {
        return false; // 一度も読んでいない = この実行では何も編集していない
    }
    SortingLayers onDisk;
    onDisk.Load(loadedRoot_, true);
    if (onDisk.layers_.size() != layers_.size()) {
        return true;
    }
    for (size_t i = 0; i < layers_.size(); ++i) {
        if (onDisk.layers_[i].id != layers_[i].id
            || std::strncmp(onDisk.layers_[i].name, layers_[i].name, sizeof(layers_[i].name)) != 0) {
            return true;
        }
    }
    return false;
}

int SortingLayers::IndexOfId(int32_t id) const
{
    for (int i = 0; i < Count(); ++i) {
        if (layers_[i].id == id) {
            return i;
        }
    }
    return -1;
}

int32_t SortingLayers::IdOf(const char* name) const
{
    if (name == nullptr) {
        return -1;
    }
    for (const SortingLayer& l : layers_) {
        if (std::strncmp(l.name, name, sizeof(l.name)) == 0) {
            return l.id;
        }
    }
    return -1;
}

int32_t SortingLayers::Add(const char* name)
{
    if (Count() >= kMaxLayers) {
        return -1;
    }
    uint32_t usedMask = 0;
    for (const SortingLayer& l : layers_) {
        usedMask |= 1u << l.id;
    }
    for (int32_t id = 0; id < kMaxLayers; ++id) {
        if (((usedMask >> id) & 1u) == 0u) {
            layers_.push_back(MakeLayer(id, name));
            return id;
        }
    }
    return -1; // Count < kMaxLayers なら到達しない
}

bool SortingLayers::Remove(int32_t id)
{
    const int index = IndexOfId(id);
    if (id == kDefaultId || index < 0) {
        return false;
    }
    layers_.erase(layers_.begin() + index);
    return true;
}

bool SortingLayers::Rename(int32_t id, const char* name)
{
    const int index = IndexOfId(id);
    if (index < 0) {
        return false;
    }
    std::snprintf(layers_[index].name, sizeof(layers_[index].name), "%s", name != nullptr ? name : "");
    return true;
}

bool SortingLayers::Move(int from, int to)
{
    if (from < 0 || from >= Count() || to < 0 || to >= Count()) {
        return false;
    }
    const SortingLayer moved = layers_[from];
    layers_.erase(layers_.begin() + from);
    layers_.insert(layers_.begin() + to, moved);
    return true;
}

int SortingLayers::OrderOf(int32_t id) const
{
    int index = IndexOfId(id);
    if (index < 0) {
        index = IndexOfId(kDefaultId);
    }
    return index;
}

} // namespace mye
