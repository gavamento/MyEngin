#include "Editor/EditorSettings.h"

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Log.h"

namespace mye {

void EditorSettings::Load(const std::wstring& dir)
{
    path_ = dir + L"\\editor_settings.json";
    std::ifstream f(std::filesystem::path(path_), std::ios::binary);
    if (!f) {
        return;
    }
    try {
        nlohmann::json root;
        f >> root;
        lastScenePath = root.value("lastScenePath", lastScenePath);
        externalEditorCmd = root.value("externalEditorCmd", externalEditorCmd);
        snapTranslate = root.value("snapTranslate", snapTranslate);
        snapRotateDeg = root.value("snapRotateDeg", snapRotateDeg);
        snapScale = root.value("snapScale", snapScale);
        gridVisible = root.value("gridVisible", gridVisible);
        camMoveSpeed = root.value("camMoveSpeed", camMoveSpeed);
        scmAutoFetch = root.value("scmAutoFetch", scmAutoFetch);
        scmFetchIntervalMin = root.value("scmFetchIntervalMin", scmFetchIntervalMin);
        particleCompareMode = root.value("particleCompareMode", particleCompareMode);
        particleCompareOffsetX = root.value("particleCompareOffsetX", particleCompareOffsetX);
        particleCpuSimd = root.value("particleCpuSimd", particleCpuSimd);
    } catch (const nlohmann::json::exception& ex) {
        MYE_LOG_WARN("editor_settings.json parse error: %s", ex.what());
    }
}

void EditorSettings::Save() const
{
    if (path_.empty()) {
        return;
    }
    // 既存の内容を保持しつつエディタ設定キーのみ更新 (マージ保存)
    nlohmann::json root;
    {
        std::ifstream f(std::filesystem::path(path_), std::ios::binary);
        if (f) {
            try {
                f >> root;
            } catch (const nlohmann::json::exception&) {
                root = nlohmann::json::object();
            }
        }
    }
    root["lastScenePath"] = lastScenePath;
    root["externalEditorCmd"] = externalEditorCmd;
    root["snapTranslate"] = snapTranslate;
    root["snapRotateDeg"] = snapRotateDeg;
    root["snapScale"] = snapScale;
    root["gridVisible"] = gridVisible;
    root["camMoveSpeed"] = camMoveSpeed;
    root["scmAutoFetch"] = scmAutoFetch;
    root["scmFetchIntervalMin"] = scmFetchIntervalMin;
    root["particleCompareMode"] = particleCompareMode;
    root["particleCompareOffsetX"] = particleCompareOffsetX;
    root["particleCpuSimd"] = particleCpuSimd;

    std::ofstream f(std::filesystem::path(path_), std::ios::binary);
    if (f) {
        const std::string text = root.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace mye
