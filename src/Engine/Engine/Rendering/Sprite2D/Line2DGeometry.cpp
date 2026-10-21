//====================================================================================
//                          Line2DGeometry.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          Line2D の帯 (ストローク) と塗りの頂点生成の実装
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Line2DGeometry.h"

#include <cmath>

#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"

using namespace DirectX;

namespace mye {
namespace line2d {
namespace {

// これ以下の距離の隣り合う点は同じ位置とみなす (向きを決められない線分を作らない)
constexpr float kMinSegmentLength = 1.0e-5f;

// 継ぎ目 1 点ぶんの左右の頂点。in = 手前の線分の終端、out = 次の線分の始端 (マイターなら同じ位置)
struct Join {
    XMFLOAT2 inLeft = {};
    XMFLOAT2 inRight = {};
    XMFLOAT2 outLeft = {};
    XMFLOAT2 outRight = {};
    bool bevel = false;
};

XMFLOAT2 Offset(const XMFLOAT2& p, const XMFLOAT2& n, float s)
{
    return { p.x + n.x * s, p.y + n.y * s };
}

} // namespace

void BuildStroke(const XMFLOAT2* points, int count, float width, bool loop, std::vector<StrokeVertex>& out)
{
    out.clear();
    if (points == nullptr || count < 2 || !(width > 0.0f)) {
        return;
    }

    // 同じ位置が続く点を 1 つにまとめる (loop では最後と最初が同位置でも 1 つにする)
    std::vector<XMFLOAT2> p;
    p.reserve(static_cast<size_t>(count));
    const auto samePoint = [](const XMFLOAT2& a, const XMFLOAT2& b) {
        return std::hypot(b.x - a.x, b.y - a.y) <= kMinSegmentLength;
    };
    for (int i = 0; i < count; ++i) {
        if (p.empty() || !samePoint(p.back(), points[i])) {
            p.push_back(points[i]);
        }
    }
    if (loop && p.size() >= 2 && samePoint(p.front(), p.back())) {
        p.pop_back();
    }
    const int n = static_cast<int>(p.size());
    if (n < 2) {
        return;
    }
    const bool closed = loop && n >= 3;
    const int segCount = closed ? n : n - 1;

    // 線分ごとの単位方向、左法線 (方向を反時計回りに 90 度)、始点までの長さ
    std::vector<XMFLOAT2> dir(static_cast<size_t>(segCount));
    std::vector<XMFLOAT2> normal(static_cast<size_t>(segCount));
    std::vector<float> arc(static_cast<size_t>(segCount) + 1, 0.0f);
    for (int s = 0; s < segCount; ++s) {
        const XMFLOAT2& a = p[static_cast<size_t>(s)];
        const XMFLOAT2& b = p[static_cast<size_t>((s + 1) % n)];
        const float len = std::hypot(b.x - a.x, b.y - a.y);
        const XMFLOAT2 d = { (b.x - a.x) / len, (b.y - a.y) / len };
        dir[static_cast<size_t>(s)] = d;
        normal[static_cast<size_t>(s)] = { -d.y, d.x };
        arc[static_cast<size_t>(s) + 1] = arc[static_cast<size_t>(s)] + len;
    }
    const float total = arc[static_cast<size_t>(segCount)];

    // 継ぎ目の頂点。開いた線の両端は線分 1 本の法線そのまま
    const float half = width * 0.5f;
    std::vector<Join> joins(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const bool hasIn = closed || i > 0;
        const bool hasOut = closed || i < n - 1;
        const XMFLOAT2& pt = p[static_cast<size_t>(i)];
        Join& j = joins[static_cast<size_t>(i)];
        const int segIn = closed ? (i + n - 1) % n : i - 1;
        const int segOut = i;
        if (hasIn && hasOut) {
            const XMFLOAT2& n0 = normal[static_cast<size_t>(segIn)];
            const XMFLOAT2& n1 = normal[static_cast<size_t>(segOut)];
            const XMFLOAT2 sum = { n0.x + n1.x, n0.y + n1.y };
            const float sumLen2 = sum.x * sum.x + sum.y * sum.y;
            // |n0 + n1| = 2 cos(角/2)。マイターの長さ = 半幅 / cos(角/2)
            const float cosHalf = std::sqrt(sumLen2) * 0.5f;
            if (cosHalf * kMiterLimit >= 1.0f) {
                const XMFLOAT2 miter = { sum.x * (2.0f * half / sumLen2), sum.y * (2.0f * half / sumLen2) };
                j.inLeft = j.outLeft = { pt.x + miter.x, pt.y + miter.y };
                j.inRight = j.outRight = { pt.x - miter.x, pt.y - miter.y };
            } else {
                j.bevel = true;
                j.inLeft = Offset(pt, n0, half);
                j.inRight = Offset(pt, n0, -half);
                j.outLeft = Offset(pt, n1, half);
                j.outRight = Offset(pt, n1, -half);
            }
        } else if (hasOut) {
            const XMFLOAT2& n1 = normal[static_cast<size_t>(segOut)];
            j.outLeft = Offset(pt, n1, half);
            j.outRight = Offset(pt, n1, -half);
        } else {
            const XMFLOAT2& n0 = normal[static_cast<size_t>(segIn)];
            j.inLeft = Offset(pt, n0, half);
            j.inRight = Offset(pt, n0, -half);
        }
    }

    out.reserve(static_cast<size_t>(segCount) * 6 + static_cast<size_t>(n) * 3);
    const auto push = [&](const XMFLOAT2& pos, float arcLen, float v) {
        out.push_back({ pos.x, pos.y, arcLen / total, v });
    };
    for (int s = 0; s < segCount; ++s) {
        const Join& a = joins[static_cast<size_t>(s)];
        const Join& b = joins[static_cast<size_t>((s + 1) % n)];
        const float u0 = arc[static_cast<size_t>(s)];
        const float u1 = arc[static_cast<size_t>(s) + 1];
        // 反時計回り: 始端の左 → 始端の右 → 終端の右、始端の左 → 終端の右 → 終端の左
        push(a.outLeft, u0, 0.0f);
        push(a.outRight, u0, 1.0f);
        push(b.inRight, u1, 1.0f);
        push(a.outLeft, u0, 0.0f);
        push(b.inRight, u1, 1.0f);
        push(b.inLeft, u1, 0.0f);
    }
    // ベベル: 曲がりの外側の隙間を三角形で埋める (内側は前後の四角形が重なる)
    for (int i = 0; i < n; ++i) {
        const Join& j = joins[static_cast<size_t>(i)];
        if (!j.bevel) {
            continue;
        }
        const XMFLOAT2& d0 = dir[static_cast<size_t>(closed ? (i + n - 1) % n : i - 1)];
        const XMFLOAT2& d1 = dir[static_cast<size_t>(i)];
        const float cross = d0.x * d1.y - d0.y * d1.x;
        const float arcLen = arc[static_cast<size_t>(i)];
        const XMFLOAT2& pt = p[static_cast<size_t>(i)];
        if (cross > 0.0f) {
            // 左に曲がる = 外側は右
            push(pt, arcLen, 0.5f);
            push(j.inRight, arcLen, 1.0f);
            push(j.outRight, arcLen, 1.0f);
        } else {
            push(pt, arcLen, 0.5f);
            push(j.outLeft, arcLen, 0.0f);
            push(j.inLeft, arcLen, 0.0f);
        }
    }
}

void Triangulate(const XMFLOAT2* points, int count, std::vector<uint32_t>& out)
{
    out.clear();
    if (points == nullptr || count < 3) {
        return;
    }
    const std::vector<XMFLOAT2> polygon(points, points + count);
    TriangulatePolygon2D(polygon, out);
}

} // namespace line2d
} // namespace mye
