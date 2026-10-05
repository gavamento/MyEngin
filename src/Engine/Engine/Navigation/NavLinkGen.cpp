//====================================================================================
//                          NavLinkGen.cpp
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ベイク時の Off-Mesh Link の自動生成の実装
//====================================================================================
#include "Engine/Engine/Navigation/NavLinkGen.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>

namespace mye {
namespace {

constexpr int kQueryNodes = 2048;
constexpr int kMaxQueryPolys = 64;
constexpr int kMaxPathPolys = 128;
// ナビメッシュ上を歩いてこの倍率 x Link の長さ以内で着くなら、近道にならないので作らない
constexpr float kShortcutRatio = 2.0f;
// まとめる距離の下限 (m)。入口と出口がどちらもこれより近い候補は 1 本にする
constexpr float kMinLinkSpacing = 1.0f;
// 辺を調べる間隔の下限 (m)
constexpr float kMinSampleStep = 0.5f;
// 三角形の XZ 格子の一辺 (m)。セル数が kMaxGridCells を超える範囲では広げる
constexpr float kGridCell = 2.0f;
constexpr int kMaxGridCells = 1 << 20;
constexpr float kRayEpsilon = 1.0e-6f;

struct QueryDeleter {
    void operator()(dtNavMeshQuery* q) const { dtFreeNavMeshQuery(q); }
};

float Dist2Xz(const float* a, const float* b)
{
    const float dx = a[0] - b[0];
    const float dz = a[2] - b[2];
    return dx * dx + dz * dz;
}

float Dist3(const float* a, const float* b)
{
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// 線分と三角形の交差 (Moller-Trumbore、両面)。端点ちょうどの接触も当たりにする
bool SegmentHitsTriangle(const float* p, const float* q, const float* a, const float* b, const float* c)
{
    const float dir[3] = { q[0] - p[0], q[1] - p[1], q[2] - p[2] };
    const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    const float h[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0] };
    const float det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
    if (std::fabs(det) < kRayEpsilon) {
        return false; // 平行
    }
    const float inv = 1.0f / det;
    const float s[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float u = inv * (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]);
    if (u < 0.0f || u > 1.0f) {
        return false;
    }
    const float qv[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
    const float v = inv * (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]);
    if (v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const float t = inv * (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]);
    return t >= 0.0f && t <= 1.0f;
}

// 三角形を XZ の格子へ振り分けたもの。Link の候補が途中で物にぶつからないかを調べる
class SoupGrid {
public:
    explicit SoupGrid(const NavTriangleSoup& soup) : soup_(soup)
    {
        const int triCount = soup.TriangleCount();
        if (triCount == 0) {
            return;
        }
        float lo[2] = { FLT_MAX, FLT_MAX };
        float hi[2] = { -FLT_MAX, -FLT_MAX };
        for (size_t i = 0; i + 2 < soup.verts.size(); i += 3) {
            lo[0] = (std::min)(lo[0], soup.verts[i]);
            lo[1] = (std::min)(lo[1], soup.verts[i + 2]);
            hi[0] = (std::max)(hi[0], soup.verts[i]);
            hi[1] = (std::max)(hi[1], soup.verts[i + 2]);
        }
        cell_ = kGridCell;
        while (CellsFor(lo, hi) > static_cast<int64_t>(kMaxGridCells)) {
            cell_ *= 2.0f;
        }
        originX_ = lo[0];
        originZ_ = lo[1];
        width_ = static_cast<int>((hi[0] - lo[0]) / cell_) + 1;
        height_ = static_cast<int>((hi[1] - lo[1]) / cell_) + 1;
        cells_.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_));
        for (int t = 0; t < triCount; ++t) {
            float tlo[2] = { FLT_MAX, FLT_MAX };
            float thi[2] = { -FLT_MAX, -FLT_MAX };
            for (int k = 0; k < 3; ++k) {
                const float* v = Vertex(t, k);
                tlo[0] = (std::min)(tlo[0], v[0]);
                tlo[1] = (std::min)(tlo[1], v[2]);
                thi[0] = (std::max)(thi[0], v[0]);
                thi[1] = (std::max)(thi[1], v[2]);
            }
            int x0, z0, x1, z1;
            CellRange(tlo, thi, x0, z0, x1, z1);
            for (int z = z0; z <= z1; ++z) {
                for (int x = x0; x <= x1; ++x) {
                    cells_[static_cast<size_t>(z) * static_cast<size_t>(width_) + static_cast<size_t>(x)].push_back(t);
                }
            }
        }
        stamp_.assign(static_cast<size_t>(triCount), 0u);
    }

    // 線分 p -> q がどれかの三角形に当たるか
    bool SegmentHits(const float* p, const float* q)
    {
        if (cells_.empty()) {
            return false;
        }
        const float lo[2] = { (std::min)(p[0], q[0]), (std::min)(p[2], q[2]) };
        const float hi[2] = { (std::max)(p[0], q[0]), (std::max)(p[2], q[2]) };
        int x0, z0, x1, z1;
        CellRange(lo, hi, x0, z0, x1, z1);
        ++stampValue_; // 複数のセルに入った三角形を 1 回だけ調べる
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                for (const int t : cells_[static_cast<size_t>(z) * static_cast<size_t>(width_) + static_cast<size_t>(x)]) {
                    if (stamp_[static_cast<size_t>(t)] == stampValue_) {
                        continue;
                    }
                    stamp_[static_cast<size_t>(t)] = stampValue_;
                    if (SegmentHitsTriangle(p, q, Vertex(t, 0), Vertex(t, 1), Vertex(t, 2))) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

private:
    const float* Vertex(int tri, int corner) const
    {
        return &soup_.verts[static_cast<size_t>(soup_.tris[static_cast<size_t>(tri) * 3 + static_cast<size_t>(corner)]) * 3];
    }

    int64_t CellsFor(const float* lo, const float* hi) const
    {
        return (static_cast<int64_t>((hi[0] - lo[0]) / cell_) + 1) * (static_cast<int64_t>((hi[1] - lo[1]) / cell_) + 1);
    }

    void CellRange(const float* lo, const float* hi, int& x0, int& z0, int& x1, int& z1) const
    {
        const auto clampX = [&](float v) { return std::clamp(static_cast<int>(std::floor((v - originX_) / cell_)), 0, width_ - 1); };
        const auto clampZ = [&](float v) { return std::clamp(static_cast<int>(std::floor((v - originZ_) / cell_)), 0, height_ - 1); };
        x0 = clampX(lo[0]);
        x1 = clampX(hi[0]);
        z0 = clampZ(lo[1]);
        z1 = clampZ(hi[1]);
    }

    const NavTriangleSoup& soup_;
    float cell_ = kGridCell;
    float originX_ = 0.0f;
    float originZ_ = 0.0f;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::vector<int>> cells_;
    std::vector<uint32_t> stamp_;
    uint32_t stampValue_ = 0;
};

// ナビメッシュ上を start から end まで歩いた距離。届かない (部分経路・探索の打ち切り) なら FLT_MAX
float WalkDistance(const dtNavMeshQuery& query, const dtQueryFilter& filter, dtPolyRef startRef, const float* startPos,
                   dtPolyRef endRef, const float* endPos)
{
    dtPolyRef path[kMaxPathPolys];
    int pathCount = 0;
    if (dtStatusFailed(query.findPath(startRef, endRef, startPos, endPos, &filter, path, &pathCount, kMaxPathPolys))
        || pathCount == 0 || path[pathCount - 1] != endRef) {
        return FLT_MAX;
    }
    float corners[kMaxPathPolys * 3];
    int cornerCount = 0;
    if (dtStatusFailed(query.findStraightPath(startPos, endPos, path, pathCount, corners, nullptr, nullptr, &cornerCount,
                                              kMaxPathPolys))
        || cornerCount == 0) {
        return FLT_MAX;
    }
    float length = 0.0f;
    for (int i = 1; i < cornerCount; ++i) {
        length += Dist3(&corners[(i - 1) * 3], &corners[i * 3]);
    }
    return length;
}

// 外周の辺 1 本の上の 1 点 (と外向きの水平な単位ベクトル)
struct EdgeSample {
    dtPolyRef ref = 0;
    float pos[3] = {};
    float normal[3] = {};
};

class LinkGenerator {
public:
    LinkGenerator(const NavBakeConfig& config, const NavTriangleSoup& soup, const dtNavMeshQuery& query)
        : config_(config), grid_(soup), query_(query)
    {
        filter_.setIncludeFlags(kNavFlagAllAreas);
        filter_.setExcludeFlags(0);
        // 歩行面のフラグは 1 << area (NavMeshProcess)。歩行不可のエリア 1 はフラグ 0 なので filter に通らない
        climb_ = config.agentMaxClimb;
        radius_ = config.agentRadius;
        spacing_ = (std::max)(kMinLinkSpacing, config.agentRadius * 4.0f);
    }

    float SampleStep() const { return (std::max)(config_.cellSize * 2.0f, kMinSampleStep); }

    void Visit(const EdgeSample& s)
    {
        if (static_cast<int>(links_.size()) >= kNavMaxGeneratedLinks) {
            return;
        }
        if (config_.linkDropHeight > climb_) {
            TryDrop(s);
        }
        if (config_.linkJumpDistance > 0.0f) {
            TryJump(s);
        }
    }

    std::vector<NavLinkSpec>& Links() { return links_; }

private:
    // 外側の下に、登れない (maxClimb を超える) が飛び降りられる高さの歩行面を探す
    void TryDrop(const EdgeSample& s)
    {
        // 上の縁は段の端から半径だけ内側、下の面は段の壁から半径だけ離れている。その少し先を中心に探す
        const float reach = radius_ * 2.0f + config_.cellSize * 2.0f;
        const float top = s.pos[1] - climb_;
        const float bottom = s.pos[1] - config_.linkDropHeight;
        const float center[3] = { s.pos[0] + s.normal[0] * reach, (top + bottom) * 0.5f, s.pos[2] + s.normal[2] * reach };
        const float half[3] = { radius_ + config_.cellSize * 2.0f, (top - bottom) * 0.5f, radius_ + config_.cellSize * 2.0f };
        dtPolyRef polys[kMaxQueryPolys];
        int polyCount = 0;
        query_.queryPolygons(center, half, &filter_, polys, &polyCount, kMaxQueryPolys);
        dtPolyRef bestRef = 0;
        float best[3] = {};
        for (int i = 0; i < polyCount; ++i) {
            float p[3];
            if (dtStatusFailed(query_.closestPointOnPoly(polys[i], center, p, nullptr))) {
                continue;
            }
            const float drop = s.pos[1] - p[1];
            const float outward = (p[0] - s.pos[0]) * s.normal[0] + (p[2] - s.pos[2]) * s.normal[2];
            if (drop <= climb_ || drop > config_.linkDropHeight || outward < radius_) {
                continue;
            }
            // いちばん浅い着地点 (同じ高さならポリゴン参照の小さい方。queryPolygons の順に依らない)
            if (bestRef == 0 || p[1] > best[1] || (p[1] == best[1] && polys[i] < bestRef)) {
                bestRef = polys[i];
                std::copy(p, p + 3, best);
            }
        }
        if (bestRef == 0) {
            return;
        }
        // 縁の上から外へ出て、着地点の上から降りる。途中の手すりや天井に当たるなら作らない
        const float lift = config_.agentHeight * 0.5f;
        const float a[3] = { s.pos[0], s.pos[1] + lift, s.pos[2] };
        const float b[3] = { best[0], s.pos[1] + lift, best[2] };
        const float c[3] = { best[0], best[1] + climb_, best[2] };
        if (grid_.SegmentHits(a, b) || grid_.SegmentHits(b, c)) {
            return;
        }
        Emit(s, bestRef, best, false);
    }

    // 外側の水平方向に、隙間を挟んで同じくらいの高さの歩行面を探す (いちばん近いもの)
    void TryJump(const EdgeSample& s)
    {
        const float step = config_.cellSize;
        const float start = radius_ + step;
        const float limit = config_.linkJumpDistance + radius_ * 2.0f + step;
        for (float d = start; d <= limit; d += step) {
            const float center[3] = { s.pos[0] + s.normal[0] * d, s.pos[1], s.pos[2] + s.normal[2] * d };
            const float half[3] = { step, climb_ + config_.cellHeight, step };
            dtPolyRef polys[kMaxQueryPolys];
            int polyCount = 0;
            query_.queryPolygons(center, half, &filter_, polys, &polyCount, kMaxQueryPolys);
            dtPolyRef bestRef = 0;
            float best[3] = {};
            float bestOutward = FLT_MAX;
            for (int i = 0; i < polyCount; ++i) {
                if (polys[i] == s.ref) {
                    continue;
                }
                float p[3];
                if (dtStatusFailed(query_.closestPointOnPoly(polys[i], center, p, nullptr))) {
                    continue;
                }
                const float outward = (p[0] - s.pos[0]) * s.normal[0] + (p[2] - s.pos[2]) * s.normal[2];
                if (std::fabs(p[1] - s.pos[1]) > climb_ || outward < start - step * 0.5f) {
                    continue;
                }
                if (bestRef == 0 || outward < bestOutward || (outward == bestOutward && polys[i] < bestRef)) {
                    bestRef = polys[i];
                    bestOutward = outward;
                    std::copy(p, p + 3, best);
                }
            }
            if (bestRef == 0) {
                continue;
            }
            // 腰の高さで間に壁が無いこと (柱の裏・壁の向こうへ飛ばない)
            const float lift = config_.agentHeight * 0.5f;
            const float a[3] = { s.pos[0], s.pos[1] + lift, s.pos[2] };
            const float b[3] = { best[0], best[1] + lift, best[2] };
            if (!grid_.SegmentHits(a, b)) {
                Emit(s, bestRef, best, true);
            }
            return; // いちばん近い面だけを見る (その先の面へは、近い面からまた Link が出る)
        }
    }

    void Emit(const EdgeSample& s, dtPolyRef endRef, const float* end, bool bidirectional)
    {
        const float linkLength = Dist3(s.pos, end);
        if (WalkDistance(query_, filter_, s.ref, s.pos, endRef, end) <= linkLength * kShortcutRatio) {
            return;
        }
        // 入口は辺の上の点を少し内側へ寄せる (タイルの境目でも入口が辺のポリゴンのタイルに入るように)
        const float inset = config_.cellSize * 0.5f;
        const float startPos[3] = { s.pos[0] - s.normal[0] * inset, s.pos[1], s.pos[2] - s.normal[2] * inset };
        const float spacing2 = spacing_ * spacing_;
        for (const NavLinkSpec& l : links_) {
            const bool sameWay = Dist2Xz(l.start, startPos) < spacing2 && Dist2Xz(l.end, end) < spacing2
                && std::fabs(l.start[1] - startPos[1]) <= climb_ && std::fabs(l.end[1] - end[1]) <= climb_;
            const bool reverse = bidirectional && l.bidirectional != 0 && Dist2Xz(l.start, end) < spacing2
                && Dist2Xz(l.end, startPos) < spacing2 && std::fabs(l.start[1] - end[1]) <= climb_
                && std::fabs(l.end[1] - startPos[1]) <= climb_;
            if ((sameWay && (l.bidirectional != 0) == bidirectional) || reverse) {
                return;
            }
        }
        const uint32_t index = static_cast<uint32_t>(links_.size());
        NavLinkSpec& link = links_.emplace_back();
        link.key = kNavGeneratedLinkKeyBit | index;
        std::copy(startPos, startPos + 3, link.start);
        std::copy(end, end + 3, link.end);
        link.radius = (std::max)(radius_, config_.cellSize);
        link.bidirectional = bidirectional ? 1 : 0;
        link.area = kNavGeneratedLinkArea;
        link.userId = kNavGeneratedLinkUserIdBit | index;
    }

    const NavBakeConfig& config_;
    SoupGrid grid_;
    const dtNavMeshQuery& query_;
    dtQueryFilter filter_;
    float climb_ = 0.0f;
    float radius_ = 0.0f;
    float spacing_ = kMinLinkSpacing;
    std::vector<NavLinkSpec> links_;
};

// ポリゴンの辺 edge が外周 (隣のポリゴンもタイルをまたぐ接続も無い) か
bool IsBoundaryEdge(const dtMeshTile& tile, const dtPoly& poly, int edge)
{
    if (poly.neis[edge] == 0) {
        return true;
    }
    if ((poly.neis[edge] & DT_EXT_LINK) == 0) {
        return false; // 同じタイルの隣のポリゴン
    }
    // タイルの境目の辺。隣のタイルに相手がいれば dtLink が張られている
    for (unsigned int li = poly.firstLink; li != DT_NULL_LINK; li = tile.links[li].next) {
        if (tile.links[li].edge == edge) {
            return false;
        }
    }
    return true;
}

} // namespace

bool NavGenerateLinks(const NavBakeConfig& config, const NavTriangleSoup& soup, const dtNavMesh& nav,
                      const std::atomic<bool>* cancel, std::vector<NavLinkSpec>& out)
{
    out.clear();
    if (config.generateLinks == 0 || (config.linkDropHeight <= config.agentMaxClimb && config.linkJumpDistance <= 0.0f)) {
        return true;
    }
    std::unique_ptr<dtNavMeshQuery, QueryDeleter> query(dtAllocNavMeshQuery());
    if (!query || dtStatusFailed(query->init(&nav, kQueryNodes))) {
        return true; // 調べられないときは Link なしで焼く (ナビメッシュ自体は使える)
    }
    LinkGenerator gen(config, soup, *query);
    const float step = gen.SampleStep();
    // タイルはスロット番号順、ポリゴン・辺は格納順に回す。スロットの割り当ては層のキー順で決まる (ADR-023)
    for (int ti = 0; ti < nav.getMaxTiles(); ++ti) {
        if (cancel != nullptr && cancel->load()) {
            out.clear();
            return false;
        }
        const dtMeshTile* tile = nav.getTile(ti);
        if (tile == nullptr || tile->header == nullptr) {
            continue;
        }
        const dtPolyRef base = nav.getPolyRefBase(tile);
        for (int pi = 0; pi < tile->header->polyCount; ++pi) {
            const dtPoly& poly = tile->polys[pi];
            if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.flags == 0 || poly.vertCount < 3) {
                continue;
            }
            float centroid[3] = {};
            for (int v = 0; v < poly.vertCount; ++v) {
                for (int k = 0; k < 3; ++k) {
                    centroid[k] += tile->verts[poly.verts[v] * 3 + k];
                }
            }
            for (int k = 0; k < 3; ++k) {
                centroid[k] /= static_cast<float>(poly.vertCount);
            }
            for (int e = 0; e < poly.vertCount; ++e) {
                if (!IsBoundaryEdge(*tile, poly, e)) {
                    continue;
                }
                const float* va = &tile->verts[poly.verts[e] * 3];
                const float* vb = &tile->verts[poly.verts[(e + 1) % poly.vertCount] * 3];
                const float dx = vb[0] - va[0];
                const float dz = vb[2] - va[2];
                const float length = std::sqrt(dx * dx + dz * dz);
                if (length < 1.0e-4f) {
                    continue;
                }
                EdgeSample s;
                s.ref = base | static_cast<dtPolyRef>(pi);
                s.normal[0] = dz / length;
                s.normal[2] = -dx / length;
                const float mid[2] = { (va[0] + vb[0]) * 0.5f, (va[2] + vb[2]) * 0.5f };
                if ((mid[0] - centroid[0]) * s.normal[0] + (mid[1] - centroid[2]) * s.normal[2] < 0.0f) {
                    s.normal[0] = -s.normal[0];
                    s.normal[2] = -s.normal[2];
                }
                const int samples = (std::max)(1, static_cast<int>(length / step));
                for (int i = 0; i < samples; ++i) {
                    const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
                    for (int k = 0; k < 3; ++k) {
                        s.pos[k] = va[k] + (vb[k] - va[k]) * t;
                    }
                    gen.Visit(s);
                }
            }
        }
    }
    out = std::move(gen.Links());
    return true;
}

} // namespace mye
