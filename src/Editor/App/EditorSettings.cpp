#include "Editor/App/EditorSettings.h"

#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

#include "Engine/Core/Diagnostics/Log.h"

namespace mye {
namespace {

// OverlayColorId の並びと同じ (追加・更新・削除・競合)
constexpr const char* kAiChangeColorKeys[kOverlayColorCount] = {
    "aiChangeColorAdded", "aiChangeColorModified", "aiChangeColorRemoved", "aiChangeColorConflict",
};

} // namespace

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
        // Save は既定値のままでもキーを書くので、旧既定値は「選んだ値」と区別できない。
        // 旧既定値と完全一致するときだけ移行し、手で変えた値は残す
        if (externalEditorCmd == kLegacyExternalEditorCmd) {
            externalEditorCmd = kExternalEditorVisualStudio;
        }
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
        feelMuteCamera = root.value("feelMuteCamera", feelMuteCamera);
        changeTrackingEnabled = root.value("changeTrackingEnabled", changeTrackingEnabled);
        if (root.contains("aiChangeAcks") && root["aiChangeAcks"].is_object()) {
            aiChangeAcks.clear();
            for (const auto& item : root["aiChangeAcks"].items()) {
                if (item.value().is_number_integer()) {
                    aiChangeAcks[item.key()] = item.value().get<int64_t>();
                }
            }
        }
        aiChangeShowOverlay = root.value("aiChangeShowOverlay", aiChangeShowOverlay);
        aiChangeShowGhosts = root.value("aiChangeShowGhosts", aiChangeShowGhosts);
        for (int i = 0; i < kOverlayColorCount; ++i) {
            const char* key = kAiChangeColorKeys[i];
            if (!root.contains(key) || !root[key].is_array() || root[key].size() != 4) {
                continue;
            }
            OverlayColor color = aiChangePalette.colors[i];
            float* components[4] = { &color.r, &color.g, &color.b, &color.a };
            bool valid = true;
            for (int c = 0; c < 4; ++c) {
                valid = valid && root[key][c].is_number();
                if (valid) {
                    *components[c] = root[key][c].get<float>();
                }
            }
            if (valid) {
                aiChangePalette.colors[i] = color;
            }
        }
        profilerTab = root.value("profilerTab", profilerTab);
        profilerAvgFrames = root.value("profilerAvgFrames", profilerAvgFrames);
        profilerHideZero = root.value("profilerHideZero", profilerHideZero);
        profilerSortByCost = root.value("profilerSortByCost", profilerSortByCost);
        profilerItemTiming = root.value("profilerItemTiming", profilerItemTiming);
        profilerCaptureEnabled = root.value("profilerCaptureEnabled", profilerCaptureEnabled);
        profilerCaptureMs = root.value("profilerCaptureMs", profilerCaptureMs);
        if (root.contains("profilerBudgets") && root["profilerBudgets"].is_object()) {
            profilerBudgets.clear();
            for (const auto& item : root["profilerBudgets"].items()) {
                if (item.value().is_number()) {
                    profilerBudgets.push_back(ProfilerBudget{ item.key(), item.value().get<float>() });
                }
            }
        }
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
    root["feelMuteCamera"] = feelMuteCamera;
    root["changeTrackingEnabled"] = changeTrackingEnabled;
    {
        nlohmann::json acks = nlohmann::json::object();
        for (const auto& [documentId, revisionId] : aiChangeAcks) {
            acks[documentId] = revisionId;
        }
        root["aiChangeAcks"] = acks;
    }
    root["aiChangeShowOverlay"] = aiChangeShowOverlay;
    root["aiChangeShowGhosts"] = aiChangeShowGhosts;
    for (int i = 0; i < kOverlayColorCount; ++i) {
        const OverlayColor& color = aiChangePalette.colors[i];
        root[kAiChangeColorKeys[i]] = nlohmann::json::array({ color.r, color.g, color.b, color.a });
    }
    root["profilerTab"] = profilerTab;
    root["profilerAvgFrames"] = profilerAvgFrames;
    root["profilerHideZero"] = profilerHideZero;
    root["profilerSortByCost"] = profilerSortByCost;
    root["profilerItemTiming"] = profilerItemTiming;
    root["profilerCaptureEnabled"] = profilerCaptureEnabled;
    root["profilerCaptureMs"] = profilerCaptureMs;
    {
        nlohmann::json budgets = nlohmann::json::object();
        for (const ProfilerBudget& budget : profilerBudgets) {
            budgets[budget.key] = budget.ms;
        }
        root["profilerBudgets"] = budgets;
    }

    std::ofstream f(std::filesystem::path(path_), std::ios::binary);
    if (f) {
        const std::string text = root.dump(2);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}

} // namespace mye
