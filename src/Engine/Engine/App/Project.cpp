#include "Engine/Engine/App/Project.h"

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

std::wstring ManifestPath(const std::wstring& root)
{
    return root + L"\\" + kProjectManifestFile;
}

constexpr const char* kStartContentDemo = "demo";
constexpr const char* kStartContentEmpty = "empty";

// 知らない綴りは Unspecified (新しい版が書いた値を古い版が読んでも落ちない)
ProjectStartContent ParseStartContent(const std::string& text)
{
    if (text == kStartContentDemo) {
        return ProjectStartContent::Demo;
    }
    if (text == kStartContentEmpty) {
        return ProjectStartContent::Empty;
    }
    return ProjectStartContent::Unspecified;
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
        out.canonicalRoot = j.value("canonicalRoot", out.canonicalRoot);
        out.startContent = ParseStartContent(j.value("startContent", std::string()));
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
    // 空なら**キー自体を書かない**。既定値の "" を書き出すと、
    // 「未記録」と「空に設定した」の区別が付かなくなる (M66b)
    if (!m.canonicalRoot.empty()) {
        j["canonicalRoot"] = m.canonicalRoot;
    }
    // Unspecified はキーを書かない (読み直しても「推定する」のままにする)
    if (m.startContent == ProjectStartContent::Demo) {
        j["startContent"] = kStartContentDemo;
    } else if (m.startContent == ProjectStartContent::Empty) {
        j["startContent"] = kStartContentEmpty;
    }

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
