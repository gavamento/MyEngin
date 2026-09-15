//====================================================================================
//                          DisplaySettings.cpp
//  三校/ 秋田蓮音                                                          09/15/2026
//                                  ウィンドウの表示モードを起動をまたいで覚える
//====================================================================================
#include "Engine/Engine/DisplaySettings.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace display {
namespace {

constexpr const char* kNameWindowed = "windowed";
constexpr const char* kNameBorderless = "borderless";

bool ModeFromName(const nlohmann::json& v, WindowMode& out)
{
    if (!v.is_string()) {
        return false;
    }
    const std::string& s = v.get_ref<const std::string&>();
    if (s == kNameWindowed) {
        out = WindowMode::Windowed;
        return true;
    }
    if (s == kNameBorderless) {
        out = WindowMode::Borderless;
        return true;
    }
    return false;
}

bool ReadText(const std::wstring& path, std::string& out)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

const char* WindowModeName(WindowMode mode)
{
    return (mode == WindowMode::Borderless) ? kNameBorderless : kNameWindowed;
}

std::wstring DisplaySettingsPath(const std::wstring& saveDir)
{
    return saveDir + L"\\display.json";
}

bool ParseDisplaySettings(const std::string& jsonText, WindowMode& out)
{
    const nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    return root.is_object() && root.contains("windowMode") && ModeFromName(root.at("windowMode"), out);
}

bool ParseProjectDefaultWindowMode(const std::string& jsonText, WindowMode& out)
{
    const nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object() || !root.contains("window") || !root.at("window").is_object()) {
        return false;
    }
    const nlohmann::json& window = root.at("window");
    return window.contains("defaultMode") && ModeFromName(window.at("defaultMode"), out);
}

WindowMode LoadStartupWindowMode(const std::wstring& assetsRoot, const std::wstring& saveDir)
{
    WindowMode mode = WindowMode::Windowed;
    std::string text;
    if (!assetsRoot.empty() && ReadText(assetsRoot + L"\\project_settings.json", text)) {
        ParseProjectDefaultWindowMode(text, mode); // 無ければウィンドウのまま (失敗しても mode は触られない)
    }
    if (!saveDir.empty() && ReadText(DisplaySettingsPath(saveDir), text)
        && !ParseDisplaySettings(text, mode)) {
        MYE_LOG_WARN("display.json: windowMode must be \"windowed\" or \"borderless\" - using %s",
                     WindowModeName(mode));
    }
    return mode;
}

bool SaveWindowMode(const std::wstring& saveDir, WindowMode mode)
{
    if (saveDir.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(saveDir, ec);
    nlohmann::json root = nlohmann::json::object();
    root["windowMode"] = WindowModeName(mode);
    return WriteFileReplacing(DisplaySettingsPath(saveDir), root.dump(2) + "\n");
}

} // namespace display
} // namespace mye
