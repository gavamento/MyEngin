#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <DirectXMath.h>

namespace mye {

// Gerstner (トロコイド) 波の 1 成分パラメータ (POD)
struct GerstnerWave {
    float amplitude = 0.2f;    // 振幅 [m]
    float wavelength = 10.0f;  // 波長 [m]
    float speed = 2.0f;        // 伝播速度 [m/s]
    float dirAngleDeg = 0.0f;  // 進行方向 (度、0 = +X 方向, 90 = +Z 方向)
    float steepness = 0.5f;    // 急峻度 [0, 1] (0 = 正弦波, 1 = 最大尖り)
    float pad[3] = {};         // 16 バイト境界パディング
};

namespace wave {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 6.28318530717958647692f;
inline constexpr float kDegToRad = kPi / 180.0f;

// 1 本の波の評価結果
struct WaveSample {
    DirectX::XMFLOAT3 displacement = { 0.0f, 0.0f, 0.0f }; // (dx, dy, dz)
    DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };       // 未正規化の法線成分
};

// 基準座標 (x, z) と時刻 t に対する水面変位と解析的法線を計算
inline void SampleGerstnerWaves(const GerstnerWave* waves, int32_t count, float x, float z, float time,
                                float baseHeight, float overallScale, float timeScale,
                                DirectX::XMFLOAT3& outPos, DirectX::XMFLOAT3& outNormal)
{
    float totalDx = 0.0f;
    float totalDz = 0.0f;
    float totalDy = 0.0f;

    // 法線計算用の偏微分累積:
    // dP/dx = (1 - sum(Q * A * k * Dx^2 * cos), sum(A * k * Dx * sin), -sum(Q * A * k * Dx * Dz * cos))
    // dP/dz = (-sum(Q * A * k * Dx * Dz * cos), sum(A * k * Dz * sin), 1 - sum(Q * A * k * Dz^2 * cos))
    // N = B x T = dP/dz x dP/dx の簡約形:
    // Nx = -sum(Dx * A * k * sin)
    // Nz = -sum(Dz * A * k * sin)
    // Ny = 1 - sum(Q * A * k * cos)
    float nX = 0.0f;
    float nZ = 0.0f;
    float nY = 1.0f;

    const float t = time * timeScale;

    if (waves != nullptr && count > 0 && overallScale > 0.0f) {
        // 急峻度の総和が 1 を超えると自己交差 (波頭がループする) ため正規化係数を計算
        float sumQ = 0.0f;
        for (int32_t i = 0; i < count; ++i) {
            sumQ += std::max(0.0f, waves[i].steepness);
        }
        const float qNorm = (sumQ > 1.0f) ? (1.0f / sumQ) : 1.0f;

        for (int32_t i = 0; i < count; ++i) {
            const GerstnerWave& w = waves[i];
            const float a = std::max(0.0f, w.amplitude) * overallScale;
            if (a <= 1e-6f) {
                continue;
            }

            const float len = std::max(0.1f, w.wavelength);
            const float k = kTwoPi / len; // 波数
            const float omega = k * w.speed; // 角周波数
            const float rad = w.dirAngleDeg * kDegToRad;
            const float dirX = std::cos(rad);
            const float dirZ = std::sin(rad);

            // 急峻度 (尖り率)
            const float q = std::max(0.0f, std::min(1.0f, w.steepness)) * qNorm;
            const float qa = q * a;

            // 位相角: phi = k * (dir . pos) - omega * t
            const float phi = k * (dirX * x + dirZ * z) - omega * t;
            const float sinPhi = std::sin(phi);
            const float cosPhi = std::cos(phi);

            // 水平変位 (波頭に向かって引き寄せられる)
            totalDx -= dirX * qa * sinPhi;
            totalDz -= dirZ * qa * sinPhi;
            // 垂直変位
            totalDy += a * cosPhi;

            // 法線寄与
            const float ka = k * a;
            nX -= dirX * ka * sinPhi;
            nZ -= dirZ * ka * sinPhi;
            nY -= q * ka * cosPhi;
        }
    }

    outPos.x = x + totalDx;
    outPos.y = baseHeight + totalDy;
    outPos.z = z + totalDz;

    // 法線の正規化
    const float lenSq = nX * nX + nY * nY + nZ * nZ;
    if (lenSq > 1e-12f) {
        const float invLen = 1.0f / std::sqrt(lenSq);
        outNormal.x = nX * invLen;
        outNormal.y = nY * invLen;
        outNormal.z = nZ * invLen;
    } else {
        outNormal.x = 0.0f;
        outNormal.y = 1.0f;
        outNormal.z = 0.0f;
    }
}

// 浮力・物理用の波高計算 (変位なし直接評価: 高速版)
inline float EvaluateWaveHeight(const GerstnerWave* waves, int32_t count, float x, float z, float time,
                                float baseHeight, float overallScale, float timeScale)
{
    float totalDy = 0.0f;
    const float t = time * timeScale;

    if (waves != nullptr && count > 0 && overallScale > 0.0f) {
        for (int32_t i = 0; i < count; ++i) {
            const GerstnerWave& w = waves[i];
            const float a = std::max(0.0f, w.amplitude) * overallScale;
            if (a <= 1e-6f) {
                continue;
            }

            const float len = std::max(0.1f, w.wavelength);
            const float k = kTwoPi / len;
            const float omega = k * w.speed;
            const float rad = w.dirAngleDeg * kDegToRad;
            const float dirX = std::cos(rad);
            const float dirZ = std::sin(rad);

            const float phi = k * (dirX * x + dirZ * z) - omega * t;
            totalDy += a * std::cos(phi);
        }
    }

    return baseHeight + totalDy;
}

// 水平変位の逆写像 (固定点反復) による厳密な波高計算
// ワールド座標 (worldX, worldZ) に変位後到達する原位置 (u, v) を逆算
inline float EvaluateWaveHeightIterative(const GerstnerWave* waves, int32_t count, float worldX, float worldZ,
                                        float time, float baseHeight, float overallScale, float timeScale,
                                        int32_t iterations = 2)
{
    float u = worldX;
    float v = worldZ;

    for (int32_t iter = 0; iter < iterations; ++iter) {
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 norm;
        SampleGerstnerWaves(waves, count, u, v, time, baseHeight, overallScale, timeScale, pos, norm);
        // 変位誤差 (pos.x - worldX, pos.z - worldZ) を補正
        u -= (pos.x - worldX);
        v -= (pos.z - worldZ);
    }

    DirectX::XMFLOAT3 finalPos;
    DirectX::XMFLOAT3 finalNorm;
    SampleGerstnerWaves(waves, count, u, v, time, baseHeight, overallScale, timeScale, finalPos, finalNorm);
    return finalPos.y;
}

} // namespace wave
} // namespace mye
