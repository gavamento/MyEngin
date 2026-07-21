#include "Engine/Engine/Project.h"

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::wstring ManifestPath(const std::wstring& root)
{
    return root + L"\\" + kProjectManifestFile;
}

} // namespace

bool IsProjectRoot(const std::wstring& dir)
{
    std::error_code ec;
    return std::filesystem::exists(ManifestPath(dir), ec);
}

bool LoadProjectManifest(const std::wstring& root, ProjectManifest& out)
{
    std::ifstream f(std::filesystem::path(ManifestPath(root)), std::ios::binary);
    if (!f) {
        return false;
    }
    try {
        nlohmann::json j;
        f >> j;
        out.name = j.value("name", out.name);
        out.engineVersion = j.value("engineVersion", out.engineVersion);
        out.bootScene = j.value("bootScene", out.bootScene);
        return true;
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("project.mye.json parse error: %s", ex.what());
        return false;
    }
}

bool SaveProjectManifest(const std::wstring& root, const ProjectManifest& m)
{
    nlohmann::json j;
    j["formatVersion"] = 1;
    j["name"] = m.name;
    j["engineVersion"] = m.engineVersion;
    j["bootScene"] = m.bootScene;

    std::ofstream f(std::filesystem::path(ManifestPath(root)), std::ios::binary);
    if (!f) {
        return false;
    }
    const std::string text = j.dump(2);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return true;
}

std::wstring ProjectBootScenePath(const std::wstring& root, const ProjectManifest& m)
{
    std::wstring rel = Utf8ToWide(m.bootScene);
    for (wchar_t& c : rel) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    return root + L"\\assets\\" + rel;
}

} // namespace mye
