#pragma once
#include <string>

#include "Engine/Engine/EngineLoop.h"

namespace mye {

// Build Settings ウィンドウ (engine_spec.md 9 章、M15)。
// Runtime.exe + GameLogic.dll + assets\ を出力フォルダへパッケージし、
// 選択したブートシーンを main.scene.json として配置する。
// (Release ビルド自体は VS / MSBuild で行う前提。ここは成果物のパッケージング)
class BuildSettingsWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx);

private:
    void DoPackage(EngineContext& ctx);

    char outputDir_[512] = {};
    std::string bootScene_ = "main.scene.json";
    bool init_ = false;
    std::string status_;
};

} // namespace mye
