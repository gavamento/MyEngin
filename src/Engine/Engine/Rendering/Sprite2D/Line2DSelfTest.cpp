//====================================================================================
//                          Line2DSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          Line2D の帯・塗りの頂点生成と描画リストの回帰テスト
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/Line2DSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Rendering/Sprite2D/Line2DGeometry.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"

using namespace DirectX;

namespace mye {
namespace {

bool Approx(float a, float b)
{
    return std::fabs(a - b) < 1.0e-4f;
}

bool AtPoint(const line2d::StrokeVertex& v, float x, float y)
{
    return Approx(v.x, x) && Approx(v.y, y);
}

// 三角形リストの符号つき面積 (反時計回りが正) の最小値。裏返った三角形があれば負になる
float MinTriangleArea(const std::vector<line2d::StrokeVertex>& v)
{
    float minArea = 1.0e30f;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        const float area = 0.5f * ((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y)
                                   - (v[i + 1].y - v[i].y) * (v[i + 2].x - v[i].x));
        minArea = (std::min)(minArea, area);
    }
    return minArea;
}

bool AllFinite(const std::vector<line2d::StrokeVertex>& v)
{
    return std::all_of(v.begin(), v.end(), [](const line2d::StrokeVertex& s) {
        return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.u) && std::isfinite(s.v);
    });
}

// Triangulate の結果の面積の合計 (向きは問わない) と、反時計回りでない三角形の有無
float TriangleAreaSum(const std::vector<XMFLOAT2>& pts, const std::vector<uint32_t>& idx, bool& allCcw)
{
    float sum = 0.0f;
    allCcw = true;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const XMFLOAT2& a = pts[idx[i]];
        const XMFLOAT2& b = pts[idx[i + 1]];
        const XMFLOAT2& c = pts[idx[i + 2]];
        const float area = 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        allCcw = allCcw && area > 0.0f;
        sum += std::fabs(area);
    }
    return sum;
}

// 多角形の面積 (靴紐の公式の絶対値)
float PolygonArea(const std::vector<XMFLOAT2>& pts)
{
    float sum = 0.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        const XMFLOAT2& a = pts[i];
        const XMFLOAT2& b = pts[(i + 1) % pts.size()];
        sum += a.x * b.y - b.x * a.y;
    }
    return std::fabs(sum) * 0.5f;
}

} // namespace

bool RunLine2DSelfTest()
{
    MYE_LOG_INFO("==== Line2D self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) BuildStroke ----
    {
        std::vector<line2d::StrokeVertex> out;
        // 緩やかな 5 点: 継ぎ目は全部マイター = 線分 4 本 x 6 頂点。端は平ら (法線方向に半幅だけ)
        const XMFLOAT2 five[5] = { { 0, 0 }, { 1, 0 }, { 2, 0.5f }, { 3, 0 }, { 4, 0 } };
        line2d::BuildStroke(five, 5, 0.2f, false, out);
        check(out.size() == 24, "Stroke: 5 points open = 4 segments x 6 vertices");
        check(out.size() == 24 && AtPoint(out[0], 0.0f, 0.1f) && AtPoint(out[1], 0.0f, -0.1f)
                  && out[0].v == 0.0f && out[1].v == 1.0f,
              "Stroke: the start is flat (left +0.1, right -0.1 at x = 0)");
        check(out.size() == 24 && AtPoint(out[20], 4.0f, -0.1f) && Approx(out[20].u, 1.0f) && Approx(out[0].u, 0.0f),
              "Stroke: the end is flat and u runs 0 .. 1");
        check(AllFinite(out) && MinTriangleArea(out) > 0.0f, "Stroke: every triangle is counter-clockwise");

        // 一直線: 継ぎ目も幅は変わらない
        const XMFLOAT2 straight[3] = { { 0, 0 }, { 1, 0 }, { 2, 0 } };
        line2d::BuildStroke(straight, 3, 0.2f, false, out);
        bool flat = out.size() == 12;
        for (const line2d::StrokeVertex& v : out) {
            flat = flat && Approx(std::fabs(v.y), 0.1f);
        }
        check(flat, "Stroke: a straight line keeps the half width at the joints");

        // 直角: マイターの点は外側 (1.1, -0.1) と内側 (0.9, 0.1)。前後の線分が同じ点を共有する
        const XMFLOAT2 corner[3] = { { 0, 0 }, { 1, 0 }, { 1, 1 } };
        line2d::BuildStroke(corner, 3, 0.2f, false, out);
        check(out.size() == 12 && AtPoint(out[2], 1.1f, -0.1f) && AtPoint(out[5], 0.9f, 0.1f)
                  && AtPoint(out[6], 0.9f, 0.1f) && AtPoint(out[7], 1.1f, -0.1f),
              "Stroke: a right angle is a miter shared by both segments");
        check(MinTriangleArea(out) > 0.0f, "Stroke: the right-angle miter has no flipped triangle");

        // 鋭すぎる角 (上限を超える) はベベル: 線分ごとの法線の点 + 外側の三角形 1 枚
        const XMFLOAT2 sharp[3] = { { 0, 0 }, { 1, 0 }, { 0, 0.1f } };
        line2d::BuildStroke(sharp, 3, 0.2f, false, out);
        check(out.size() == 15 && AtPoint(out[12], 1.0f, 0.0f) && AtPoint(out[13], 1.0f, -0.1f),
              "Stroke: a sharp angle is a bevel (2 quads + 1 triangle at the outer side)");
        check(AllFinite(out) && MinTriangleArea(out) > 0.0f, "Stroke: the bevel triangle is counter-clockwise");

        // 折り返し (180 度) もベベルで有限の値
        const XMFLOAT2 reverse[3] = { { 0, 0 }, { 1, 0 }, { 0, 0 } };
        line2d::BuildStroke(reverse, 3, 0.2f, false, out);
        check(out.size() == 15 && AllFinite(out), "Stroke: a 180 degree turn is a bevel with finite vertices");

        // loop: 最後の線分の終端が最初の線分の始端と同じ点 (継ぎ目に隙間も重なりもない)
        const XMFLOAT2 square[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        line2d::BuildStroke(square, 4, 0.2f, true, out);
        check(out.size() == 24, "Stroke: 4 points loop = 4 segments x 6 vertices");
        check(out.size() == 24 && AtPoint(out[0], 0.1f, 0.1f) && AtPoint(out[23], 0.1f, 0.1f)
                  && AtPoint(out[1], -0.1f, -0.1f) && AtPoint(out[20], -0.1f, -0.1f),
              "Stroke: the loop seam is one shared miter (last segment end == first segment start)");
        check(out.size() == 24 && Approx(out[20].u, 1.0f) && Approx(out[0].u, 0.0f) && MinTriangleArea(out) > 0.0f,
              "Stroke: the loop u runs 0 .. 1 and the seam triangles are counter-clockwise");
        line2d::BuildStroke(square, 4, 0.2f, false, out);
        check(out.size() == 18 && AtPoint(out[0], 0.0f, 0.1f), "Stroke: without loop the same points are open (3 segments, flat start)");
        // 最後の点が最初の点と同じ位置の loop は、点を 1 つにまとめて同じ形
        const XMFLOAT2 closedTwice[5] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { 0, 0 } };
        line2d::BuildStroke(closedTwice, 5, 0.2f, true, out);
        check(out.size() == 24, "Stroke: a loop whose last point repeats the first is the same loop");

        // 境界: 0 / 1 / 2 点
        line2d::BuildStroke(five, 0, 0.2f, false, out);
        check(out.empty(), "Stroke: 0 points draw nothing");
        line2d::BuildStroke(five, 1, 0.2f, false, out);
        check(out.empty(), "Stroke: 1 point draws nothing");
        line2d::BuildStroke(five, 2, 0.2f, false, out);
        check(out.size() == 6, "Stroke: 2 points = 1 segment (6 vertices)");
        line2d::BuildStroke(five, 2, 0.2f, true, out);
        check(out.size() == 6, "Stroke: a loop of 2 points is an open single segment");
        // 底角 26.6 度の二等辺三角形: 3 線分 x 6 + 鋭い底角 2 か所のベベル x 3 (頂角 126.9 度はマイター)
        const XMFLOAT2 triangle[3] = { { 0, 0 }, { 4, 0 }, { 2, 1 } };
        line2d::BuildStroke(triangle, 3, 0.2f, true, out);
        check(out.size() == 24 && AllFinite(out), "Stroke: a loop of 3 points = 3 segments (+ bevels at the sharp corners)");
        line2d::BuildStroke(five, 5, 0.0f, false, out);
        check(out.empty(), "Stroke: width 0 draws nothing");
        line2d::BuildStroke(nullptr, 5, 0.2f, false, out);
        check(out.empty(), "Stroke: null points draw nothing");
        const XMFLOAT2 same[3] = { { 1, 1 }, { 1, 1 }, { 1, 1 } };
        line2d::BuildStroke(same, 3, 0.2f, false, out);
        check(out.empty(), "Stroke: all points at one position draw nothing");
        const XMFLOAT2 dup[3] = { { 0, 0 }, { 0, 0 }, { 1, 0 } };
        line2d::BuildStroke(dup, 3, 0.2f, false, out);
        check(out.size() == 6 && AtPoint(out[0], 0.0f, 0.1f), "Stroke: repeated points are merged (1 segment)");
    }

    // ---- (b) Triangulate ----
    {
        std::vector<uint32_t> idx;
        bool ccw = false;
        const std::vector<XMFLOAT2> convex = { { 0, 0 }, { 2, 0 }, { 3, 1.5f }, { 1, 3 }, { -1, 1.5f } };
        line2d::Triangulate(convex.data(), 5, idx);
        const float convexArea = PolygonArea(convex);
        check(idx.size() == 9, "Triangulate: a convex pentagon = n - 2 = 3 triangles");
        check(Approx(TriangleAreaSum(convex, idx, ccw), convexArea) && ccw, "Triangulate: the convex triangles cover the polygon exactly");
        std::vector<XMFLOAT2> convexCw(convex.rbegin(), convex.rend());
        line2d::Triangulate(convexCw.data(), 5, idx);
        check(idx.size() == 9 && Approx(TriangleAreaSum(convexCw, idx, ccw), convexArea),
              "Triangulate: a clockwise polygon gives the same triangles");

        const std::vector<XMFLOAT2> concave = { { 0, 0 }, { 4, 0 }, { 4, 4 }, { 2, 1 }, { 0, 4 } };
        line2d::Triangulate(concave.data(), 5, idx);
        check(idx.size() == 9 && Approx(TriangleAreaSum(concave, idx, ccw), 10.0f) && ccw,
              "Triangulate: a concave pentagon = 3 triangles and no triangle covers the dent (area 10)");
        std::vector<XMFLOAT2> concaveCw(concave.rbegin(), concave.rend());
        line2d::Triangulate(concaveCw.data(), 5, idx);
        check(idx.size() == 9 && Approx(TriangleAreaSum(concaveCw, idx, ccw), 10.0f), "Triangulate: the clockwise concave polygon too");
        line2d::Triangulate(concave.data(), 3, idx);
        check(idx.size() == 3, "Triangulate: 3 points = 1 triangle");
        line2d::Triangulate(concave.data(), 2, idx);
        check(idx.empty(), "Triangulate: 2 points = nothing");
        line2d::Triangulate(concave.data(), 0, idx);
        check(idx.empty(), "Triangulate: 0 points = nothing");
    }

    // ---- (c) コンポーネントの登録 ----
    {
        RegisterBuiltinComponents();
        check(Line2DComponent::sTypeId == 88, "Line2D is TypeId 88");
        const ComponentDesc& d = ComponentRegistry::Get().Desc(Line2DComponent::sTypeId);
        check((d.flags & kComponentNoHash) != 0, "Line2D is kComponentNoHash (render-only)");
        const Line2DComponent defaults;
        check(defaults.pointCount == 2 && defaults.width == 0.1f && !defaults.loop && !defaults.fill
                  && defaults.texture.IsNull() && defaults.color.w == 1.0f,
              "Line2D defaults: 2 points, width 0.1, white, no loop / fill");
        bool allPoints = true;
        for (uint32_t i = 0; i < static_cast<uint32_t>(kLine2DMaxPoints); ++i) {
            const std::string name = "point" + std::to_string(i);
            bool found = false;
            for (const FieldDesc& f : d.fields) {
                if (name == f.name) {
                    found = f.type == FieldType::Float2
                        && f.offset == offsetof(Line2DComponent, points) + i * sizeof(XMFLOAT2);
                }
            }
            allPoints = allPoints && found;
        }
        check(allPoints, "Line2D: point0 .. point31 are registered at points[i]");
    }

    // ---- (d) 描画リスト ----
    {
        GraphicsDevice device;
        check(device.Init(true), "Line2D collect test: WARP device init");
        RenderResources resources;
        resources.textures.Init(device);
        Sprite2DRenderer renderer;
        const AssetID white = resources.textures.White();

        World w;
        const EntityID e = w.CreateEntity("line");
        w.AddComponent<Line2DComponent>(e);
        auto* ln = w.GetComponent<Line2DComponent>(e);
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.batches.size() == 1 && l.vertices.size() == 6 && l.batches[0].texture == white
                      && !l.batches[0].lit && !l.HasLights() && l.batches[0].material.IsNull(),
                  "Collect: a default Line2D is 1 batch of 6 vertices, white, unlit (no light in the scene)");
            check(Approx(l.vertices[0].pos[0], -1.0f) && Approx(l.vertices[0].pos[1], 0.05f)
                      && l.vertices[0].color[0] == 1.0f && l.vertices[0].color[3] == 1.0f,
                  "Collect: the vertices are the local stroke (x -1, y 0.05) with the line color");
        }
        ln->pointCount = 0;
        check(renderer.Collect(w, resources).Empty(), "Collect: pointCount 0 draws nothing");
        ln->pointCount = 1;
        check(renderer.Collect(w, resources).Empty(), "Collect: pointCount 1 draws nothing");
        ln->pointCount = 2;
        check(renderer.Collect(w, resources).vertices.size() == 6, "Collect: pointCount 2 draws 1 segment");
        ln->pointCount = 500;
        check(!renderer.Collect(w, resources).Empty(), "Collect: an out-of-range pointCount is clamped to the 32 points");
        ln->pointCount = 2;
        ln->width = 0.0f;
        check(renderer.Collect(w, resources).Empty(), "Collect: width 0 without fill draws nothing");

        // 凹五角形: 塗り (n - 2 = 3 枚 = 9 頂点) が先、帯 (loop の 5 本) が後。塗りの UV = (x, -y)
        const XMFLOAT2 concave[5] = { { 0, 0 }, { 4, 0 }, { 4, 4 }, { 2, 1 }, { 0, 4 } };
        ln->pointCount = 5;
        std::copy(concave, concave + 5, ln->points);
        ln->fill = true;
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.batches.size() == 1 && l.vertices.size() == 9, "Collect: fill only (width 0) = 3 triangles");
            std::vector<uint32_t> idx;
            line2d::Triangulate(concave, 5, idx);
            const XMFLOAT2& p0 = concave[idx[0]];
            check(l.vertices.size() == 9 && Approx(l.vertices[0].pos[0], p0.x) && Approx(l.vertices[0].pos[1], p0.y)
                      && Approx(l.vertices[0].uv[0], p0.x) && Approx(l.vertices[0].uv[1], -p0.y),
                  "Collect: the fill uses local position / 1 unit as UV (v flipped so the image is upright)");
        }
        ln->width = 0.2f;
        ln->loop = true;
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            std::vector<line2d::StrokeVertex> stroke;
            line2d::BuildStroke(concave, 5, 0.2f, true, stroke);
            check(l.batches.size() == 1 && l.vertices.size() == 9 + stroke.size() && !stroke.empty(),
                  "Collect: fill + loop stroke are one batch (fill first, then the stroke)");
        }
        ln->pointCount = 2;
        check(renderer.Collect(w, resources).vertices.size() == 6, "Collect: fill with < 3 points draws only the stroke");

        // ワールド行列は位置に掛かる
        ln->fill = false;
        ln->loop = false;
        ln->width = 0.1f;
        ln->points[0] = { -1.0f, 0.0f };
        ln->points[1] = { 1.0f, 0.0f };
        w.GetComponent<WorldMatrixComponent>(e)->value._41 = 2.0f;
        w.GetComponent<WorldMatrixComponent>(e)->value._42 = 3.0f;
        {
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.vertices.size() == 6 && Approx(l.vertices[0].pos[0], 1.0f) && Approx(l.vertices[0].pos[1], 3.05f),
                  "Collect: the world matrix moves the line (translation (2, 3))");
        }
        w.GetComponent<WorldMatrixComponent>(e)->value._41 = 0.0f;
        w.GetComponent<WorldMatrixComponent>(e)->value._42 = 0.0f;

        // Sprite2D と同じソートキー: 層内の順序が小さい方が先 (別バッチ = テクスチャが違う)
        for (int spriteFirst = 1; spriteFirst >= 0; --spriteFirst) {
            World w2;
            const EntityID lineEntity = w2.CreateEntity("line");
            w2.AddComponent<Line2DComponent>(lineEntity);
            const EntityID spriteEntity = w2.CreateEntity("sprite");
            w2.AddComponent<Sprite2DComponent>(spriteEntity);
            w2.GetComponent<Line2DComponent>(lineEntity)->orderInLayer = 1;
            w2.GetComponent<Sprite2DComponent>(spriteEntity)->orderInLayer = spriteFirst != 0 ? 0 : 2;
            const Sprite2DDrawList& l = renderer.Collect(w2, resources);
            const size_t lineIndex = spriteFirst != 0 ? 1 : 0;
            check(l.batches.size() == 2 && l.batches[lineIndex].texture == white && l.batches[1 - lineIndex].texture != white,
                  spriteFirst != 0 ? "Collect: Line2D order 1 is drawn after a Sprite2D of order 0"
                                   : "Collect: Line2D order 1 is drawn before a Sprite2D of order 2");
        }

        // ライトがあるシーンでは lit (Tilemap と同じ)
        {
            World w3;
            const EntityID lineEntity = w3.CreateEntity("line");
            w3.AddComponent<Line2DComponent>(lineEntity);
            const EntityID lightEntity = w3.CreateEntity("light");
            w3.AddComponent<Light2DComponent>(lightEntity);
            const Sprite2DDrawList& l = renderer.Collect(w3, resources);
            check(l.HasLights() && l.batches.size() == 1 && l.batches[0].lit && l.lightGroups[0].anyLit,
                  "Collect: with a Light2D in the scene the Line2D batch is lit");
        }        device.Shutdown();
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Line2D self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Line2D self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
