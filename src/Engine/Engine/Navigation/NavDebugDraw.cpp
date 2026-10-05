//====================================================================================
//                          NavDebugDraw.cpp
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          ナビメッシュの表示用ジオメトリの実装
//====================================================================================
#include "Engine/Engine/Navigation/NavDebugDraw.h"

#include <algorithm>
#include <chrono>
#include <memory>

#include "DebugDraw.h"
#include "DetourDebugDraw.h"
#include "DetourNavMesh.h"
#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"
#include "Engine/Engine/Navigation/NavSystem.h"

namespace mye {

namespace {

// 塗り (下) と線 (上) を床と Z ファイトさせないための持ち上げ (m)。ナビメッシュは歩ける面より少し上に出る
constexpr float kFillLift = 0.02f;
constexpr float kLineLift = 0.03f;
// 塗りの不透明度 (0..1)。床の色が透けて見え、かつ NavMesh の範囲が一目で分かる強さ
constexpr float kFillAlpha = 0.38f;
// DebugUtils が外周を太い線 (2.5)、内側の辺を細い線 (1.5) で描くことを見分けに使う
constexpr float kBoundaryWidthMin = 2.0f;

// 歩行面から外れる三角形・線分を割る下限の辺の長さ (層のセル数)。段差の縁に出る斜めの帯の幅でもある
constexpr float kSubdivideCells = 3.0f;
constexpr int kSubdivideMaxDepth = 24;
// 割らずに済む、頂点の高さで張った平面と歩行面の最大の差 (m)。塗りと歩行面の許容差 0.1 m より十分小さく取る
constexpr float kFlatTolerance = 0.04f;

// 表示の作り直しを毎回ログに出す回数 (以降は 2 の冪の回だけ)
constexpr uint32_t kLogAlways = 8;

// 0xRRGGBBAA (DebugLineCmd と同じ並び)
constexpr uint32_t kBoundaryColor = 0x40E8FFFFu;
constexpr uint32_t kInnerEdgeColor = 0x40E8FF60u;
constexpr uint32_t kTileBoundColor = 0xFFC040C0u;

// エリア ID ごとの塗りの色 (r, g, b)。0 = 歩行可 (Unity 風の水色)、1 = 歩行不可、2 = Jump。
// 3 以降はユーザー定義で、色相を散らした固定パレット
constexpr uint8_t kAreaPalette[kNavAreaCount][3] = {
    { 0, 192, 255 }, { 128, 128, 128 }, { 255, 150, 30 }, { 120, 220, 70 },
    { 230, 90, 190 }, { 250, 220, 50 }, { 150, 110, 255 }, { 60, 220, 170 },
    { 235, 80, 70 },  { 90, 150, 60 },  { 200, 160, 110 }, { 70, 100, 230 },
    { 190, 255, 120 }, { 255, 120, 150 }, { 120, 200, 210 }, { 180, 140, 190 },
};

bool KeyLess(const EntityID& a, const EntityID& b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

// DebugUtils の duDebugDraw を表示用の三角形と線へ流す。点・四角形は捨てる (ナビメッシュ本体は使わない)
class GeometryCollector final : public duDebugDraw {
public:
    // store の層の高さで頂点の y を歩行面へ合わせる。ポリゴンは頂点の高さの平面なので、段差の天面や坂からずれる。
    // 辺は最大 kSubdivideCells セルまで割り、割った点ごとに合わせる
    GeometryCollector(std::vector<DebugFillVertex>& fill, std::vector<DebugLineCmd>& lines, const NavTileStore& store)
        : fill_(fill), lines_(lines), store_(store), maxEdge_(kSubdivideCells * store.CellSize())
    {
    }

    void depthMask(bool) override {}
    void texture(bool) override {}

    void begin(duDebugDrawPrimitives prim, float size) override
    {
        prim_ = prim;
        lineColor_ = size >= kBoundaryWidthMin ? kBoundaryColor : kInnerEdgeColor;
        pendingLine_ = 0;
        pendingTri_ = 0;
    }

    void vertex(const float* pos, unsigned int color) override { Push(pos[0], pos[1], pos[2], color); }
    void vertex(const float x, const float y, const float z, unsigned int color) override { Push(x, y, z, color); }
    void vertex(const float* pos, unsigned int color, const float*) override { Push(pos[0], pos[1], pos[2], color); }
    void vertex(const float x, const float y, const float z, unsigned int color, const float, const float) override
    {
        Push(x, y, z, color);
    }

    void end() override
    {
        prim_ = DU_DRAW_POINTS;
        pendingLine_ = 0;
        pendingTri_ = 0;
    }

    // 面ごとの色は areaToCol で決め、DebugUtils が付ける半透明度は捨てて kFillAlpha に揃える
    unsigned int areaToCol(unsigned int area) override
    {
        const uint8_t* c = kAreaPalette[area < static_cast<unsigned int>(kNavAreaCount) ? area : 0];
        return duRGBA(c[0], c[1], c[2], 255);
    }

private:
    void Push(float x, float y, float z, unsigned int color)
    {
        if (prim_ == DU_DRAW_TRIS) {
            tri_[pendingTri_][0] = x;
            tri_[pendingTri_][1] = y;
            tri_[pendingTri_][2] = z;
            if (++pendingTri_ == 3) {
                EmitTriangle(MakePoint(tri_[0]), MakePoint(tri_[1]), MakePoint(tri_[2]), color, 0);
                pendingTri_ = 0;
            }
            return;
        }
        if (prim_ != DU_DRAW_LINES) {
            return;
        }
        if (pendingLine_ == 0) {
            first_[0] = x;
            first_[1] = y;
            first_[2] = z;
            pendingLine_ = 1;
            return;
        }
        const float second[3] = { x, y, z };
        EmitSegment(MakePoint(first_), MakePoint(second), 0);
        pendingLine_ = 0;
    }

    static float DistSqr(const float* a, const float* b)
    {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    }

    // 割った点。p は DebugUtils が出したポリゴン上の位置 (y は平面の高さ)、surfaceY は歩行面の高さ
    struct Point {
        float p[3];
        float surfaceY;
    };

    Point MakePoint(const float* p) const
    {
        Point out = { { p[0], p[1], p[2] }, p[1] };
        store_.SampleSurfaceHeight(p[0], p[2], p[1], out.surfaceY);
        return out;
    }

    Point MidPoint(const Point& a, const Point& b) const
    {
        const float m[3] = { (a.p[0] + b.p[0]) * 0.5f, (a.p[1] + b.p[1]) * 0.5f, (a.p[2] + b.p[2]) * 0.5f };
        return MakePoint(m);
    }

    // 三角形の頂点の歩行面の高さで張った平面に対して、層の高さが kFlatTolerance を超えて外れるか。
    // 層のセルの 2 つおきに調べ、重心は必ず調べる
    bool Deviates(const Point& a, const Point& b, const Point& c) const
    {
        const float det = (b.p[2] - c.p[2]) * (a.p[0] - c.p[0]) + (c.p[0] - b.p[0]) * (a.p[2] - c.p[2]);
        if (std::fabs(det) < 1e-12f) {
            return false;
        }
        const auto outside = [&](float x, float z, float& w0, float& w1, float& w2) {
            w0 = ((b.p[2] - c.p[2]) * (x - c.p[0]) + (c.p[0] - b.p[0]) * (z - c.p[2])) / det;
            w1 = ((c.p[2] - a.p[2]) * (x - c.p[0]) + (a.p[0] - c.p[0]) * (z - c.p[2])) / det;
            w2 = 1.0f - w0 - w1;
            return w0 < 0.0f || w1 < 0.0f || w2 < 0.0f;
        };
        const auto deviation = [&](float x, float z, float w0, float w1, float w2) {
            float surface = 0.0f;
            const float hint = w0 * a.p[1] + w1 * b.p[1] + w2 * c.p[1];
            if (!store_.SampleSurfaceHeight(x, z, hint, surface)) {
                return 0.0f;
            }
            return std::fabs(surface - (w0 * a.surfaceY + w1 * b.surfaceY + w2 * c.surfaceY));
        };
        const float cx = (a.p[0] + b.p[0] + c.p[0]) / 3.0f;
        const float cz = (a.p[2] + b.p[2] + c.p[2]) / 3.0f;
        if (deviation(cx, cz, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f) > kFlatTolerance) {
            return true;
        }
        const float stride = 2.0f * store_.CellSize();
        const float minX = (std::min)(a.p[0], (std::min)(b.p[0], c.p[0]));
        const float maxX = (std::max)(a.p[0], (std::max)(b.p[0], c.p[0]));
        const float minZ = (std::min)(a.p[2], (std::min)(b.p[2], c.p[2]));
        const float maxZ = (std::max)(a.p[2], (std::max)(b.p[2], c.p[2]));
        for (float z = std::ceil(minZ / stride) * stride; z <= maxZ; z += stride) {
            for (float x = std::ceil(minX / stride) * stride; x <= maxX; x += stride) {
                float w0 = 0.0f;
                float w1 = 0.0f;
                float w2 = 0.0f;
                if (!outside(x, z, w0, w1, w2) && deviation(x, z, w0, w1, w2) > kFlatTolerance) {
                    return true;
                }
            }
        }
        return false;
    }

    // 平らな三角形はそのまま、歩行面から外れるものは最長辺を二等分して割る (辺が maxEdge_ 以下になるまで)
    void EmitTriangle(const Point& a, const Point& b, const Point& c, unsigned int color, int depth)
    {
        const float lab = DistSqr(a.p, b.p);
        const float lbc = DistSqr(b.p, c.p);
        const float lca = DistSqr(c.p, a.p);
        const float longest = (std::max)(lab, (std::max)(lbc, lca));
        if (longest > maxEdge_ * maxEdge_ && depth < kSubdivideMaxDepth && Deviates(a, b, c)) {
            if (longest == lab) {
                const Point m = MidPoint(a, b);
                EmitTriangle(a, m, c, color, depth + 1);
                EmitTriangle(m, b, c, color, depth + 1);
            } else if (longest == lbc) {
                const Point m = MidPoint(b, c);
                EmitTriangle(a, b, m, color, depth + 1);
                EmitTriangle(a, m, c, color, depth + 1);
            } else {
                const Point m = MidPoint(c, a);
                EmitTriangle(a, b, m, color, depth + 1);
                EmitTriangle(m, b, c, color, depth + 1);
            }
            return;
        }
        for (const Point* pt : { &a, &b, &c }) {
            DebugFillVertex v;
            v.x = pt->p[0];
            v.y = pt->surfaceY + kFillLift;
            v.z = pt->p[2];
            v.r = static_cast<float>(color & 0xFFu) / 255.0f;
            v.g = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
            v.b = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
            v.a = kFillAlpha;
            fill_.push_back(v);
        }
    }

    // 線分も同じ。層のセルごとに調べ、歩行面から外れるなら半分に割る
    void EmitSegment(const Point& a, const Point& b, int depth)
    {
        if (DistSqr(a.p, b.p) > maxEdge_ * maxEdge_ && depth < kSubdivideMaxDepth && SegmentDeviates(a, b)) {
            const Point m = MidPoint(a, b);
            EmitSegment(a, m, depth + 1);
            EmitSegment(m, b, depth + 1);
            return;
        }
        DebugLineCmd cmd;
        cmd.ax = a.p[0];
        cmd.ay = a.surfaceY + kLineLift;
        cmd.az = a.p[2];
        cmd.bx = b.p[0];
        cmd.by = b.surfaceY + kLineLift;
        cmd.bz = b.p[2];
        cmd.rgba = lineColor_;
        lines_.push_back(cmd);
    }

    bool SegmentDeviates(const Point& a, const Point& b) const
    {
        const float length = std::sqrt(DistSqr(a.p, b.p));
        const int steps = (std::max)(1, static_cast<int>(length / (2.0f * store_.CellSize())));
        for (int i = 0; i <= steps; ++i) {
            const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(steps + 1);
            float surface = 0.0f;
            const float hint = a.p[1] + (b.p[1] - a.p[1]) * t;
            if (store_.SampleSurfaceHeight(a.p[0] + (b.p[0] - a.p[0]) * t, a.p[2] + (b.p[2] - a.p[2]) * t, hint, surface)
                && std::fabs(surface - (a.surfaceY + (b.surfaceY - a.surfaceY) * t)) > kFlatTolerance) {
                return true;
            }
        }
        return false;
    }

    std::vector<DebugFillVertex>& fill_;
    std::vector<DebugLineCmd>& lines_;
    const NavTileStore& store_;
    float maxEdge_;
    float tri_[3][3] = {};
    int pendingTri_ = 0;
    duDebugDrawPrimitives prim_ = DU_DRAW_POINTS;
    uint32_t lineColor_ = kBoundaryColor;
    int pendingLine_ = 0;
    float first_[3] = {};
};

void AddRect(std::vector<DebugLineCmd>& out, const float* bmin, const float* bmax)
{
    const float y = bmin[1] + kLineLift;
    const float corners[4][2] = { { bmin[0], bmin[2] }, { bmax[0], bmin[2] }, { bmax[0], bmax[2] }, { bmin[0], bmax[2] } };
    for (int i = 0; i < 4; ++i) {
        DebugLineCmd cmd;
        cmd.ax = corners[i][0];
        cmd.ay = y;
        cmd.az = corners[i][1];
        cmd.bx = corners[(i + 1) % 4][0];
        cmd.by = y;
        cmd.bz = corners[(i + 1) % 4][1];
        cmd.rgba = kTileBoundColor;
        out.push_back(cmd);
    }
}

} // namespace

void NavDebugView::ScanKeys(World& world, const NavSystem* nav)
{
    scanKeys_.clear();
    // 編集中の表示にも NavMeshModifier のエリアを映す。Modifier が無いシーンでは空のまま (確保しない)
    std::vector<NavObstacleSpec> modifiers;
    std::vector<NavObstacleSpec> modifiersHere;
    NavCollectModifierSpecs(world, modifiers);
    // グループ (同じ agentTypeId の Surface、M84b) ごとに leader の .mnav を 1 回だけ出す。NavSystem の読み込みと同じ規則。
    // 表示フラグはグループのどれかの Surface で立っていれば有効 (選んでいる Surface が leader でなくても切り替えられる)
    std::vector<NavSurfaceGroup> groups;
    NavCollectSurfaceGroups(world, groups);
    for (const NavSurfaceGroup& group : groups) {
        const EntityID e = group.leader;
        const auto* surface = world.GetComponent<NavMeshSurfaceComponent>(e);
        if (surface->navAsset.value == 0) {
            continue;
        }
        Key key;
        key.entity = e;
        key.assetGuid = surface->navAsset.value;
        if (nav != nullptr) {
            // sim が読み込み済みの Surface は、その実行時のナビメッシュ (世代が変わるたびに作り直す)
            for (const NavSurfaceRuntime& rt : nav->Surfaces()) {
                if (rt.state == NavSurfaceState::Loaded && rt.entity == e && rt.assetGuid == key.assetGuid) {
                    key.generation = rt.store->Generation();
                    key.live = rt.store.get();
                    break;
                }
            }
        }
        if (key.live == nullptr && !modifiers.empty()) {
            NavFilterSpecsToSurface(world, e, modifiers, modifiersHere);
            uint64_t h = kNavFnvSeed;
            for (const NavObstacleSpec& spec : modifiersHere) {
                h = NavFnv1a(h, &spec.key, sizeof(spec.key));
                h = NavFnv1a(h, &spec.type, sizeof(spec.type));
                h = NavFnv1a(h, spec.v, sizeof(spec.v));
                h = NavFnv1a(h, &spec.yaw, sizeof(spec.yaw));
                h = NavFnv1a(h, &spec.area, sizeof(spec.area));
            }
            key.modifierHash = modifiersHere.empty() ? 0 : h;
        }
        key.flags = 0;
        for (const EntityID member : group.members) {
            const auto* m = world.GetComponent<NavMeshSurfaceComponent>(member);
            key.flags |= static_cast<uint8_t>((m->drawNavMesh ? kOutline : 0) | (m->drawNavMeshFill ? kFill : 0)
                                              | (m->drawTileBounds ? kTileBounds : 0));
        }
        scanKeys_.push_back(key);
    }
}

void NavDebugView::BuildFromMesh(Geometry& g, const NavTileStore& store)
{
    const dtNavMesh& mesh = *store.NavMesh();
    for (int i = 0; i < mesh.getMaxTiles(); ++i) {
        const dtMeshTile* tile = mesh.getTile(i);
        if (tile != nullptr && tile->header != nullptr) {
            AddRect(g.tileBounds, tile->header->bmin, tile->header->bmax);
        }
    }
    GeometryCollector collector(g.fill, g.outline, store);
    duDebugDrawNavMesh(&collector, mesh, 0);
    g.loaded = true;
}

NavDebugView::Geometry& NavDebugView::GeometryFor(World& world, const Key& key)
{
    for (Geometry& g : geometries_) {
        if (g.entity == key.entity && g.assetGuid == key.assetGuid && g.generation == key.generation
            && g.modifierHash == key.modifierHash) {
            return g;
        }
    }
    Geometry& g = geometries_.emplace_back();
    g.entity = key.entity;
    g.assetGuid = key.assetGuid;
    g.generation = key.generation;
    g.modifierHash = key.modifierHash;
    if (key.live != nullptr) {
        ++stats_.liveCount;
        BuildFromMesh(g, *static_cast<const NavTileStore*>(key.live));
        return g;
    }
    ++stats_.loadCount;

    const char* name = world.GetName(key.entity);
    NavMeshAsset::Data data;
    auto store = std::make_unique<NavTileStore>();
    if (!NavMeshAsset::LoadByGuid(key.assetGuid, data) || !NavMeshAsset::BuildStore(data, *store)) {
        MYE_LOG_WARN("[nav] surface '%s': the navigation mesh could not be loaded for display (guid %016llx)", name,
                     static_cast<unsigned long long>(key.assetGuid));
        return g;
    }
    if (key.modifierHash != 0) {
        // 編集中: .mnav に NavMeshModifier を重ねた姿を見せる (Play 中の NavSystem と同じ関数・同じ塗り方)
        std::vector<NavObstacleSpec> modifiers;
        std::vector<NavObstacleSpec> modifiersHere;
        NavCollectModifierSpecs(world, modifiers);
        NavFilterSpecsToSurface(world, key.entity, modifiers, modifiersHere);
        int failures = 0;
        NavApplyModifiers(*store, modifiersHere, failures);
        if (failures > 0) {
            MYE_LOG_WARN("[nav] surface '%s': %d modifier(s) could not be applied to the displayed navigation mesh", name,
                         failures);
        }
    }
    BuildFromMesh(g, *store);
    return g;
}

void NavDebugView::Rebuild(World& world)
{
    const auto t0 = std::chrono::steady_clock::now();

    // 構成から消えた Surface の分を捨てる
    geometries_.erase(std::remove_if(geometries_.begin(), geometries_.end(),
                                     [this](const Geometry& g) {
                                         return std::none_of(scanKeys_.begin(), scanKeys_.end(), [&g](const Key& k) {
                                             return k.entity == g.entity && k.assetGuid == g.assetGuid
                                                 && k.generation == g.generation && k.modifierHash == g.modifierHash;
                                         });
                                     }),
                      geometries_.end());

    fill_.clear();
    lines_.clear();
    for (const Key& key : scanKeys_) {
        const Geometry& g = GeometryFor(world, key); // 結合まで使い切るので、次の読み込みで再配置されても参照は残らない
        if (!g.loaded) {
            continue;
        }
        if ((key.flags & kFill) != 0) {
            fill_.insert(fill_.end(), g.fill.begin(), g.fill.end());
        }
        if ((key.flags & kOutline) != 0) {
            lines_.insert(lines_.end(), g.outline.begin(), g.outline.end());
        }
        if ((key.flags & kTileBounds) != 0) {
            lines_.insert(lines_.end(), g.tileBounds.begin(), g.tileBounds.end());
        }
    }
    keys_ = scanKeys_;
    ++fillSerial_;
    ++stats_.rebuildCount;
    stats_.lastRebuildUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    stats_.lastTriangles = static_cast<int>(fill_.size() / 3);
    stats_.lastLines = static_cast<int>(lines_.size());
    // 動く障害物で毎フレーム作り直す場合にログを埋めないよう、最初の数回と 2 の冪の回だけ出す (回数は Stats に残る)
    const uint32_t n = stats_.rebuildCount;
    if (n <= kLogAlways || (n & (n - 1)) == 0) {
        MYE_LOG_INFO("[nav] debug view rebuilt (#%u, %u from .mnav, %u from the live navmesh): %zu surface(s), %d triangles, %d lines, %.2f ms",
                     n, stats_.loadCount, stats_.liveCount, keys_.size(), stats_.lastTriangles, stats_.lastLines,
                     stats_.lastRebuildUs / 1000.0);
    }
}

void NavDebugView::Refresh(World& world, const NavSystem* nav)
{
    ++stats_.refreshCount;
    ScanKeys(world, nav);
    if (scanKeys_ == keys_) {
        return;
    }
    Rebuild(world);
}

void NavDebugView::Reset()
{
    keys_.clear();
    scanKeys_.clear();
    geometries_.clear();
    fill_.clear();
    lines_.clear();
    ++fillSerial_;
}

} // namespace mye
