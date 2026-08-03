#include "Editor/EditorGlobalSettings.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

std::wstring MachineLocalDir()
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
    return base + L"\\MyEngine";
}

std::wstring EditorGlobalSettings::Path()
{
    return MachineLocalDir() + L"\\editor_global.json";
}

void EditorGlobalSettings::Load()
{
    std::ifstream f(std::filesystem::path(Path()), std::ios::binary);
    if (!f) {
        return; // 未作成なら既定値 (日本語) のまま
    }
    try {
        nlohmann::json root;
        f >> root;
        uiLanguage = LangFromString(root.value("uiLanguage", LangToString(uiLanguage)).c_str());
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("editor_global.json parse error: %s", ex.what());
    }
}

void EditorGlobalSettings::Save() const
{
    // 既存の内容を保持しつつ自分のキーのみ更新 (EditorSettings と同じマージ保存)
    nlohmann::json root;
    {
        std::ifstream f(std::filesystem::path(Path()), std::ios::binary);
        if (f) {
            try {
                f >> root;
            } catch (const nlohmann::json::exception&) {
                root = nlohmann::json::object();
            }
        }
    }
    root["uiLanguage"] = LangToString(uiLanguage);

    std::error_code ec;
    std::filesystem::create_directories(MachineLocalDir(), ec);
    std::ofstream f(std::filesystem::path(Path()), std::ios::binary);
    if (f) {
        const std::string text = root.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace mye
