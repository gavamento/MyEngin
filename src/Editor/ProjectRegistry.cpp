#include "Editor/ProjectRegistry.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Log.h"
#include "Engine/Engine/Project.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::string NowIsoUtc()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm = {};
    gmtime_s(&tm, &now);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

std::wstring ProjectRegistry::RegistryPath()
{
    wchar_t* localAppData = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&localAppData, &len, L"LOCALAPPDATA") == 0 && localAppData) {
        base = localAppData;
        free(localAppData);
    }
    if (base.empty()) {
        base = GetExecutableDir(); // フォールバック (通常は到達しない)
    }
    return base + L"\\MyEngine\\projects.json";
}

void ProjectRegistry::Load()
{
    entries_.clear();
    std::ifstream f(std::filesystem::path(RegistryPath()), std::ios::binary);
    if (!f) {
        return;
    }
    try {
        nlohmann::json j;
        f >> j;
        for (const auto& p : j.value("projects", nlohmann::json::array())) {
            ProjectRegistryEntry e;
            e.path = Utf8ToWide(p.value("path", std::string()));
            e.name = p.value("name", std::string());
            e.lastOpenedIso = p.value("lastOpened", std::string());
            e.pinned = p.value("pinned", false);
            if (e.path.empty()) {
                continue;
            }
            std::error_code ec;
            e.missing = !std::filesystem::exists(e.path + L"\\" + kProjectManifestFile, ec);
            entries_.push_back(std::move(e));
        }
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("projects.json parse error: %s", ex.what());
        entries_.clear();
    }
    SortEntries();
}

void ProjectRegistry::Save() const
{
    const std::wstring path = RegistryPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    nlohmann::json j;
    j["formatVersion"] = 1;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries_) {
        nlohmann::json p;
        p["path"] = WideToUtf8(e.path);
        p["name"] = e.name;
        p["lastOpened"] = e.lastOpenedIso;
        p["pinned"] = e.pinned;
        arr.push_back(std::move(p));
    }
    j["projects"] = std::move(arr);

    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    if (f) {
        const std::string text = j.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

ProjectRegistryEntry* ProjectRegistry::FindByPath(const std::wstring& projectRoot)
{
    const std::wstring key = NormalizePathKey(projectRoot);
    for (auto& e : entries_) {
        if (NormalizePathKey(e.path) == key) {
            return &e;
        }
    }
    return nullptr;
}

void ProjectRegistry::Touch(const std::wstring& projectRoot, const std::string& name)
{
    if (ProjectRegistryEntry* e = FindByPath(projectRoot)) {
        e->lastOpenedIso = NowIsoUtc();
        if (!name.empty()) {
            e->name = name;
        }
        e->missing = false;
    } else {
        ProjectRegistryEntry n;
        n.path = projectRoot;
        n.name = name;
        n.lastOpenedIso = NowIsoUtc();
        entries_.push_back(std::move(n));
    }
    SortEntries();
    Save();
}

void ProjectRegistry::SetPinned(const std::wstring& projectRoot, bool pinned)
{
    if (ProjectRegistryEntry* e = FindByPath(projectRoot)) {
        e->pinned = pinned;
        SortEntries();
        Save();
    }
}

void ProjectRegistry::Remove(const std::wstring& projectRoot)
{
    const std::wstring key = NormalizePathKey(projectRoot);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&key](const ProjectRegistryEntry& e) {
                                      return NormalizePathKey(e.path) == key;
                                  }),
                   entries_.end());
    Save();
}

void ProjectRegistry::SortEntries()
{
    // pinned 優先 → lastOpened 降順 (ISO8601 は文字列比較で時刻順になる)
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const ProjectRegistryEntry& a, const ProjectRegistryEntry& b) {
                         if (a.pinned != b.pinned) {
                             return a.pinned;
                         }
                         return a.lastOpenedIso > b.lastOpenedIso;
                     });
}

} // namespace mye
