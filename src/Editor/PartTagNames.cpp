#include "Editor/PartTagNames.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Parts.h"

#include "nlohmann/json.hpp"

namespace mye {

using json = nlohmann::json;

namespace {

// 初回 (キーが無いプロジェクト) に入れる例。空表だと Inspector のドロップダウンが
// 空になって「何を書けばいいか分からない」ので、よくある部位を種として置く
constexpr const char* kDefaultTags[] = { "Head", "HandL", "HandR", "FootL", "FootR", "Weapon" };

} // namespace

PartTagNames& PartTagNames::Get()
{
    static PartTagNames instance;
    return instance;
}

void PartTagNames::Load(const std::wstring& assetsRoot, bool force)
{
    if (!force && loadedRoot_ == assetsRoot) {
        return;
    }
    loadedRoot_ = assetsRoot;
    std::memset(names_, 0, sizeof(names_));
    count_ = 0;

    json j;
    {
        std::ifstream f(std::filesystem::path(assetsRoot + L"\\project_settings.json"));
        if (f) {
            try {
                f >> j;
            } catch (const json::exception& e) {
                MYE_LOG_WARN("[parts] project_settings.json parse failed: %s", e.what());
                j = json::object();
            }
        }
    }
    if (j.contains("partTags") && j["partTags"].is_array()) {
        for (const json& v : j["partTags"]) {
            if (count_ >= kMaxTags || !v.is_string()) {
                continue;
            }
            const std::string s = v.get<std::string>();
            if (s.empty()) {
                continue;
            }
            std::snprintf(names_[count_], kNameCapacity, "%s", s.c_str());
            ++count_;
        }
        return;
    }
    for (const char* d : kDefaultTags) {
        std::snprintf(names_[count_++], kNameCapacity, "%s", d);
    }
}

bool PartTagNames::Save(const std::wstring& assetsRoot) const
{
    const std::filesystem::path path(assetsRoot + L"\\project_settings.json");
    json j = json::object();
    {
        // 既存キー (physicsLayers / particle 設定) を保存で破壊しない read-modify-write
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
    for (int i = 0; i < count_; ++i) {
        if (names_[i][0] != '\0') {
            arr.push_back(std::string(names_[i])); // 空欄は落とす (= 行を消す操作になる)
        }
    }
    j["partTags"] = arr;
    std::ofstream out(path);
    if (!out) {
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

void PartTagNames::SetCount(int n)
{
    if (n < 0) {
        n = 0;
    }
    if (n > kMaxTags) {
        n = kMaxTags;
    }
    for (int i = n; i < count_; ++i) {
        names_[i][0] = '\0'; // 縮めた分は消しておく (次に増やしたとき残骸が出ない)
    }
    count_ = n;
}

const char* PartTagNames::Name(int i) const
{
    return (i < 0 || i >= count_) ? "" : names_[i];
}

uint64_t PartTagNames::Id(int i) const
{
    return Parts::TagOf(Name(i)); // 名前のハッシュが実体 (PartComponent::tag と同じ定義)
}

char* PartTagNames::EditBuffer(int i)
{
    static char dummy[kNameCapacity] = {};
    return (i < 0 || i >= kMaxTags) ? dummy : names_[i];
}

const char* PartTagNames::NameOf(uint64_t tag) const
{
    if (tag == 0) {
        return nullptr;
    }
    for (int i = 0; i < count_; ++i) {
        if (Parts::TagOf(names_[i]) == tag) {
            return names_[i];
        }
    }
    return nullptr;
}

} // namespace mye
