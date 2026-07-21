#include "Engine/Engine/UI/UISelfTest.h"

#include <cmath>

#include "Engine/Core/Log.h"
#include "Engine/Engine/UI/UIRenderer.h"

namespace mye {

bool RunUISelfTest()
{
    MYE_LOG_INFO("==== UI self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ResolveAnchor: 9-grid が正しい画面基準点にマップされるか (x/y は基準点からのオフセット)
    constexpr int W = 1000;
    constexpr int H = 800;
    constexpr float ox = 10.0f;
    constexpr float oy = 5.0f;
    struct Case {
        int anchor;
        float baseX;
        float baseY;
        const char* name;
    };
    const Case cases[9] = {
        { 0, 0.0f, 0.0f, "top-left" },      { 1, 500.0f, 0.0f, "top-center" },
        { 2, 1000.0f, 0.0f, "top-right" },  { 3, 0.0f, 400.0f, "mid-left" },
        { 4, 500.0f, 400.0f, "center" },    { 5, 1000.0f, 400.0f, "mid-right" },
        { 6, 0.0f, 800.0f, "bottom-left" }, { 7, 500.0f, 800.0f, "bottom-center" },
        { 8, 1000.0f, 800.0f, "bottom-right" },
    };
    for (const Case& c : cases) {
        float rx = 0, ry = 0;
        UIRenderer::ResolveAnchor(c.anchor, ox, oy, 100.0f, 40.0f, W, H, rx, ry);
        const bool ok = std::fabs(rx - (c.baseX + ox)) < 1e-4f
            && std::fabs(ry - (c.baseY + oy)) < 1e-4f;
        check(ok, c.name);
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== UI self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== UI self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
