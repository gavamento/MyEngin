//====================================================================================
//                          DisturbanceNoise.cpp
//  MyEngin/ 秋田蓮音                                                       11/09/2026
//                                          かき乱しフィールドの霧のノイズの CB を組む実装
//====================================================================================
#include "Engine/Renderer/Passes/DisturbanceNoise.h"

#include <algorithm>
#include <cmath>

namespace mye {

namespace {

float FiniteOrDefault(float v, float fallback)
{
    return std::isfinite(v) ? v : fallback;
}

// 強さを [0,1] へ。非有限は 0 (掛けない)
float Strength01(float v)
{
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

// 模様の大きさ [m]。小さすぎるとピクセルより細かい模様になり、ちらつくだけなので下限を置く
constexpr float kMinNoiseScale = 0.1f;

float ScaleOrDefault(float v, float fallback)
{
    return (std::isfinite(v) && v > 0.0f) ? (std::max)(v, kMinNoiseScale) : fallback;
}

// x を [0, period) へ折り返す。double で計算してから float へ落とす (長時間回しても桁が落ちない)
float Wrap(double x, double period)
{
    if (!std::isfinite(x)) {
        return 0.0f;
    }
    double r = std::fmod(x, period);
    if (r < 0.0) {
        r += period;
    }
    return static_cast<float>(r);
}

} // namespace

DisturbanceNoiseCB PackDisturbanceNoise(const DisturbanceNoiseUpload& n, double timeSeconds)
{
    DisturbanceNoiseCB cb;
    const double t = std::isfinite(timeSeconds) ? timeSeconds : 0.0;
    const double period = static_cast<double>(kDisturbanceNoisePeriod);

    const float strength = Strength01(n.strength);
    if (strength > 0.0f) {
        const float scale = ScaleOrDefault(n.scale, 4.0f);
        const double inv = 1.0 / static_cast<double>(scale);
        for (int k = 0; k < 3; ++k) {
            cb.a[k] = Wrap(static_cast<double>(FiniteOrDefault(n.scroll[k], 0.0f)) * t * inv, period);
        }
        cb.a[3] = static_cast<float>(inv);
        cb.b[0] = strength;
        cb.b[1] = static_cast<float>(std::clamp(n.octaves, 1, kDisturbanceNoiseMaxOctaves));
        cb.b[2] = Wrap(static_cast<double>((std::max)(FiniteOrDefault(n.evolve, 0.0f), 0.0f)) * t, period);
    }

    const float mapStrength = n.mapSRV != nullptr ? Strength01(n.mapStrength) : 0.0f;
    if (mapStrength > 0.0f) {
        const float mapScale = ScaleOrDefault(n.mapScale, 8.0f);
        const double inv = 1.0 / static_cast<double>(mapScale);
        cb.b[3] = mapStrength;
        cb.c[0] = Wrap(static_cast<double>(FiniteOrDefault(n.mapScroll[0], 0.0f)) * t * inv, 1.0);
        cb.c[1] = Wrap(static_cast<double>(FiniteOrDefault(n.mapScroll[1], 0.0f)) * t * inv, 1.0);
        cb.c[2] = static_cast<float>(inv);
        cb.c[3] = 1.0f;
    }
    return cb;
}

} // namespace mye
