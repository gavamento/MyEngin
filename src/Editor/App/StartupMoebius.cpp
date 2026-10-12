/*----
 StartupMoebius.cpp メビウスの輪ワイヤーフレーム (起動スプラッシュ用)
 作成者: 秋田蓮音								10/13/2026
----*/
#include "Editor/App/StartupMoebius.h"

#include <cmath>

namespace mye {
namespace startupmoebius {
namespace {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 RotateY(Vec3 p, float a)
{
    const float c = std::cos(a);
    const float s = std::sin(a);
    return { p.x * c + p.z * s, p.y, -p.x * s + p.z * c };
}

Vec3 RotateX(Vec3 p, float a)
{
    const float c = std::cos(a);
    const float s = std::sin(a);
    return { p.x, p.y * c - p.z * s, p.y * s + p.z * c };
}

Vec3 MoebiusPoint(float u, float v, float majorR, float halfW)
{
    const float cu = std::cos(u);
    const float su = std::sin(u);
    const float hv = halfW * v;
    const float ring = majorR + hv * std::cos(u * 0.5f);
    return { ring * cu, ring * su, hv * std::sin(u * 0.5f) };
}

Vec3 ToEyeSpace(float u, float v, const BuildParams& params)
{
    Vec3 p = MoebiusPoint(u, v, params.majorRadius, params.halfWidth);
    p = RotateY(p, params.rotYRad);
    p = RotateX(p, params.rotXRad);
    p.z += params.cameraDistance;
    return p;
}

bool ProjectPoint(float u, float v, const BuildParams& params, float& outX, float& outY, float& outEyeZ)
{
    const Vec3 eye = ToEyeSpace(u, v, params);
    outEyeZ = eye.z;
    if (eye.z <= 0.01f) {
        return false;
    }
    const float inv = params.scale * params.cameraDistance / eye.z;
    outX = params.centerX + eye.x * inv;
    outY = params.centerY - eye.y * inv;
    return true;
}

void AddSegment(const BuildParams& params, float u0, float v0, float u1, float v1,
                std::vector<WireSegment2D>& out)
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float z0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float z1 = 0.0f;
    if (!ProjectPoint(u0, v0, params, x0, y0, z0) || !ProjectPoint(u1, v1, params, x1, y1, z1)) {
        return;
    }
    WireSegment2D seg;
    seg.x0 = x0;
    seg.y0 = y0;
    seg.x1 = x1;
    seg.y1 = y1;
    seg.eyeZ = (z0 + z1) * 0.5f;
    out.push_back(seg);
}

} // namespace

void BuildWireframe(const BuildParams& params, std::vector<WireSegment2D>& outSegments)
{
    outSegments.clear();
    if (params.uSteps < 4 || params.vLines < 2) {
        return;
    }
    const float twoPi = 6.28318530718f;
    const float uStep = twoPi / static_cast<float>(params.uSteps);
    const float vMin = -1.0f;
    const float vMax = 1.0f;
    const float vStep = (vMax - vMin) / static_cast<float>(params.vLines - 1);

    for (int vi = 0; vi < params.vLines; ++vi) {
        const float v = vMin + static_cast<float>(vi) * vStep;
        for (int ui = 0; ui < params.uSteps; ++ui) {
            const float u0 = static_cast<float>(ui) * uStep;
            const float u1 = (ui + 1 == params.uSteps) ? twoPi : static_cast<float>(ui + 1) * uStep;
            AddSegment(params, u0, v, u1, v, outSegments);
        }
    }
    for (int ui = 0; ui <= params.uSteps; ++ui) {
        const float u = (ui == params.uSteps) ? twoPi : static_cast<float>(ui) * uStep;
        for (int vi = 0; vi < params.vLines - 1; ++vi) {
            const float v0 = vMin + static_cast<float>(vi) * vStep;
            const float v1 = v0 + vStep;
            AddSegment(params, u, v0, u, v1, outSegments);
        }
    }
}

float SampleEyeZ(float uRad, float v, const BuildParams& params)
{
    return ToEyeSpace(uRad, v, params).z;
}

} // namespace startupmoebius
} // namespace mye
