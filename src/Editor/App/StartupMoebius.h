/*----
 StartupMoebius.h メビウスの輪ワイヤーフレーム (起動スプラッシュ用)
 作成者: 秋田蓮音								10/13/2026
----*/
#pragma once
#include <cstdint>
#include <vector>

namespace mye {
namespace startupmoebius {

struct WireSegment2D {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float eyeZ = 0.0f; // カメラ空間の奥行 (大きいほど手前)
};

struct BuildParams {
    float rotYRad = 0.0f;
    float rotXRad = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float scale = 1.0f;
    int uSteps = 96;
    int vLines = 9;
    float majorRadius = 1.0f;
    float halfWidth = 0.28f;
    float cameraDistance = 4.0f;
};

// 透視投影した線分を生成 (描画は呼び出し側)
void BuildWireframe(const BuildParams& params, std::vector<WireSegment2D>& outSegments);

// 点 (u,v) のカメラ空間 Z (自己テスト用)
float SampleEyeZ(float uRad, float v, const BuildParams& params);

} // namespace startupmoebius
} // namespace mye
