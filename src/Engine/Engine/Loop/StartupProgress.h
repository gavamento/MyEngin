/*----
 StartupProgress.h 起動段階の通知 (Editor スプラッシュ等)
 作成者: 秋田蓮音								10/13/2026
----*/
#pragma once
#include <cstdint>

namespace mye {

// EngineLoop::Run と EditorApp::OnStart が報告する起動段階 (順序固定)
enum class StartupStage : uint8_t {
    Window = 0,
    Device,
    Shaders,
    RenderPaths,
    Ui,
    Scripts,
    CSharp,
    Audio,
    Assets,
    Scene,
    Ready,
};

// Ready を除く進捗ステップ数 (表示は 1..kStartupProgressSteps / kStartupProgressSteps)
inline constexpr int kStartupProgressSteps = 10;

inline int StartupStageIndex(StartupStage stage)
{
    switch (stage) {
    case StartupStage::Window: return 0;
    case StartupStage::Device: return 1;
    case StartupStage::Shaders: return 2;
    case StartupStage::RenderPaths: return 3;
    case StartupStage::Ui: return 4;
    case StartupStage::Scripts: return 5;
    case StartupStage::CSharp: return 6;
    case StartupStage::Audio: return 7;
    case StartupStage::Assets: return 8;
    case StartupStage::Scene: return 9;
    case StartupStage::Ready: return kStartupProgressSteps;
    }
    return 0;
}

inline float StartupProgressFraction(StartupStage stage)
{
    const int idx = StartupStageIndex(stage);
    if (idx >= kStartupProgressSteps) {
        return 1.0f;
    }
    return static_cast<float>(idx + 1) / static_cast<float>(kStartupProgressSteps);
}

// ログ用の固定名 (ローカライズしない)
inline const char* StartupStageLogName(StartupStage stage)
{
    switch (stage) {
    case StartupStage::Window: return "window";
    case StartupStage::Device: return "device";
    case StartupStage::Shaders: return "shaders";
    case StartupStage::RenderPaths: return "render_paths";
    case StartupStage::Ui: return "ui";
    case StartupStage::Scripts: return "scripts";
    case StartupStage::CSharp: return "csharp";
    case StartupStage::Audio: return "audio";
    case StartupStage::Assets: return "assets";
    case StartupStage::Scene: return "scene";
    case StartupStage::Ready: return "ready";
    }
    return "unknown";
}

class IStartupObserver {
public:
    virtual ~IStartupObserver() = default;
    virtual void OnStage(StartupStage stage) = 0;
    virtual void OnReady() = 0;
};

} // namespace mye
