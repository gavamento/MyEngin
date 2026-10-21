//====================================================================================
//                          SpriteShapeSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          SpriteShape2D の輪郭・縁・塗り・衝突の回帰テスト
//====================================================================================
#include "Engine/Engine/Rendering/Sprite2D/SpriteShapeSelfTest.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Collider/Shape2DLibrary.h"
#include "Engine/Engine/Physics/Collider/SpriteShapeCollider.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteShape.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Renderer/Device/GpuResources.h"
#include "Engine/Renderer/Device/GraphicsDevice.h"

using namespace DirectX;

namespace mye {

using spriteshape::EdgeQuad;
using spriteshape::EdgeSpriteInfo;

namespace {

constexpr float kDt = 1.0f / 60.0f;

bool Approx(float a, float b, float eps = 1.0e-4f)
{
    return std::fabs(a - b) < eps;
}

// 外向き法線の角度が 4 つの区間に入るプロファイルの縁: 0 = 上面 (-45..45)、1 = 左の壁 (45..135)、2 = 右の壁 (-135..-45)
std::vector<SpriteShapeEdge> ThreeEdges()
{
    return { { -45.0f, 45.0f, 11 }, { 45.0f, 135.0f, 12 }, { -135.0f, -45.0f, 13 } };
}

// 幅 1 のスプライト (UV 全面) を 3 縁ぶん
std::vector<EdgeSpriteInfo> UnitSprites()
{
    EdgeSpriteInfo info;
    info.width = 1.0f;
    return { info, info, info };
}

size_t CountEdge(const std::vector<EdgeQuad>& quads, int edge)
{
    return static_cast<size_t>(std::count_if(quads.begin(), quads.end(), [edge](const EdgeQuad& q) { return q.edge == edge; }));
}

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

float FillArea(const std::vector<spriteshape::FillVertex>& v)
{
    float sum = 0.0f;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        sum += 0.5f * std::fabs((v[i + 1].x - v[i].x) * (v[i + 2].y - v[i].y) - (v[i + 1].y - v[i].y) * (v[i + 2].x - v[i].x));
    }
    return sum;
}

// 丘: 左上から右下へ下り、谷の先で少し上る 8 点 (開いた滑らかな線)。球が転がり落ちる
constexpr int kHillPointCount = 8;
const XMFLOAT2 kHillPoints[kHillPointCount] = { { -4.0f, 3.0f }, { -3.0f, 2.7f }, { -2.0f, 2.0f }, { -1.0f, 1.2f },
                                                { 0.0f, 0.6f },  { 1.0f, 0.3f },  { 2.5f, 0.2f }, { 4.0f, 0.8f } };

// 折れ線 (ローカル = ワールド) の x での高さ。範囲外は端の高さ
float SurfaceY(const std::vector<XMFLOAT2>& poly, float x)
{
    if (x <= poly.front().x) {
        return poly.front().y;
    }
    for (size_t i = 0; i + 1 < poly.size(); ++i) {
        if (x <= poly[i + 1].x) {
            const float t = (x - poly[i].x) / (poly[i + 1].x - poly[i].x);
            return poly[i].y + (poly[i + 1].y - poly[i].y) * t;
        }
    }
    return poly.back().y;
}

// 供給口の Collect だけを物理へ渡す (CollectWithin の既定 = 絞らない Collect)。絞り込みの有無を比べる用
class UnculledShapes final : public StaticShapeSource {
public:
    explicit UnculledShapes(SpriteShapeColliders& inner) : inner_(inner) {}
    void Collect(World& world, std::vector<StaticShape>& out) override { inner_.Collect(world, out); }

private:
    SpriteShapeColliders& inner_;
};

// 丘 + 球 1 個の sim 1 本ぶん
struct HillSim {
    Scene scene;
    PhysicsSystem phys;
    SpriteShapeColliders colliders;
    UnculledShapes unculled{ colliders };
    EntityID hill = kNullEntity;
    EntityID ball = kNullEntity;

    explicit HillSim(bool culled)
    {
        if (culled) {
            phys.AddStaticShapeSource(&colliders);
        } else {
            phys.AddStaticShapeSource(&unculled);
        }
        GameObject h = scene.CreateGameObjectTracked("Hill");
        SpriteShape2DComponent* sh = h.AddComponent<SpriteShape2DComponent>();
        sh->pointCount = kHillPointCount;
        std::copy(kHillPoints, kHillPoints + kHillPointCount, sh->points);
        sh->smooth = true;
        sh->collider = true;
        hill = h.Id();
        GameObject b = scene.CreateGameObjectTracked("Ball");
        b.SetLocalPosition(-3.5f, 4.5f, 0.0f);
        auto* col = b.AddComponent<ColliderComponent>();
        col->shape = collidershape::kSphere;
        col->isTrigger = false;
        col->radius = 0.25f;
        auto* rb = b.AddComponent<RigidbodyComponent>();
        rb->mass = 1.0f;
        rb->plane2D = true;
        ball = b.Id();
        scene.GetWorld().ApplyStructuralChanges();
    }
    HillSim(const HillSim&) = delete;
    HillSim& operator=(const HillSim&) = delete;

    World& GetWorld() { return scene.GetWorld(); }
    XMFLOAT3 BallPos() { return GetWorld().GetComponent<LocalTransform>(ball)->position; }
    uint64_t Hash() { return HashWorld(GetWorld(), SimSourcesOf(scene, nullptr, nullptr, nullptr)); }
};

} // namespace

bool RunSpriteShapeSelfTest()
{
    MYE_LOG_INFO("==== SpriteShape2D self test ====");
    int failCount = 0;
    const auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- (a) EvaluateSpline ----
    {
        std::vector<XMFLOAT2> out;
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, true, out);
        check(out.size() == static_cast<size_t>((kHillPointCount - 1) * 8 + 1), "Spline: 8 points open smooth = 7 spans x 8 + 1 = 57 points");
        bool onControl = true;
        for (int k = 0; k < kHillPointCount; ++k) {
            const size_t at = (k == kHillPointCount - 1) ? out.size() - 1 : static_cast<size_t>(k) * 8;
            onControl = onControl && out[at].x == kHillPoints[k].x && out[at].y == kHillPoints[k].y;
        }
        check(onControl, "Spline: the curve passes exactly through every control point (including both ends)");

        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, true, true, out);
        check(out.size() == static_cast<size_t>(kHillPointCount * 8), "Spline: 8 points closed smooth = 8 spans x 8 = 64 points (the first point is not repeated)");
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, false, out);
        check(out.size() == static_cast<size_t>(kHillPointCount) && out[3].x == kHillPoints[3].x, "Spline: smooth off returns the points as they are");
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, true, false, out);
        check(out.size() == static_cast<size_t>(kHillPointCount), "Spline: closed without smooth does not repeat the first point either");
        spriteshape::EvaluateSpline(kHillPoints, 2, false, true, out);
        check(out.size() == 9, "Spline: 2 points smooth = 1 span x 8 + 1 = 9 points");
        spriteshape::EvaluateSpline(kHillPoints, 2, true, true, out);
        check(out.size() == 9, "Spline: closed needs 3 points; 2 points stay an open line");
        spriteshape::EvaluateSpline(kHillPoints, 1, false, true, out);
        check(out.size() == 1, "Spline: 1 point is returned as it is");
        spriteshape::EvaluateSpline(kHillPoints, 0, false, true, out);
        check(out.empty(), "Spline: 0 points = nothing");
        spriteshape::EvaluateSpline(nullptr, 5, false, true, out);
        check(out.empty(), "Spline: null points = nothing");

        // 一直線上の点は曲がらない
        const XMFLOAT2 line[3] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 2.0f, 1.0f } };
        spriteshape::EvaluateSpline(line, 3, false, true, out);
        bool flat = true;
        for (const XMFLOAT2& p : out) {
            flat = flat && p.y == 1.0f;
        }
        check(flat, "Spline: collinear points stay on the line");
        // 2 回評価して同じビット (決定論)
        std::vector<XMFLOAT2> again;
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, true, out);
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, true, again);
        check(out.size() == again.size() && std::equal(out.begin(), out.end(), again.begin(), [](const XMFLOAT2& a, const XMFLOAT2& b) { return a.x == b.x && a.y == b.y; }),
              "Spline: evaluating twice gives identical bits");
    }

    // ---- (b) 縁の選択 ----
    {
        const std::vector<SpriteShapeEdge> edges = ThreeEdges();
        check(spriteshape::SelectEdge(edges, 0.0f) == 0 && spriteshape::SelectEdge(edges, 44.9f) == 0 && spriteshape::SelectEdge(edges, -44.9f) == 0,
              "SelectEdge: angles around 0 pick the top edge");
        check(spriteshape::SelectEdge(edges, 90.0f) == 1 && spriteshape::SelectEdge(edges, -90.0f) == 2, "SelectEdge: +90 is the left wall, -90 the right wall");
        check(spriteshape::SelectEdge(edges, 180.0f) == -1 && spriteshape::SelectEdge(edges, -180.0f) == -1 && spriteshape::SelectEdge(edges, 170.0f) == -1,
              "SelectEdge: the bottom (no edge covers it) picks nothing");
        check(spriteshape::SelectEdge(edges, 45.0f) == 0, "SelectEdge: a shared boundary angle goes to the first matching edge (array order)");
        check(spriteshape::SelectEdge(edges, 405.0f) == 0 && spriteshape::SelectEdge(edges, -270.0f) == 1, "SelectEdge: angles are folded into -180..180");
        check(spriteshape::SelectEdge(edges, std::numeric_limits<float>::quiet_NaN()) == -1, "SelectEdge: NaN picks nothing");
        const std::vector<SpriteShapeEdge> wrap = { { 135.0f, 225.0f, 1 } };
        check(spriteshape::SelectEdge(wrap, 180.0f) == 0 && spriteshape::SelectEdge(wrap, -170.0f) == 0 && spriteshape::SelectEdge(wrap, 140.0f) == 0 && spriteshape::SelectEdge(wrap, 100.0f) == -1,
              "SelectEdge: a range across 180 degrees (135..225) also covers -170");
        const std::vector<SpriteShapeEdge> swapped = { { 45.0f, -45.0f, 1 } };
        check(spriteshape::SelectEdge(swapped, 0.0f) == 0, "SelectEdge: angleFrom > angleTo is swapped");
        check(spriteshape::SelectEdge({}, 0.0f) == -1, "SelectEdge: no edges picks nothing");
        check(Approx(spriteshape::OutwardAngleDeg(0.0f, 1.0f), 0.0f) && Approx(spriteshape::OutwardAngleDeg(-1.0f, 0.0f), 90.0f)
                  && Approx(spriteshape::OutwardAngleDeg(1.0f, 0.0f), -90.0f)
                  && Approx(std::fabs(spriteshape::OutwardAngleDeg(0.0f, -1.0f)), 180.0f),
              "OutwardAngleDeg: up = 0, left = +90, right = -90, down = 180");
    }

    // ---- (c) BuildEdgeQuads ----
    {
        const std::vector<SpriteShapeEdge> edges = ThreeEdges();
        const std::vector<EdgeSpriteInfo> sprites = UnitSprites();
        std::vector<EdgeQuad> quads;

        // 左から右へ長さ 3 の開いた線: 進行方向の左 (上) が外側 = 上面の縁を 3 枚、帯は線を中心に厚み 1
        const std::vector<XMFLOAT2> flat = { { 0.0f, 0.0f }, { 3.0f, 0.0f } };
        bool capped = spriteshape::BuildEdgeQuads(flat, false, edges, sprites, 1.0f, 100, quads);
        check(!capped && quads.size() == 3 && CountEdge(quads, 0) == 3, "Quads: a flat 3 m open line left to right = 3 top-edge quads");
        check(quads.size() == 3 && Approx(quads[0].x[0], 0.0f) && Approx(quads[0].y[0], 0.5f) && Approx(quads[0].x[1], 1.0f)
                  && Approx(quads[0].y[2], -0.5f) && Approx(quads[0].x[3], 0.0f),
              "Quads: corners are TL (0, 0.5) / TR (1, 0.5) / BR (1, -0.5) / BL (0, -0.5): the band is centered on the line");
        check(quads.size() == 3 && quads[0].u[0] == 0.0f && quads[0].u[1] == 1.0f && quads[0].v[0] == 0.0f && quads[0].v[2] == 1.0f,
              "Quads: u runs 0..1 per sprite width, the top of the sprite (v 0) is the outer side");
        // 端数: 長さ 2.5 -> 3 枚、最後は UV を半分に切る
        const std::vector<XMFLOAT2> partial = { { 0.0f, 0.0f }, { 2.5f, 0.0f } };
        spriteshape::BuildEdgeQuads(partial, false, edges, sprites, 1.0f, 100, quads);
        check(quads.size() == 3 && Approx(quads[2].u[1], 0.5f) && Approx(quads[2].x[1], 2.5f) && Approx(quads[2].x[0], 2.0f),
              "Quads: the last quad of a 2.5 m run is clipped to half the sprite (u 0..0.5)");
        // 右から左へ描くと外側が下 = どの縁にも当たらない (開いた線は進行方向の左が表)
        const std::vector<XMFLOAT2> reversed = { { 3.0f, 0.0f }, { 0.0f, 0.0f } };
        spriteshape::BuildEdgeQuads(reversed, false, edges, sprites, 1.0f, 100, quads);
        check(quads.empty(), "Quads: an open line drawn right to left faces down (180 degrees) and has no edge");

        // 閉じた四角形 (反時計回りで与える): 上辺 = 縁 0、左辺 = 縁 1、右辺 = 縁 2、下辺 = 縁なし。各辺 4 枚
        const std::vector<XMFLOAT2> square = { { 0.0f, 0.0f }, { 4.0f, 0.0f }, { 4.0f, 4.0f }, { 0.0f, 4.0f } };
        spriteshape::BuildEdgeQuads(square, true, edges, sprites, 1.0f, 200, quads);
        check(quads.size() == 12 && CountEdge(quads, 0) == 4 && CountEdge(quads, 1) == 4 && CountEdge(quads, 2) == 4,
              "Quads: a closed square = top / left / right sides 4 quads each, the bottom has no edge");
        // 時計回りで与えても同じ (向きを揃えるので外側は同じ)
        const std::vector<XMFLOAT2> squareCw(square.rbegin(), square.rend());
        std::vector<EdgeQuad> quadsCw;
        spriteshape::BuildEdgeQuads(squareCw, true, edges, sprites, 1.0f, 200, quadsCw);
        check(quadsCw.size() == 12 && CountEdge(quadsCw, 0) == 4 && CountEdge(quadsCw, 1) == 4 && CountEdge(quadsCw, 2) == 4,
              "Quads: the same square given clockwise gives the same edges (outline orientation is normalized)");
        // 最初の点が末尾に重なっていても同じ
        std::vector<XMFLOAT2> squareRepeated = square;
        squareRepeated.push_back(square[0]);
        spriteshape::BuildEdgeQuads(squareRepeated, true, edges, sprites, 1.0f, 200, quads);
        check(quads.size() == 12, "Quads: a closed outline whose last point repeats the first is the same outline");

        // 位相の引き継ぎ: 長さ 0.4 の同じ縁が 5 本続く -> u が 0.4 刻みで連続し、スプライト境界をまたぐ区間は 2 枚に割れる (計 6 枚)
        std::vector<XMFLOAT2> steps;
        for (int i = 0; i <= 5; ++i) {
            steps.push_back({ 0.4f * static_cast<float>(i), 0.0f });
        }
        spriteshape::BuildEdgeQuads(steps, false, edges, sprites, 1.0f, 100, quads);
        check(quads.size() == 6 && Approx(quads[1].u[0], 0.4f) && Approx(quads[1].u[1], 0.8f) && Approx(quads[2].u[0], 0.8f)
                  && Approx(quads[2].u[1], 1.0f) && Approx(quads[3].u[0], 0.0f) && Approx(quads[3].u[1], 0.2f)
                  && Approx(quads[5].u[1], 1.0f),
              "Quads: the sprite phase carries over between spans of the same edge (u continues 0.4 / 0.8 / 1.0|0.0 / 0.2 ...)");

        // 継ぎ目は法線の平均 (マイター): 同じ縁で続く 2 区間は継ぎ目の角を共有する
        const std::vector<XMFLOAT2> bend = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 2.0f, 0.5f } };
        spriteshape::BuildEdgeQuads(bend, false, edges, sprites, 1.0f, 100, quads);
        check(quads.size() == 3 && Approx(quads[0].x[1], quads[1].x[0]) && Approx(quads[0].y[1], quads[1].y[0])
                  && Approx(quads[0].x[2], quads[1].x[3]) && Approx(quads[0].y[2], quads[1].y[3]),
              "Quads: two spans of the same edge share the miter corners (no gap, no overlap; the 1.12 m second span splits into 2 quads)");
        // 縁が変わる継ぎ目は共有しない
        const std::vector<XMFLOAT2> wall = { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f } };
        // 上り (0,0)->(0,1) は法線が左 = -X (角度 90 = 縁 1)、右へ (0,1)->(1,1) は法線が上 (縁 0)
        spriteshape::BuildEdgeQuads(wall, false, edges, sprites, 1.0f, 100, quads);
        check(quads.size() == 2 && quads[0].edge == 1 && quads[1].edge == 0, "Quads: a wall going up uses the left-wall edge and the top after the corner uses the top edge");

        // 上限・厚み・スプライト無し
        const std::vector<XMFLOAT2> longLine = { { 0.0f, 0.0f }, { 50.0f, 0.0f } };
        capped = spriteshape::BuildEdgeQuads(longLine, false, edges, sprites, 1.0f, 7, quads);
        check(capped && quads.size() == 7, "Quads: stops at maxQuads and reports it");
        check(!spriteshape::BuildEdgeQuads(flat, false, edges, sprites, 0.0f, 100, quads) && quads.empty(), "Quads: height 0 draws nothing");
        const std::vector<EdgeSpriteInfo> noSprites(3);
        spriteshape::BuildEdgeQuads(flat, false, edges, noSprites, 1.0f, 100, quads);
        check(quads.empty(), "Quads: edges whose sprite is unresolved (width 0) draw nothing");
        const std::vector<XMFLOAT2> one = { { 1.0f, 1.0f } };
        spriteshape::BuildEdgeQuads(one, false, edges, sprites, 1.0f, 100, quads);
        check(quads.empty(), "Quads: 1 point draws nothing");
        const std::vector<XMFLOAT2> same = { { 1.0f, 1.0f }, { 1.0f, 1.0f } };
        spriteshape::BuildEdgeQuads(same, false, edges, sprites, 1.0f, 100, quads);
        check(quads.empty(), "Quads: all points at one position draw nothing");
        // 滑らかな丘 (開いた 8 点): 全ての区間は上向き側 (-45..45 または側面) で、出力は有限
        std::vector<XMFLOAT2> hill;
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, true, hill);
        spriteshape::BuildEdgeQuads(hill, false, edges, sprites, 1.0f, 1000, quads);
        bool finite = !quads.empty();
        for (const EdgeQuad& q : quads) {
            for (int k = 0; k < 4; ++k) {
                finite = finite && std::isfinite(q.x[k]) && std::isfinite(q.y[k]) && std::isfinite(q.u[k]);
            }
        }
        check(finite && CountEdge(quads, 2) == 0, "Quads: the hill (open, smooth) is all finite and uses no right-wall edge (it slopes down to the right)");
    }

    // ---- (d) BuildFill ----
    {
        std::vector<spriteshape::FillVertex> fill;
        const std::vector<XMFLOAT2> square = { { 0.0f, 0.0f }, { 2.0f, 0.0f }, { 2.0f, 2.0f }, { 0.0f, 2.0f } };
        bool capped = spriteshape::BuildFill(square, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1000, fill);
        check(!capped && fill.size() == 24 && Approx(FillArea(fill), 4.0f), "Fill: a 2x2 square on a 1x1 grid = 4 cells x 2 triangles, area 4");
        bool uvOk = true;
        for (const spriteshape::FillVertex& v : fill) {
            uvOk = uvOk && v.u >= -1.0e-5f && v.u <= 1.0f + 1.0e-5f && v.v >= -1.0e-5f && v.v <= 1.0f + 1.0e-5f;
        }
        check(uvOk, "Fill: every UV stays inside the sprite rect (the image repeats per cell)");
        // ずれた輪郭: 格子は座標系の原点基準。面積は保存され、UV は格子内の位置
        const std::vector<XMFLOAT2> shifted = { { 0.5f, 0.5f }, { 2.5f, 0.5f }, { 2.5f, 2.5f }, { 0.5f, 2.5f } };
        spriteshape::BuildFill(shifted, 1.0f, 1.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1000, fill);
        check(Approx(FillArea(fill), 4.0f), "Fill: a shifted square keeps its area (9 grid cells, partially covered)");
        bool corner = false;
        for (const spriteshape::FillVertex& v : fill) {
            // (0.5, 0.5) はセル (0, 0) の中央: u = 0.25 + 0.5 * 0.5 = 0.5、v = 1.0 - 0.5 * 0.5 = 0.75
            if (Approx(v.x, 0.5f) && Approx(v.y, 0.5f)) {
                corner = corner || (Approx(v.u, 0.5f) && Approx(v.v, 0.75f));
            }
        }
        check(corner, "Fill: UV = position inside the cell mapped to the sprite rect (v flipped so the image is upright)");
        // 凹五角形の面積
        const std::vector<XMFLOAT2> concave = { { 0, 0 }, { 4, 0 }, { 4, 4 }, { 2, 1 }, { 0, 4 } };
        spriteshape::BuildFill(concave, 3.0f, 3.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1000, fill);
        check(Approx(FillArea(fill), PolygonArea(concave), 1.0e-3f), "Fill: a concave polygon clipped by a coarse grid keeps its exact area (10)");
        // 打ち切り・不正入力
        capped = spriteshape::BuildFill(square, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 9, fill);
        check(capped && fill.size() <= 9, "Fill: stops at the vertex cap and reports it");
        capped = spriteshape::BuildFill(square, 1.0e-4f, 1.0e-4f, 0.0f, 0.0f, 1.0f, 1.0f, 100000, fill);
        check(capped, "Fill: a grid far too fine for the outline is cut off instead of looping");
        check(!spriteshape::BuildFill(square, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1000, fill) && fill.empty(), "Fill: tile width 0 draws nothing");
        const std::vector<XMFLOAT2> twoPoints = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
        spriteshape::BuildFill(twoPoints, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1000, fill);
        check(fill.empty(), "Fill: fewer than 3 points draws nothing");
    }

    // ---- (e) プロファイルの JSON とライブラリ ----
    {
        SpriteShapeProfile p;
        p.name = "Hill";
        p.fill = 77;
        p.height = 0.75f;
        p.edges = { { -45.0f, 45.0f, 1 }, { 45.0f, 135.0f, 2 } };
        SpriteShapeProfile back;
        check(SpriteShapeLibrary::FromJson(SpriteShapeLibrary::ToJson(p), back) && back.name == "Hill" && back.fill == 77
                  && back.height == 0.75f && back.edges.size() == 2 && back.edges[1].angleFrom == 45.0f && back.edges[1].sprite == 2,
              "Profile: ToJson -> FromJson round-trips fill / height / edges");
        SpriteShapeProfile other;
        check(!SpriteShapeLibrary::FromJson(nlohmann::json::parse("{\"shape2d\":1}"), other), "Profile: a JSON without the spriteshape key is rejected");
        check(!SpriteShapeLibrary::FromJson(nlohmann::json::parse("[1,2]"), other), "Profile: a non-object JSON is rejected");
        SpriteShapeProfile big;
        big.edges.assign(kMaxSpriteShapeEdges + 5, SpriteShapeEdge{});
        big.height = std::numeric_limits<float>::quiet_NaN();
        SpriteShapeLibrary::Sanitize(big);
        check(big.edges.size() == kMaxSpriteShapeEdges && big.height == kDefaultSpriteShapeHeight, "Profile: Sanitize caps the edges and replaces a NaN height");
        big.height = 1.0e9f;
        SpriteShapeLibrary::Sanitize(big);
        check(big.height == kMaxSpriteShapeHeight, "Profile: Sanitize clamps an huge height");

        SpriteShapeLibrary lib;
        const uint64_t hb = lib.Register(L"C:\\sel\\spriteshape\\b_cliff.spriteshape.json", p);
        SpriteShapeProfile named = p;
        named.name = "";
        const uint64_t ha = lib.Register(L"C:\\sel\\spriteshape\\a_hill.spriteshape.json", named);
        const std::vector<SpriteShapeEntry> list = lib.Enumerate();
        check(ha != hb && lib.Contains(ha) && lib.Get(ha) != nullptr && lib.Get(ha)->name == "a_hill",
              "Library: Register keys by path and names an unnamed profile after its file");
        check(list.size() == 2 && list[0].name == "Hill" && list[1].name == "a_hill", "Library: Enumerate is sorted by name (not by hash order)");
        spriteshape::Install(&lib);
        check(spriteshape::Library() == &lib && spriteshape::Resolve(AssetID{ ha }) == lib.Get(ha) && spriteshape::Resolve(AssetID{}) == nullptr
                  && spriteshape::Resolve(AssetID{ 12345 }) == nullptr,
              "spriteshape::Resolve finds registered ids and nothing for null / unknown");
        spriteshape::Install(nullptr);
        check(spriteshape::Resolve(AssetID{ ha }) == nullptr, "spriteshape::Install(nullptr) disconnects");
    }

    // ---- (f) コンポーネントの登録 ----
    {
        RegisterBuiltinComponents();
        check(SpriteShape2DComponent::sTypeId == 89, "SpriteShape2D is TypeId 89");
        const ComponentDesc& d = ComponentRegistry::Get().Desc(SpriteShape2DComponent::sTypeId);
        check((d.flags & kComponentNoHash) == 0, "SpriteShape2D is hashed (its points are a physics input)");
        const SpriteShape2DComponent defaults;
        check(defaults.pointCount == 2 && defaults.points[0].x == -2.0f && defaults.points[1].x == 2.0f && !defaults.closed && !defaults.smooth
                  && !defaults.collider && !defaults.oneWay && defaults.profile.IsNull(),
              "SpriteShape2D defaults: 2 points (-2, 0)-(2, 0), open, not smooth, no collider");
        bool allPoints = true;
        for (uint32_t i = 0; i < static_cast<uint32_t>(kSpriteShape2DMaxPoints); ++i) {
            const std::string name = "point" + std::to_string(i);
            bool found = false;
            for (const FieldDesc& f : d.fields) {
                if (name == f.name) {
                    found = f.type == FieldType::Float2 && f.offset == offsetof(SpriteShape2DComponent, points) + i * sizeof(XMFLOAT2);
                }
            }
            allPoints = allPoints && found;
        }
        check(allPoints, "SpriteShape2D: point0 .. point23 are registered at points[i]");

        // 点を 1 つ動かすと WorldHash が変わる (Line2D のような NoHash では変わらない)
        Scene a;
        Scene b;
        GameObject goA = a.CreateGameObjectTracked("S");
        goA.AddComponent<SpriteShape2DComponent>();
        GameObject goB = b.CreateGameObjectTracked("S");
        goB.AddComponent<SpriteShape2DComponent>();
        a.GetWorld().ApplyStructuralChanges();
        b.GetWorld().ApplyStructuralChanges();
        const uint64_t before = HashWorld(a.GetWorld(), SimSourcesOf(a, nullptr, nullptr, nullptr));
        check(before == HashWorld(b.GetWorld(), SimSourcesOf(b, nullptr, nullptr, nullptr)), "Hash: two identical SpriteShape2D scenes hash the same");
        a.GetWorld().GetComponent<SpriteShape2DComponent>(goA.Id())->points[1].y = 0.25f;
        check(HashWorld(a.GetWorld(), SimSourcesOf(a, nullptr, nullptr, nullptr)) != before, "Hash: moving a point changes the WorldHash");
    }

    // ---- (g) 描画リスト (ライブラリに無いスプライトはマゼンタ 1x1 単位で解決される。内蔵テクスチャのため WARP デバイスを作る) ----
    {
        GraphicsDevice device;
        check(device.Init(true), "SpriteShape2D collect test: WARP device init");
        RenderResources resources;
        resources.textures.Init(device);
        Sprite2DRenderer renderer;
        SpriteShapeLibrary lib;
        SpriteShapeProfile profile;
        profile.edges = ThreeEdges();
        profile.fill = 21;
        profile.height = 1.0f;
        const AssetID profileId{ lib.Register(L"C:\\sel\\spriteshape\\hill.spriteshape.json", profile) };
        spriteshape::Install(&lib);

        const auto makeShape = [&](World& w, const std::vector<XMFLOAT2>& pts, bool closed, bool smooth) {
            const EntityID e = w.CreateEntity("shape");
            auto* sh = w.AddComponent<SpriteShape2DComponent>(e);
            sh->profile = profileId;
            sh->pointCount = static_cast<int32_t>(pts.size());
            std::copy(pts.begin(), pts.end(), sh->points);
            sh->closed = closed;
            sh->smooth = smooth;
            return e;
        };

        {
            World w;
            makeShape(w, { { 0.0f, 0.0f }, { 3.0f, 0.0f } }, false, false);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.batches.size() == 1 && l.vertices.size() == 3 * 6 && !l.batches[0].lit && !l.HasLights(),
                  "Collect: a 3 m flat open line = 3 top-edge quads (18 vertices) in one unlit batch");
            check(l.vertices.size() == 18 && Approx(l.vertices[0].pos[0], 0.0f) && Approx(l.vertices[0].pos[1], 0.5f),
                  "Collect: the first corner is the outer (upper) left corner of the band");
        }
        {
            World w; // 閉じた四角形: 縁 12 枚 (72 頂点) + 塗り (4 x 4 を 1x1 の格子で = 16 セル x 2 三角形 = 96 頂点)
            makeShape(w, { { 0.0f, 0.0f }, { 4.0f, 0.0f }, { 4.0f, 4.0f }, { 0.0f, 4.0f } }, true, false);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            size_t total = 0;
            for (const Sprite2DBatch& b : l.batches) {
                total += b.count;
            }
            check(total == l.vertices.size() && l.vertices.size() == 96 + 72,
                  "Collect: a closed 4x4 square = fill (96 vertices) + 12 edge quads (72 vertices)");
        }
        {
            World w;
            const EntityID e = makeShape(w, { { 0.0f, 0.0f }, { 3.0f, 0.0f } }, false, false);
            w.GetComponent<SpriteShape2DComponent>(e)->pointCount = 1;
            check(renderer.Collect(w, resources).Empty(), "Collect: pointCount 1 draws nothing");
            w.GetComponent<SpriteShape2DComponent>(e)->pointCount = 2;
            w.GetComponent<SpriteShape2DComponent>(e)->profile = AssetID{};
            check(renderer.Collect(w, resources).Empty(), "Collect: no profile draws nothing");
            w.GetComponent<SpriteShape2DComponent>(e)->profile = AssetID{ 99999 };
            check(renderer.Collect(w, resources).Empty(), "Collect: an unknown profile id draws nothing");
            w.GetComponent<SpriteShape2DComponent>(e)->profile = profileId;
            w.GetComponent<SpriteShape2DComponent>(e)->pointCount = 500;
            check(!renderer.Collect(w, resources).Empty(), "Collect: an out-of-range pointCount is clamped to 24");
        }
        {
            World w; // ワールド行列 (2, 3) 平行移動
            const EntityID e = makeShape(w, { { 0.0f, 0.0f }, { 3.0f, 0.0f } }, false, false);
            w.GetComponent<WorldMatrixComponent>(e)->value._41 = 2.0f;
            w.GetComponent<WorldMatrixComponent>(e)->value._42 = 3.0f;
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(!l.vertices.empty() && Approx(l.vertices[0].pos[0], 2.0f) && Approx(l.vertices[0].pos[1], 3.5f),
                  "Collect: the world matrix moves the whole shape (translation (2, 3))");
        }
        {
            World w; // ソートキー: Sprite2D (order 0) -> SpriteShape2D (order 1) -> Sprite2D (order 2)
            const EntityID shape = makeShape(w, { { 0.0f, 0.0f }, { 1.0f, 0.0f } }, false, false);
            w.GetComponent<SpriteShape2DComponent>(shape)->orderInLayer = 1;
            // 縁のスプライトはマゼンタ (16x16 を 1 単位)、Sprite2D も同じ未解決のマゼンタで同じテクスチャ -> 頂点数の並びで順序を見る
            const EntityID first = w.CreateEntity("first");
            w.AddComponent<Sprite2DComponent>(first);
            w.GetComponent<Sprite2DComponent>(first)->orderInLayer = 0;
            w.GetComponent<WorldMatrixComponent>(first)->value._41 = -10.0f;
            const EntityID last = w.CreateEntity("last");
            w.AddComponent<Sprite2DComponent>(last);
            w.GetComponent<Sprite2DComponent>(last)->orderInLayer = 2;
            w.GetComponent<WorldMatrixComponent>(last)->value._41 = 10.0f;
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.vertices.size() == 18 && l.vertices[0].pos[0] < -9.0f && l.vertices[6].pos[0] < 1.5f && l.vertices[6].pos[0] > -1.5f
                      && l.vertices[12].pos[0] > 9.0f,
                  "Collect: SpriteShape2D uses the Sprite2D sort key (order 0 sprite, order 1 shape, order 2 sprite)");
        }
        {
            World w; // ライトのあるシーンでは lit
            makeShape(w, { { 0.0f, 0.0f }, { 3.0f, 0.0f } }, false, false);
            const EntityID light = w.CreateEntity("light");
            w.AddComponent<Light2DComponent>(light);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            check(l.HasLights() && !l.batches.empty() && l.batches[0].lit && l.lightGroups[0].anyLit,
                  "Collect: with a Light2D in the scene the SpriteShape2D batch is lit");
        }
        {
            // 8 点の滑らかな丘 + 塗り: 縁の頂点数は BuildEdgeQuads の結果と一致する
            World w;
            makeShape(w, std::vector<XMFLOAT2>(kHillPoints, kHillPoints + kHillPointCount), false, true);
            const Sprite2DDrawList& l = renderer.Collect(w, resources);
            std::vector<XMFLOAT2> poly;
            spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, true, poly);
            std::vector<EdgeQuad> quads;
            spriteshape::BuildEdgeQuads(poly, false, profile.edges, UnitSprites(), 1.0f, 682, quads);
            check(!quads.empty() && l.vertices.size() == quads.size() * 6, "Collect: the smooth open hill draws exactly the edge quads BuildEdgeQuads makes (no fill: open)");
        }
        spriteshape::Install(nullptr);
        device.Shutdown();
    }

    // ---- (h) 辺の箱の供給 ----
    {
        World w;
        const auto addShape = [&](const XMFLOAT2* pts, int count, bool closed, bool smooth, bool collider) {
            const EntityID e = w.CreateEntity("s");
            auto* sh = w.AddComponent<SpriteShape2DComponent>(e);
            sh->pointCount = count;
            std::copy(pts, pts + count, sh->points);
            sh->closed = closed;
            sh->smooth = smooth;
            sh->collider = collider;
            return e;
        };
        SpriteShapeColliders colliders;
        std::vector<StaticShape> out;

        const EntityID hill = addShape(kHillPoints, kHillPointCount, false, true, true);
        colliders.Collect(w, out);
        check(out.size() == 56 && out[0].owner == hill && out[0].col != nullptr && out[0].col->shape == collidershape::kBox && !out[0].col->oneWay,
              "Colliders: the 8-point open smooth hill = 56 boxes (7 spans x 8), owner is the entity");
        check(colliders.LastCollectBoxCount() == 56, "Colliders: LastCollectBoxCount reports the boxes of the last Collect");
        const StaticShape first = out[0];
        check(Approx(first.pose.hy, kSpriteShapeColliderHalfThickness) && Approx(first.pose.hz, kSpriteShapeColliderDepth * 0.5f)
                  && first.pose.shape == collidershape::kBox,
              "Colliders: boxes are thin (half thickness 0.1) with a z depth of 1");

        const EntityID off = addShape(kHillPoints, kHillPointCount, false, true, false);
        out.clear();
        colliders.Collect(w, out);
        check(out.size() == 56, "Colliders: a shape without collider supplies nothing");
        (void)off;

        const XMFLOAT2 tri[3] = { { 0.0f, 0.0f }, { 2.0f, 0.0f }, { 1.0f, 2.0f } };
        const EntityID closedTri = addShape(tri, 3, true, false, true);
        const EntityID openTri = addShape(tri, 3, false, false, true);
        out.clear();
        colliders.Collect(w, out);
        size_t closedBoxes = 0;
        size_t openBoxes = 0;
        for (const StaticShape& s : out) {
            closedBoxes += (s.owner == closedTri) ? 1 : 0;
            openBoxes += (s.owner == openTri) ? 1 : 0;
        }
        check(closedBoxes == 3 && openBoxes == 2, "Colliders: a closed triangle = 3 boxes, the same points open = 2 boxes");
        // 持ち主の index 昇順 (hill -> ... -> closedTri -> openTri)
        bool ordered = true;
        for (size_t i = 1; i < out.size(); ++i) {
            ordered = ordered && out[i - 1].owner.index <= out[i].owner.index;
        }
        check(ordered, "Colliders: boxes are ordered by owner entity index");

        w.GetComponent<SpriteShape2DComponent>(openTri)->oneWay = true;
        out.clear();
        colliders.Collect(w, out);
        bool oneWayOk = true;
        for (const StaticShape& s : out) {
            oneWayOk = oneWayOk && (s.col->oneWay == (s.owner == openTri));
        }
        check(oneWayOk, "Colliders: oneWay is inherited by that shape's boxes only");

        // 点を動かすと次の Collect で箱が変わる (内容ハッシュのキャッシュが無効になる)
        std::vector<StaticShape> beforeMove;
        colliders.AppendEntityShapes(w, closedTri, beforeMove);
        w.GetComponent<SpriteShape2DComponent>(closedTri)->points[2] = { 1.0f, 5.0f };
        std::vector<StaticShape> afterMove;
        colliders.AppendEntityShapes(w, closedTri, afterMove);
        check(beforeMove.size() == 3 && afterMove.size() == 3 && afterMove[1].pose.py > beforeMove[1].pose.py + 0.5f,
              "Colliders: moving a point rebuilds the boxes on the next Collect");
        w.GetComponent<SpriteShape2DComponent>(closedTri)->collider = false;
        std::vector<StaticShape> noCollider;
        colliders.AppendEntityShapes(w, closedTri, noCollider);
        check(noCollider.empty(), "Colliders: AppendEntityShapes supplies nothing once collider is off");

        // ワールド姿勢: 平行移動・スケール・Z 回転 (90 度)
        {
            World w2;
            const EntityID e = w2.CreateEntity("s");
            auto* sh = w2.AddComponent<SpriteShape2DComponent>(e);
            sh->collider = true; // 既定の 2 点 (-2, 0)-(2, 0)
            auto* lt = w2.GetComponent<LocalTransform>(e);
            lt->position = { 10.0f, 5.0f, 3.0f };
            lt->scale = { 2.0f, 3.0f, 1.0f };
            SpriteShapeColliders c2;
            std::vector<StaticShape> o2;
            c2.Collect(w2, o2);
            check(o2.size() == 1 && Approx(o2[0].pose.px, 10.0f) && Approx(o2[0].pose.py, 5.0f) && Approx(o2[0].pose.pz, 3.0f)
                      && Approx(o2[0].pose.hx, 4.0f + kSpriteShapeColliderHalfThickness * 3.0f),
                  "Colliders: position and scale apply (scale 2 doubles the length, the half thickness follows the larger of x / y)");
            lt->scale = { 1.0f, 1.0f, 1.0f };
            lt->rotation = { 0.0f, 0.0f, 0.70710678f, 0.70710678f };
            lt->position = { 0.0f, 0.0f, 0.0f };
            sh->points[0] = { 0.0f, 0.0f };
            sh->points[1] = { 2.0f, 0.0f };
            o2.clear();
            c2.Collect(w2, o2);
            check(o2.size() == 1 && Approx(o2[0].pose.px, 0.0f, 1.0e-3f) && Approx(o2[0].pose.py, 1.0f, 1.0e-3f),
                  "Colliders: a 90 degree Z rotation turns the horizontal segment into a vertical one (center (0, 1))");
            lt->scale = { -1.0f, 1.0f, 1.0f };
            lt->rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
            o2.clear();
            c2.Collect(w2, o2);
            check(o2.size() == 1 && Approx(o2[0].pose.px, -1.0f), "Colliders: a negative scale mirrors the outline (center x -1)");
        }

        // 範囲による絞り込み
        {
            World w3;
            const EntityID e = w3.CreateEntity("s");
            auto* sh = w3.AddComponent<SpriteShape2DComponent>(e);
            sh->collider = true;
            SpriteShapeColliders c3;
            std::vector<StaticShape> o3;
            MotionRegions nearRegion([](std::vector<StaticRegion>& r) { r.push_back({ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f }); });
            c3.CollectWithin(w3, nearRegion, o3);
            check(o3.size() == 1, "CollectWithin: an outline inside a moving body's reach is supplied");
            o3.clear();
            MotionRegions farRegion([](std::vector<StaticRegion>& r) { r.push_back({ 50.0f, 50.0f, -1.0f, 51.0f, 51.0f, 1.0f }); });
            c3.CollectWithin(w3, farRegion, o3);
            check(o3.empty(), "CollectWithin: an outline out of every body's reach is left out");
            o3.clear();
            MotionRegions behind([](std::vector<StaticRegion>& r) { r.push_back({ -1.0f, -1.0f, 20.0f, 1.0f, 1.0f, 21.0f }); });
            c3.CollectWithin(w3, behind, o3);
            check(o3.empty(), "CollectWithin: z is checked too (a body 20 m behind the plane does not reach it)");
        }

        // 無効なエンティティ・点が足りない
        {
            World w4;
            const EntityID e = w4.CreateEntity("s");
            auto* sh = w4.AddComponent<SpriteShape2DComponent>(e);
            sh->collider = true;
            sh->pointCount = 1;
            SpriteShapeColliders c4;
            std::vector<StaticShape> o4;
            c4.Collect(w4, o4);
            check(o4.empty(), "Colliders: fewer than 2 points = no boxes");
            w4.GetComponent<SpriteShape2DComponent>(e)->pointCount = 2;
            w4.AddComponent<ActiveComponent>(e);
            w4.GetComponent<ActiveComponent>(e)->enabled = false;
            c4.Collect(w4, o4);
            check(o4.empty(), "Colliders: a disabled entity supplies nothing");
        }
    }

    // ---- (i) 丘を転がる球 (plane2D)。決定論・2 ワールド並走・絞り込みの有無 ----
    {
        std::vector<XMFLOAT2> poly;
        spriteshape::EvaluateSpline(kHillPoints, kHillPointCount, false, true, poly);

        HillSim a(true);
        HillSim b(true);
        HillSim unculled(false);
        constexpr int kTicks = 600;
        float minClearance = 1.0e9f;
        bool mirrored = true;
        bool planeOk = true;
        for (int tick = 0; tick < kTicks; ++tick) {
            a.phys.Update(a.GetWorld(), kDt);
            b.phys.Update(b.GetWorld(), kDt);
            unculled.phys.Update(unculled.GetWorld(), kDt);
            const XMFLOAT3 p = a.BallPos();
            minClearance = (std::min)(minClearance, p.y - SurfaceY(poly, p.x));
            mirrored = mirrored && a.Hash() == b.Hash();
            const auto* rb = a.GetWorld().GetComponent<RigidbodyComponent>(a.ball);
            planeOk = planeOk && p.z == 0.0f && rb->velocity.z == 0.0f;
        }
        const XMFLOAT3 end = a.BallPos();
        MYE_LOG_INFO("  [spriteshape] ball start (-3.5, 4.5) -> end (%.3f, %.3f), min clearance %.3f", end.x, end.y, minClearance);
        check(end.x > -1.0f && end.y < 3.0f, "Hill: the ball rolls down the smooth hill (ends right of x = -1 and below y = 3)");
        check(minClearance > -0.15f, "Hill: the ball never sinks below the surface (min clearance > -0.15 m) over 600 ticks");
        check(planeOk, "Hill: plane2D keeps z and vz exactly at 0");
        check(mirrored, "Hill: two worlds run in parallel have the same WorldHash every tick");
        const uint64_t finalHash = a.Hash();
        check(finalHash == unculled.Hash(), "Hill: the culled and the unculled supply give the same final WorldHash");
        MYE_LOG_INFO("  [spriteshape] hill scenario final WorldHash %016llX", static_cast<unsigned long long>(finalHash));
        // Debug / Release で同じ値 (焼いた値)
        constexpr uint64_t kHillScenarioExpectedHash = 0xAE6DC1DD4BAEC0E6ull;
        check(finalHash == kHillScenarioExpectedHash, "Hill: the final WorldHash matches the baked value");

        // 一方通行: 下から投げた球は床を抜けて、上から落ちてきて乗る。上から置いた球は乗ったまま
        {
            Scene scene;
            PhysicsSystem phys;
            SpriteShapeColliders colliders;
            phys.AddStaticShapeSource(&colliders);
            GameObject plat = scene.CreateGameObjectTracked("Platform");
            auto* sh = plat.AddComponent<SpriteShape2DComponent>();
            sh->pointCount = 2;
            sh->points[0] = { -3.0f, 0.0f };
            sh->points[1] = { 3.0f, 0.0f };
            sh->collider = true;
            sh->oneWay = true;
            const auto addBall = [&](const char* name, float x, float y, float vy) {
                GameObject b2 = scene.CreateGameObjectTracked(name);
                b2.SetLocalPosition(x, y, 0.0f);
                auto* col = b2.AddComponent<ColliderComponent>();
                col->shape = collidershape::kSphere;
                col->isTrigger = false;
                col->radius = 0.25f;
                auto* rb = b2.AddComponent<RigidbodyComponent>();
                rb->mass = 1.0f;
                rb->plane2D = true;
                rb->velocity = { 0.0f, vy, 0.0f };
                return b2.Id();
            };
            const EntityID fromBelow = addBall("below", -1.0f, -1.0f, 8.0f);
            const EntityID fromAbove = addBall("above", 1.0f, 2.0f, 0.0f);
            scene.GetWorld().ApplyStructuralChanges();
            for (int tick = 0; tick < 240; ++tick) {
                phys.Update(scene.GetWorld(), kDt);
            }
            const float belowY = scene.GetWorld().GetComponent<LocalTransform>(fromBelow)->position.y;
            const float aboveY = scene.GetWorld().GetComponent<LocalTransform>(fromAbove)->position.y;
            MYE_LOG_INFO("  [spriteshape] one-way: from below ends y %.3f, from above ends y %.3f", belowY, aboveY);
            check(belowY > 0.2f && belowY < 0.5f, "OneWay: a ball thrown up from below passes through and comes to rest on top (y ~ 0.35)");
            check(aboveY > 0.2f && aboveY < 0.5f, "OneWay: a ball dropped from above rests on top (y ~ 0.35)");
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== SpriteShape2D self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== SpriteShape2D self test: %d FAILURE(S) ====", failCount);
    return false;
}

} // namespace mye
