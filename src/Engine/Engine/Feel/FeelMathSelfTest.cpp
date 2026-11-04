//====================================================================================
//                          FeelMathSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りの共有数学 (Shared/FeelMath.h、M94a) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Feel/FeelMathSelfTest.h"

#include <cmath>
#include <cstdint>

#include "Engine/Core/Diagnostics/Log.h"
#include "Shared/FeelMath.h"

namespace mye {

bool RunFeelMathSelfTest()
{
    MYE_LOG_INFO("==== FeelMath self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 多項式の初等関数 (CRT の値とは「近い」ことだけを見る。一致は求めない) ----
    {
        double maxSinErr = 0.0;
        for (int i = -60000; i <= 60000; ++i) {
            const float x = static_cast<float>(i) * 0.001f;
            const double err = std::fabs(static_cast<double>(MyeFeelSin(x)) - std::sin(static_cast<double>(x)));
            maxSinErr = err > maxSinErr ? err : maxSinErr;
        }
        check(maxSinErr < 4e-6, "MyeFeelSin is within 4e-6 of sin on [-60, 60]");
        double maxExpErr = 0.0;
        for (int i = -30000; i <= 10000; ++i) {
            const float x = static_cast<float>(i) * 0.001f;
            const double ref = std::exp2(static_cast<double>(x));
            const double err = std::fabs(static_cast<double>(MyeFeelExp2(x)) - ref) / ref;
            maxExpErr = err > maxExpErr ? err : maxExpErr;
        }
        check(maxExpErr < 1e-6, "MyeFeelExp2 relative error < 1e-6 on [-30, 10]");
        check(MyeFeelExp2(3.0f) == 8.0f && MyeFeelExp2(-2.0f) == 0.25f, "MyeFeelExp2 is exact at integers");
    }

    // ---- イージング: 端点 0 / 1、内部で大きな段差が無い (STEP を除く) ----
    {
        bool endpoints = true;
        bool continuous = true;
        for (int32_t e = 0; e < MYE_EASE_COUNT; ++e) {
            const float a = MyeEase(e, 0.0f);
            const float b = MyeEase(e, 1.0f);
            if (std::fabs(a) > 1e-5f || std::fabs(b - 1.0f) > 1e-5f) {
                MYE_LOG_ERROR("    ease %d: f(0)=%g f(1)=%g", e, static_cast<double>(a), static_cast<double>(b));
                endpoints = false;
            }
            if (e == MYE_EASE_STEP) {
                continue;
            }
            float prev = a;
            for (int i = 1; i <= 1000; ++i) {
                const float v = MyeEase(e, static_cast<float>(i) / 1000.0f);
                if (std::fabs(v - prev) > 0.05f) {
                    MYE_LOG_ERROR("    ease %d jumps at t=%d/1000", e, i);
                    continuous = false;
                    break;
                }
                prev = v;
            }
        }
        check(endpoints, "every ease maps 0 -> 0 and 1 -> 1");
        check(continuous, "every ease except STEP is continuous (no step > 0.05 per 1/1000)");
        check(MyeEase(MYE_EASE_STEP, 0.999f) == 0.0f && MyeEase(MYE_EASE_STEP, 1.0f) == 1.0f, "STEP holds until t = 1");
        check(MyeEase(MYE_EASE_OUT_BACK, 0.7f) > 1.0f, "OUT_BACK overshoots");
        check(MyeEase(MYE_EASE_IN_QUAD, -1.0f) == 0.0f && MyeEase(MYE_EASE_IN_QUAD, 2.0f) == 1.0f, "t is clamped to [0, 1]");
        check(MyeEase(9999, 0.25f) == 0.25f, "an unknown ease falls back to linear");
    }

    // ---- カーブ ----
    {
        const MyeCurve decay = MyeMakeCurve(MYE_EASE_LINEAR, 1.0f, 0.0f);
        check(MyeEvalCurve(decay, 0.25f) == 0.75f, "preset curve maps from -> to");
        MyeCurve c;
        c.mode = MYE_CURVE_CUSTOM;
        c.keyCount = 3;
        c.keys[0] = { 0.0f, 0.0f, 0.0f, 0.0f };
        c.keys[1] = { 0.5f, 1.0f, 0.0f, 0.0f };
        c.keys[2] = { 1.0f, 0.0f, 0.0f, 0.0f };
        check(MyeEvalCurve(c, 0.0f) == 0.0f && MyeEvalCurve(c, 0.5f) == 1.0f && MyeEvalCurve(c, 1.0f) == 0.0f,
              "custom curve passes through its keys");
        check(MyeEvalCurve(c, 0.25f) == 0.5f, "flat tangents give the Hermite midpoint");
        MyeCurve one;
        one.mode = MYE_CURVE_CUSTOM;
        one.keyCount = 1;
        one.keys[0].value = 0.3f;
        MyeCurve none;
        none.mode = MYE_CURVE_CUSTOM;
        check(MyeEvalCurve(one, 0.8f) == 0.3f && MyeEvalCurve(none, 0.5f) == 0.0f, "1 key = constant, 0 keys = 0");
    }

    // ---- ばね ----
    {
        float x = 0.0f;
        float v = 0.0f;
        for (int i = 0; i < 600; ++i) {
            MyeSpringStep(x, v, 1.0f, 50.0f, 0.1f, 1.0f / 60.0f); // dt に対して硬すぎる設定
        }
        check(std::isfinite(x) && std::fabs(x - 1.0f) < 1e-3f, "a stiff spring stays stable (implicit Euler)");
        x = 0.0f;
        v = 0.0f;
        float maxX = 0.0f;
        for (int i = 0; i < 180; ++i) {
            MyeSpringStep(x, v, 1.0f, 2.0f, 1.0f, 1.0f / 60.0f);
            maxX = x > maxX ? x : maxX;
        }
        check(maxX <= 1.0f + 1e-4f && std::fabs(x - 1.0f) < 1e-2f, "critical damping reaches the target without overshoot");
        float vel = 0.0f;
        float p = 0.0f;
        for (int i = 0; i < 120; ++i) {
            p = MyeSmoothDamp(p, 1.0f, vel, 0.3f, 1.0f / 60.0f);
        }
        check(std::fabs(p - 1.0f) < 1e-3f, "SmoothDamp converges");
    }

    // ---- 値ノイズ ----
    {
        bool inRange = true;
        for (int i = 0; i < 10000; ++i) {
            const float n = MyeValueNoise(1234u, static_cast<float>(i) * 0.01f);
            inRange = inRange && n >= -1.0f && n <= 1.0f;
        }
        check(inRange, "value noise stays in [-1, 1]");
        check(MyeValueNoise(7u, 3.25f) == MyeValueNoise(7u, 3.25f) && MyeValueNoise(7u, 3.25f) != MyeValueNoise(8u, 3.25f),
              "value noise is a pure function of (seed, x)");
    }

    return failCount == 0;
}

} // namespace mye
