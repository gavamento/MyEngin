#pragma once
#include <cmath>

#include <DirectXMath.h>

// 視錐台カリング (M16)。描画専用の可視判定でありワールドハッシュには一切関与しない
// (RenderQueue は sim 状態でないため、カリングの有無で決定論は変わらない)。
// M25 のジョブ並列カリングでも再利用する純関数群。
namespace mye {

// CSM のカスケード分割 (M38d、practical split)。out[i] = カスケード i の far 境界 (view 深度)。
// s_i = λ·(n·(f/n)^(i/N)) + (1−λ)·(n + (f−n)·i/N)。λ=0.5 が log/linear の折衷。
// 純関数 (RenderSelfTest 対象)。
inline void ComputeCascadeSplits(float nearZ, float farZ, int count, float lambda, float* out)
{
    for (int i = 1; i <= count; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(count);
        const float logSplit = nearZ * std::pow(farZ / nearZ, p);
        const float linSplit = nearZ + (farZ - nearZ) * p;
        out[i - 1] = lambda * logSplit + (1.0f - lambda) * linSplit;
    }
}

// ビュー射影行列から抽出した視錐台 6 平面 (Gribb-Hartmann、行ベクトル規約 clip = v*M)。
// 各平面 (a,b,c,d) に対し点 (x,y,z) が a*x+b*y+c*z+d >= 0 なら内側。
// 正規化は不要 (内外判定は符号のみ。正のスケールでは符号が不変)。
struct Frustum {
    DirectX::XMFLOAT4 planes[6] = {};
};

inline Frustum BuildFrustum(const DirectX::XMFLOAT4X4& m)
{
    using DirectX::XMFLOAT4;
    // clip = v * M の各成分は (v,1)·col_k。col0=x, col1=y, col2=z, col3=w。
    const XMFLOAT4 c0 = { m._11, m._21, m._31, m._41 };
    const XMFLOAT4 c1 = { m._12, m._22, m._32, m._42 };
    const XMFLOAT4 c2 = { m._13, m._23, m._33, m._43 };
    const XMFLOAT4 c3 = { m._14, m._24, m._34, m._44 };
    auto add = [](const XMFLOAT4& a, const XMFLOAT4& b) {
        return XMFLOAT4{ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w };
    };
    auto sub = [](const XMFLOAT4& a, const XMFLOAT4& b) {
        return XMFLOAT4{ a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w };
    };
    Frustum f;
    f.planes[0] = add(c3, c0); // left   ( x + w >= 0)
    f.planes[1] = sub(c3, c0); // right  (-x + w >= 0)
    f.planes[2] = add(c3, c1); // bottom ( y + w >= 0)
    f.planes[3] = sub(c3, c1); // top    (-y + w >= 0)
    f.planes[4] = c2;          // near   ( z     >= 0)  D3D クリップ空間 z∈[0,w]
    f.planes[5] = sub(c3, c2); // far    (-z + w >= 0)
    return f;
}

// ローカル AABB をワールド行列で変換した AABB が視錐台と交差するか (p-vertex 保守テスト)。
// 完全に外側のときのみ false。跨ぐ / 内側は true を返し、可視物を決して落とさない。
inline bool AabbInFrustum(const Frustum& f, const DirectX::XMFLOAT4X4& m,
                          const DirectX::XMFLOAT3& lmin, const DirectX::XMFLOAT3& lmax)
{
    using DirectX::XMFLOAT3;
    using DirectX::XMFLOAT4;
    const XMFLOAT3 lc = { (lmin.x + lmax.x) * 0.5f, (lmin.y + lmax.y) * 0.5f,
                          (lmin.z + lmax.z) * 0.5f };
    const XMFLOAT3 le = { (lmax.x - lmin.x) * 0.5f, (lmax.y - lmin.y) * 0.5f,
                          (lmax.z - lmin.z) * 0.5f };
    // ワールド中心 (行ベクトル: wc = lc * M) と絶対値 3x3 による world 半径
    const XMFLOAT3 wc = {
        lc.x * m._11 + lc.y * m._21 + lc.z * m._31 + m._41,
        lc.x * m._12 + lc.y * m._22 + lc.z * m._32 + m._42,
        lc.x * m._13 + lc.y * m._23 + lc.z * m._33 + m._43,
    };
    const XMFLOAT3 we = {
        std::fabs(m._11) * le.x + std::fabs(m._21) * le.y + std::fabs(m._31) * le.z,
        std::fabs(m._12) * le.x + std::fabs(m._22) * le.y + std::fabs(m._32) * le.z,
        std::fabs(m._13) * le.x + std::fabs(m._23) * le.y + std::fabs(m._33) * le.z,
    };
    for (int i = 0; i < 6; ++i) {
        const XMFLOAT4& p = f.planes[i];
        // p-vertex: 平面法線方向に最も遠い AABB 頂点
        const float px = wc.x + (p.x >= 0.0f ? we.x : -we.x);
        const float py = wc.y + (p.y >= 0.0f ? we.y : -we.y);
        const float pz = wc.z + (p.z >= 0.0f ? we.z : -we.z);
        if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) {
            return false; // この平面の外側 → 完全に視錐台外
        }
    }
    return true;
}

// ローカル AABB をワールド行列で変換した world AABB (絶対値 3x3 法) を out に返す (M17 シャドウ範囲用)。
inline void WorldAabb(const DirectX::XMFLOAT4X4& m, const DirectX::XMFLOAT3& lmin,
                      const DirectX::XMFLOAT3& lmax, DirectX::XMFLOAT3& outMin,
                      DirectX::XMFLOAT3& outMax)
{
    using DirectX::XMFLOAT3;
    const XMFLOAT3 lc = { (lmin.x + lmax.x) * 0.5f, (lmin.y + lmax.y) * 0.5f,
                          (lmin.z + lmax.z) * 0.5f };
    const XMFLOAT3 le = { (lmax.x - lmin.x) * 0.5f, (lmax.y - lmin.y) * 0.5f,
                          (lmax.z - lmin.z) * 0.5f };
    const XMFLOAT3 wc = {
        lc.x * m._11 + lc.y * m._21 + lc.z * m._31 + m._41,
        lc.x * m._12 + lc.y * m._22 + lc.z * m._32 + m._42,
        lc.x * m._13 + lc.y * m._23 + lc.z * m._33 + m._43,
    };
    const XMFLOAT3 we = {
        std::fabs(m._11) * le.x + std::fabs(m._21) * le.y + std::fabs(m._31) * le.z,
        std::fabs(m._12) * le.x + std::fabs(m._22) * le.y + std::fabs(m._32) * le.z,
        std::fabs(m._13) * le.x + std::fabs(m._23) * le.y + std::fabs(m._33) * le.z,
    };
    outMin = { wc.x - we.x, wc.y - we.y, wc.z - we.z };
    outMax = { wc.x + we.x, wc.y + we.y, wc.z + we.z };
}

} // namespace mye
