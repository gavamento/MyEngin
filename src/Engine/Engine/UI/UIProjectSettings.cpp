#include "Engine/Engine/UI/UIProjectSettings.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/App/ProjectSettingsFile.h"

namespace mye {
namespace uilayout {
namespace {

std::wstring SettingsPath(const std::wstring& assetsRoot)
{
    return ProjectSettingsPath(assetsRoot);
}

bool InRange(int v)
{
    return v >= kReferenceMin && v <= kReferenceMax;
}

} // namespace

bool ParseProjectUiSettings(const std::string& jsonText, ProjectUiSettings& out)
{
    out = ProjectUiSettings{};
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("project_settings.json parse error: %s", ex.what());
        return false;
    }
    if (!root.is_object() || !root.contains("ui") || !root["ui"].is_object()) {
        return false;
    }
    const nlohmann::json& ui = root["ui"];
    // ★幅と高さは**組で**採否を決める。片方だけ採ると「1280 x 1080」のような
    //   誰も書いていないアスペクトの基準ができてしまう
    const bool haveW = ui.contains("referenceW") && ui["referenceW"].is_number_integer();
    const bool haveH = ui.contains("referenceH") && ui["referenceH"].is_number_integer();
    if (!haveW || !haveH) {
        MYE_LOG_WARN("project_settings.json: ui.referenceW/H must both be integers - using %dx%d",
                     kCanvasRefW, kCanvasRefH);
        return false;
    }
    const int64_t w = ui["referenceW"].get<int64_t>();
    const int64_t h = ui["referenceH"].get<int64_t>();
    if (w < kReferenceMin || w > kReferenceMax || h < kReferenceMin || h > kReferenceMax) {
        MYE_LOG_WARN("project_settings.json: ui.referenceW/H out of range (%lld x %lld) - using %dx%d",
                     static_cast<long long>(w), static_cast<long long>(h), kCanvasRefW, kCanvasRefH);
        return false;
    }
    out.referenceW = static_cast<int>(w);
    out.referenceH = static_cast<int>(h);
    return true;
}

ProjectUiSettings LoadProjectUiSettings(const std::wstring& assetsRoot)
{
    ProjectUiSettings s;
    if (assetsRoot.empty()) {
        return s; // ヘッドレス (セルフテスト等) = 既定
    }
    std::ifstream f(std::filesystem::path(SettingsPath(assetsRoot)), std::ios::binary);
    if (!f) {
        return s;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    ParseProjectUiSettings(ss.str(), s);
    return s;
}

bool SaveProjectUiSettings(const std::wstring& assetsRoot, const ProjectUiSettings& settings)
{
    if (assetsRoot.empty() || !InRange(settings.referenceW) || !InRange(settings.referenceH)) {
        return false;
    }
    return UpdateProjectSettingsFile(SettingsPath(assetsRoot), [&settings](nlohmann::json& root) {
        // "ui" がオブジェクト以外で置かれていると、下の添字アクセスが例外を投げる
        if (!root.contains("ui") || !root["ui"].is_object()) {
            root["ui"] = nlohmann::json::object();
        }
        root["ui"]["referenceW"] = settings.referenceW;
        root["ui"]["referenceH"] = settings.referenceH;
    });
}

} // namespace uilayout
} // namespace mye
