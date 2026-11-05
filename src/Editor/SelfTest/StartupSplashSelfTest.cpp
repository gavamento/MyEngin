/*----
 StartupSplashSelfTest.cpp 起動スプラッシュの進捗とメビウス幾何の自己テスト
 作成者: 秋田蓮音								10/13/2026
----*/
#include "Editor/SelfTest/StartupSplashSelfTest.h"

#include <cmath>
#include <vector>

#include "Editor/App/StartupMoebius.h"
#include "Editor/App/StartupSplash.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Loop/StartupProgress.h"

namespace mye {

namespace {

bool Near(float a, float b, float eps)
{
    return std::fabs(a - b) <= eps;
}

} // namespace

bool RunStartupSplashSelfTest()
{
    MYE_LOG_INFO("==== StartupSplash self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    {
        const SplashWindowLayout at96 = ComputeSplashWindowLayout(96, 1920, 1080);
        check(at96.clientWidth == 640 && at96.clientHeight == 360 && Near(at96.renderTargetDpiX, 96.0f, 1e-3f)
                  && Near(at96.renderTargetDpiY, 96.0f, 1e-3f),
              "96 dpi: client 640x360, D2D dpi 96");
        const SplashWindowLayout at192 = ComputeSplashWindowLayout(192, 1920, 1080);
        check(at192.clientWidth == 1280 && at192.clientHeight == 720 && Near(at192.renderTargetDpiX, 192.0f, 1e-3f)
                  && Near(at192.renderTargetDpiY, 192.0f, 1e-3f),
              "192 dpi on 1920x1080 work area: client 1280x720");
        const SplashWindowLayout cramped = ComputeSplashWindowLayout(192, 800, 600);
        check(cramped.clientWidth == 800 && cramped.clientHeight == 450 && Near(cramped.renderTargetDpiX, 120.0f, 1e-3f)
                  && Near(cramped.renderTargetDpiY, 120.0f, 1e-3f),
              "192 dpi on 800x600 work area: uniform fit keeps 640x360 DIP layout");
    }

    check(Near(StartupProgressFraction(StartupStage::Window), 0.1f, 1e-4f), "Window progress = 0.1");
    check(Near(StartupProgressFraction(StartupStage::Scene), 1.0f, 1e-4f), "Scene progress = 1.0");
    check(Near(StartupProgressFraction(StartupStage::Ready), 1.0f, 1e-4f), "Ready progress = 1.0");
    check(StartupStageIndex(StartupStage::Assets) == 8, "Assets index = 8");
    check(StartupStageIndex(StartupStage::Ready) == kStartupProgressSteps, "Ready index = step count");

    startupmoebius::BuildParams params;
    params.centerX = 320.0f;
    params.centerY = 160.0f;
    params.scale = 50.0f;
    params.rotYRad = 0.0f;
    params.rotXRad = 0.0f;
    std::vector<startupmoebius::WireSegment2D> segments;
    startupmoebius::BuildWireframe(params, segments);
    check(segments.size() > 200, "Moebius wireframe has expected segment count");
    bool inBounds = true;
    for (const startupmoebius::WireSegment2D& seg : segments) {
        if (seg.x0 < 200.0f || seg.x0 > 440.0f || seg.y0 < 80.0f || seg.y0 > 240.0f) {
            inBounds = false;
            break;
        }
    }
    check(inBounds, "Projected segments stay in center region");

    const float uSample = 0.85f;
    const float vSample = 0.35f;
    const float z0 = startupmoebius::SampleEyeZ(uSample, vSample, params);
    params.rotYRad = 3.1415926535f;
    const float zPi = startupmoebius::SampleEyeZ(uSample, vSample, params);
    check(std::fabs(z0 - zPi) > 0.01f, "Half rotation changes eye-space depth (front/back swap)");

    return failCount == 0;
}

} // namespace mye
