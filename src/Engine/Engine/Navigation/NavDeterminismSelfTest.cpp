//====================================================================================
//                          NavDeterminismSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          Recast 系のビット一致と状態の保存・復元の回帰テスト (M82a)
//====================================================================================
#include "Engine/Engine/Navigation/NavDeterminismSelfTest.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Core/Util/Random.h"
#include "Engine/Engine/Navigation/NavTileCacheSupport.h"

namespace mye {
namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr int kTotalTicks = 100;
constexpr int kSaveTick = 50;
constexpr int kAgentCount = 8;

// 期待値は Debug で採取し、Release で同じ値になることを確認して焼く (docs\adr\ADR-023-navmesh.md)
struct ExpectedHash {
    const char* name;
    uint64_t value;
};
constexpr ExpectedHash kExpected[] = {
    {"bake.layers", 0xF90A00D752163B7Dull},
    {"mesh.initial", 0x2909DE81767E7597ull},
    {"mesh.initialTopology", 0xF83806C122141980ull},
    {"query.initial", 0x1F9F26FBB828C3D6ull},
    {"mesh.afterObstacles", 0x63B6F64B22685296ull},
    {"query.afterObstacles", 0x6C48956A1722E7C1ull},
    {"crowd.A.tick50", 0x6B7BE00C2A223B9Aull},
    {"crowd.A.tick100", 0x4646CDB4886EBF22ull},
    {"crowd.A.allTicks", 0x7857BC4286F8F4D4ull},
    {"capture.A.store", 0x7ACDCC5314884736ull},
    {"capture.A.crowd", 0x51EC1B9B13A804EDull},
    {"crowd.B.tick50", 0x098AB522DD4B0E68ull},
    {"crowd.B.tick100", 0x41BED8471BA8DCD3ull},
    {"crowd.B.allTicks", 0x45E44EAD88AF5893ull},
    {"capture.B.store", 0xA0A4001609A5BCE7ull},
    {"capture.B.crowd", 0x689873A2F994E35Eull},
};

// ---- 固定ジオメトリ ----

struct TriMesh {
    std::vector<float> verts;
    std::vector<int> tris;
};

int AddVert(TriMesh& m, const float* p)
{
    m.verts.insert(m.verts.end(), p, p + 3);
    return static_cast<int>(m.verts.size() / 3) - 1;
}

// outward 向きに表が向くよう頂点順を整えて三角形を足す
void AddTri(TriMesh& m, const float* a, const float* b, const float* c, const float* outward)
{
    const float e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0]};
    const float dot = n[0] * outward[0] + n[1] * outward[1] + n[2] * outward[2];
    const int ia = AddVert(m, a);
    const int ib = AddVert(m, dot >= 0.0f ? b : c);
    const int ic = AddVert(m, dot >= 0.0f ? c : b);
    m.tris.push_back(ia);
    m.tris.push_back(ib);
    m.tris.push_back(ic);
}

void AddQuad(TriMesh& m, const float* p0, const float* p1, const float* p2, const float* p3, const float* outward)
{
    AddTri(m, p0, p1, p2, outward);
    AddTri(m, p0, p2, p3, outward);
}

void AddBox(TriMesh& m, const float* bmin, const float* bmax)
{
    for (int axis = 0; axis < 3; ++axis) {
        for (int side = 0; side < 2; ++side) {
            const int u = (axis + 1) % 3;
            const int v = (axis + 2) % 3;
            float corner[4][3];
            const int du[4] = {0, 1, 1, 0};
            const int dv[4] = {0, 0, 1, 1};
            for (int i = 0; i < 4; ++i) {
                corner[i][axis] = side ? bmax[axis] : bmin[axis];
                corner[i][u] = du[i] ? bmax[u] : bmin[u];
                corner[i][v] = dv[i] ? bmax[v] : bmin[v];
            }
            float outward[3] = {0.0f, 0.0f, 0.0f};
            outward[axis] = side ? 1.0f : -1.0f;
            AddQuad(m, corner[0], corner[1], corner[2], corner[3], outward);
        }
    }
}

// 床 + 段差 + 高い壁 + 坂 + 上の足場 (下を通れる) + 柱。extraPillar で 1 本足した別版を作る
TriMesh MakeGeometry(bool extraPillar)
{
    TriMesh m;
    auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        const float lo[3] = {x0, y0, z0};
        const float hi[3] = {x1, y1, z1};
        AddBox(m, lo, hi);
    };
    box(-14.4f, -0.5f, -14.4f, 14.4f, 0.0f, 14.4f);  // 床
    box(-8.0f, 0.0f, -10.0f, -4.0f, 0.2f, 10.0f);    // 登れる段差
    box(-1.5f, 0.0f, -12.0f, 1.5f, 1.2f, -9.0f);     // 登れない壁
    box(3.0f, 2.2f, 3.0f, 12.0f, 2.4f, 12.0f);       // 足場 (下は通れる)
    box(-10.5f, 0.0f, -3.3f, -9.9f, 2.5f, -2.7f);    // 柱
    box(-10.5f, 0.0f, 2.7f, -9.9f, 2.5f, 3.3f);
    box(5.7f, 0.0f, -7.3f, 6.3f, 2.5f, -6.7f);
    box(-3.3f, 0.0f, 6.7f, -2.7f, 2.5f, 7.3f);
    // 足場へ上がる坂 (傾き約 31 度)
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float r0[3] = {6.0f, 0.0f, -1.0f};
    const float r1[3] = {9.0f, 0.0f, -1.0f};
    const float r2[3] = {9.0f, 2.4f, 3.0f};
    const float r3[3] = {6.0f, 2.4f, 3.0f};
    AddQuad(m, r0, r1, r2, r3, up);
    if (extraPillar) {
        box(2.0f, 0.0f, -3.0f, 3.4f, 2.0f, -1.6f);
        box(-3.0f, 0.0f, 1.0f, -1.8f, 2.0f, 2.2f);
    }
    return m;
}

// ---- ベイク ----

struct BakeSet {
    NavBakeConfig config;
    NavTileStoreConfig storeConfig;
    int tilesX = 0;
    int tilesY = 0;
    std::vector<std::vector<uint8_t>> layers;     // 全タイルの層 (ty, tx, layer の順)
    std::vector<std::vector<uint8_t>> altTile11;  // タイル (1, 1) だけ別ジオメトリでベイクした層
};

bool BakeAll(const TriMesh& mesh, const NavBakeConfig& config, int tilesX, int tilesY,
             std::vector<std::vector<uint8_t>>& out)
{
    NavTriangleInput in;
    in.verts = mesh.verts.data();
    in.vertCount = static_cast<int>(mesh.verts.size() / 3);
    in.tris = mesh.tris.data();
    in.triCount = static_cast<int>(mesh.tris.size() / 3);
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            std::vector<std::vector<uint8_t>> layers;
            if (!NavBakeTileLayers(config, in, tx, ty, layers)) {
                return false;
            }
            for (auto& l : layers) {
                out.push_back(std::move(l));
            }
        }
    }
    return true;
}

bool MakeBakeSet(BakeSet& set)
{
    NavBakeConfig& c = set.config;
    c.boundsMin[0] = -14.4f;
    c.boundsMin[1] = -1.0f;
    c.boundsMin[2] = -14.4f;
    c.boundsMax[0] = 14.4f;
    c.boundsMax[1] = 6.0f;
    c.boundsMax[2] = 14.4f;
    NavCalcTileGrid(c, set.tilesX, set.tilesY);
    set.storeConfig = NavMakeStoreConfig(c, 64, 1024, 128);
    if (!BakeAll(MakeGeometry(false), c, set.tilesX, set.tilesY, set.layers)) {
        return false;
    }
    std::vector<std::vector<uint8_t>> altAll;
    if (!BakeAll(MakeGeometry(true), c, set.tilesX, set.tilesY, altAll)) {
        return false;
    }
    for (auto& l : altAll) {
        const auto* h = reinterpret_cast<const dtTileCacheLayerHeader*>(l.data());
        if (h->tx == 1 && h->ty == 1) {
            set.altTile11.push_back(l);
        }
    }
    return !set.layers.empty() && !set.altTile11.empty();
}

// ---- 世界 (NavTileStore + 問い合わせ + dtCrowd) ----

float g_queryRandom(); // 前方宣言 (下で定義)
Pcg32* g_queryRng = nullptr;
float g_queryRandom()
{
    return g_queryRng->NextFloat01();
}

struct NavWorld {
    NavTileStore store;
    dtNavMeshQuery* query = nullptr;
    dtCrowd* crowd = nullptr;

    ~NavWorld()
    {
        dtFreeCrowd(crowd);
        dtFreeNavMeshQuery(query);
    }
};

dtObstacleAvoidanceParams AvoidanceParams(int level)
{
    dtObstacleAvoidanceParams p;
    std::memset(&p, 0, sizeof(p));
    p.velBias = 0.4f;
    p.weightDesVel = 2.0f;
    p.weightCurVel = 0.75f;
    p.weightSide = 0.75f;
    p.weightToi = 2.5f;
    p.horizTime = 2.5f;
    p.gridSize = 33;
    p.adaptiveDivs = 7;
    p.adaptiveRings = 2;
    p.adaptiveDepth = 5;
    if (level == 0) {
        p.adaptiveDivs = 5;
        p.adaptiveRings = 2;
        p.adaptiveDepth = 1;
    } else if (level == 1) {
        p.adaptiveDivs = 5;
        p.adaptiveRings = 2;
        p.adaptiveDepth = 2;
    } else if (level == 2) {
        p.adaptiveDivs = 7;
        p.adaptiveRings = 2;
        p.adaptiveDepth = 3;
    }
    return p;
}

bool BuildWorld(NavWorld& w, const BakeSet& bake, int crowdCapacity)
{
    if (!w.store.Init(bake.storeConfig)) {
        return false;
    }
    for (const auto& l : bake.layers) {
        if (!w.store.AddBaseLayer(l.data(), static_cast<int>(l.size()))) {
            return false;
        }
    }
    if (!w.store.BuildAll()) {
        return false;
    }
    w.query = dtAllocNavMeshQuery();
    if (!w.query || dtStatusFailed(w.query->init(w.store.NavMesh(), 2048))) {
        return false;
    }
    w.crowd = dtAllocCrowd();
    if (!w.crowd || !w.crowd->init(crowdCapacity, 0.6f, w.store.NavMesh())) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        const dtObstacleAvoidanceParams p = AvoidanceParams(i);
        w.crowd->setObstacleAvoidanceParams(i, &p);
    }
    return true;
}

// 段差の上に並べたエージェントを、z を反転した東側の点へ向かわせる (全員が中央の障害物を回り込む)
bool AddAgents(NavWorld& w, int count)
{
    dtQueryFilter filter;
    filter.setIncludeFlags(kNavFlagWalk);
    filter.setExcludeFlags(0);
    const float ext[3] = {2.0f, 4.0f, 2.0f};
    for (int i = 0; i < count; ++i) {
        const int column = i / 8;
        const int row = i % 8;
        const float z = -7.0f + 2.0f * static_cast<float>(row);
        const float startPos[3] = {-5.0f + 0.8f * static_cast<float>(column), 0.0f, z};
        const float goalPos[3] = {9.5f - 0.3f * static_cast<float>(column), 0.0f, -z};
        dtPolyRef goalRef = 0;
        float goalPt[3];
        if (dtStatusFailed(w.query->findNearestPoly(goalPos, ext, &filter, &goalRef, goalPt)) || !goalRef) {
            return false;
        }
        dtCrowdAgentParams ap;
        std::memset(&ap, 0, sizeof(ap));
        ap.radius = 0.3f;
        ap.height = 1.8f;
        ap.maxAcceleration = 8.0f;
        ap.maxSpeed = 3.5f;
        ap.collisionQueryRange = ap.radius * 12.0f;
        ap.pathOptimizationRange = ap.radius * 30.0f;
        ap.separationWeight = 2.0f;
        ap.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION
            | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO;
        ap.obstacleAvoidanceType = static_cast<unsigned char>(i % 4);
        const int idx = w.crowd->addAgent(startPos, &ap);
        if (idx != i || !w.crowd->requestMoveTarget(idx, goalRef, goalPt)) {
            return false;
        }
    }
    return true;
}

// ---- 経路クエリ ----

uint64_t HashFloats(uint64_t h, const float* p, int n)
{
    return NavFnv1a(h, p, sizeof(float) * static_cast<size_t>(n));
}

template <class T>
uint64_t HashOf(uint64_t h, const T& v)
{
    return NavFnv1a(h, &v, sizeof(T));
}

struct QueryReport {
    bool crossPathFound = false;   // 西 -> 東 の経路が最後まで届く
    float centerRayT = 0.0f;       // 中央線上のレイが当たる位置 (1.0 以上なら素通し)
    int straightCorners = 0;
};

uint64_t RunQueries(NavWorld& w, QueryReport* report)
{
    dtQueryFilter filter;
    filter.setIncludeFlags(kNavFlagWalk);
    filter.setExcludeFlags(0);
    const float ext[3] = {1.5f, 3.0f, 1.5f};
    const float pairs[][6] = {
        {-12.5f, 0.0f, -6.0f, 12.0f, 0.0f, 6.0f},   // 段差 + 中央を横切る
        {-12.5f, 0.0f, 6.0f, 12.0f, 0.0f, -6.0f},
        {-13.0f, 0.0f, 0.0f, 13.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -13.0f, 0.0f, 0.0f, 13.0f},    // 壁の脇
        {-12.0f, 0.0f, -12.0f, 12.0f, 0.0f, 12.0f}, // 足場の下
        {7.5f, 2.4f, 5.0f, -12.0f, 0.0f, 0.0f},     // 足場の上 -> 床 (坂を降りる)
        {7.5f, 0.0f, -4.0f, 7.5f, 2.4f, 6.0f},      // 坂を上る
        {-9.0f, 0.2f, 0.0f, -2.0f, 0.0f, 0.0f},
        {-3.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f},      // 中央を直進 (障害物があれば塞がれる)
        {13.0f, 0.0f, 13.0f, -13.0f, 0.0f, -13.0f},
    };
    Pcg32 rng;
    rng.Seed(0x4E41565Full);
    g_queryRng = &rng;

    uint64_t h = kNavFnvSeed;
    int index = 0;
    for (const auto& pr : pairs) {
        const float* s = pr;
        const float* e = pr + 3;
        dtPolyRef sRef = 0;
        dtPolyRef eRef = 0;
        float sPt[3] = {};
        float ePt[3] = {};
        dtStatus st = w.query->findNearestPoly(s, ext, &filter, &sRef, sPt);
        h = HashOf(h, st);
        h = HashOf(h, sRef);
        h = HashFloats(h, sPt, 3);
        st = w.query->findNearestPoly(e, ext, &filter, &eRef, ePt);
        h = HashOf(h, st);
        h = HashOf(h, eRef);
        h = HashFloats(h, ePt, 3);
        if (!sRef || !eRef) {
            ++index;
            continue;
        }

        dtPolyRef path[128];
        int pathCount = 0;
        st = w.query->findPath(sRef, eRef, sPt, ePt, &filter, path, &pathCount, 128);
        h = HashOf(h, st);
        h = HashOf(h, pathCount);
        h = NavFnv1a(h, path, sizeof(dtPolyRef) * static_cast<size_t>(pathCount));
        if (pathCount > 0) {
            float corners[64 * 3];
            unsigned char cornerFlags[64];
            dtPolyRef cornerRefs[64];
            int cornerCount = 0;
            st = w.query->findStraightPath(sPt, ePt, path, pathCount, corners, cornerFlags, cornerRefs, &cornerCount, 64);
            h = HashOf(h, st);
            h = HashOf(h, cornerCount);
            h = HashFloats(h, corners, cornerCount * 3);
            h = NavFnv1a(h, cornerFlags, static_cast<size_t>(cornerCount));
            h = NavFnv1a(h, cornerRefs, sizeof(dtPolyRef) * static_cast<size_t>(cornerCount));
            if (report && index == 0) {
                const float* last = corners + (cornerCount - 1) * 3;
                const float dx = last[0] - ePt[0];
                const float dz = last[2] - ePt[2];
                report->crossPathFound = !dtStatusDetail(st, DT_PARTIAL_RESULT) && dx * dx + dz * dz < 0.25f
                    && path[pathCount - 1] == eRef;
                report->straightCorners = cornerCount;
            }
        }

        float t = 0.0f;
        float normal[3] = {};
        dtPolyRef rayPath[64];
        int rayCount = 0;
        st = w.query->raycast(sRef, sPt, ePt, &filter, &t, normal, rayPath, &rayCount, 64);
        h = HashOf(h, st);
        h = HashOf(h, t);
        h = HashFloats(h, normal, 3);
        h = HashOf(h, rayCount);
        h = NavFnv1a(h, rayPath, sizeof(dtPolyRef) * static_cast<size_t>(rayCount));
        if (report && index == 8) {
            report->centerRayT = t;
        }

        float moved[3] = {};
        dtPolyRef visited[32];
        int visitedCount = 0;
        st = w.query->moveAlongSurface(sRef, sPt, ePt, &filter, moved, visited, &visitedCount, 32);
        h = HashOf(h, st);
        h = HashFloats(h, moved, 3);
        h = HashOf(h, visitedCount);

        float closest[3] = {};
        bool over = false;
        st = w.query->closestPointOnPoly(sRef, ePt, closest, &over);
        h = HashOf(h, st);
        h = HashFloats(h, closest, 3);
        h = HashOf(h, over);

        float wallDist = 0.0f;
        float wallPt[3] = {};
        float wallNormal[3] = {};
        st = w.query->findDistanceToWall(sRef, sPt, 4.0f, &filter, &wallDist, wallPt, wallNormal);
        h = HashOf(h, st);
        h = HashOf(h, wallDist);
        h = HashFloats(h, wallPt, 3);
        ++index;
    }

    for (int i = 0; i < 4; ++i) {
        dtPolyRef ref = 0;
        float pt[3] = {};
        dtStatus st = w.query->findRandomPoint(&filter, g_queryRandom, &ref, pt);
        h = HashOf(h, st);
        h = HashOf(h, ref);
        h = HashFloats(h, pt, 3);
        if (ref) {
            dtPolyRef ref2 = 0;
            float pt2[3] = {};
            st = w.query->findRandomPointAroundCircle(ref, pt, 5.0f, &filter, g_queryRandom, &ref2, pt2);
            h = HashOf(h, st);
            h = HashOf(h, ref2);
            h = HashFloats(h, pt2, 3);
        }
    }
    g_queryRng = nullptr;
    return h;
}

// ---- 時間線 (障害物の出し入れ / タイル差し替え) ----

enum class Scenario { Obstacles, Replace };

constexpr uint64_t kObstacleBox = 1;
constexpr uint64_t kObstacleCylinder = 2;

bool ApplyEvents(NavWorld& w, const BakeSet& bake, Scenario sc, int tick)
{
    bool touched = false;
    bool ok = true;
    if (tick == 20) {
        const float lo[3] = {-1.0f, -0.5f, -3.0f};
        const float hi[3] = {1.0f, 2.0f, 3.0f};
        ok = w.store.AddBoxObstacle(kObstacleBox, lo, hi) && ok;
        touched = true;
    }
    if (tick == 30 && sc == Scenario::Replace) {
        ok = w.store.ReplaceTileLayers(1, 1, bake.altTile11) && ok;
        touched = true;
    }
    if (tick == 45) {
        const float pos[3] = {6.0f, 0.0f, 0.0f};
        ok = w.store.AddCylinderObstacle(kObstacleCylinder, pos, 1.5f, 2.0f) && ok;
        touched = true;
    }
    if (tick == 70) {
        ok = w.store.RemoveObstacle(kObstacleBox) && ok;
        touched = true;
    }
    if (touched) {
        ok = w.store.Commit() && ok;
    }
    return ok;
}

uint64_t TickHash(NavWorld& w)
{
    uint64_t h = NavHashCrowd(*w.crowd);
    const uint64_t n = w.store.HashNavMesh(true);
    return NavFnv1a(h, &n, sizeof(n));
}

struct Capture {
    std::vector<uint8_t> store;
    std::vector<uint8_t> crowd;
};

bool CaptureWorld(NavWorld& w, Capture& c)
{
    NavByteWriter ws;
    w.store.SaveState(ws, false);
    NavByteWriter wc;
    if (!NavSaveCrowd(*w.crowd, wc)) {
        return false;
    }
    c.store = ws.Data();
    c.crowd = wc.Data();
    return true;
}

bool RestoreWorld(NavWorld& w, const Capture& c)
{
    NavByteReader rs(c.store.data(), c.store.size());
    if (!w.store.LoadState(rs)) {
        return false;
    }
    NavByteReader rc(c.crowd.data(), c.crowd.size());
    return NavLoadCrowd(*w.crowd, rc);
}

struct TimelineResult {
    std::vector<uint64_t> tickHash = std::vector<uint64_t>(kTotalTicks + 1, 0);
    bool ok = true;
    int stressMismatch = 0; // 毎 tick の 保存 -> 復元 -> 再保存 でバイト列が一致しなかった回数
    Capture capture;        // saveTick の状態
    bool captured = false;
};

// from の次の tick から to まで進める。stress が true なら各 tick の終わりに同じ世界へ保存 -> 復元 -> 再保存する
void RunTimeline(NavWorld& w, const BakeSet& bake, Scenario sc, int from, int to, bool stress, TimelineResult& r)
{
    for (int tick = from + 1; tick <= to; ++tick) {
        r.ok = ApplyEvents(w, bake, sc, tick) && r.ok;
        w.crowd->update(kDt, nullptr);
        if (stress) {
            Capture a;
            Capture b;
            if (!CaptureWorld(w, a) || !RestoreWorld(w, a) || !CaptureWorld(w, b)) {
                r.ok = false;
            } else if (a.store != b.store || a.crowd != b.crowd) {
                ++r.stressMismatch;
            }
        }
        r.tickHash[static_cast<size_t>(tick)] = TickHash(w);
        if (tick == kSaveTick && !r.captured) {
            r.captured = CaptureWorld(w, r.capture);
            r.ok = r.captured && r.ok;
        }
    }
}

uint64_t FoldTicks(const std::vector<uint64_t>& v)
{
    return NavFnv1a(kNavFnvSeed, v.data(), v.size() * sizeof(uint64_t));
}

double NowMicros()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::micro>(Clock::now().time_since_epoch()).count();
}

// ---- 検証の道具 ----

struct Checker {
    int failCount = 0;

    void Check(bool cond, const char* what)
    {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    }

    // ハッシュをログへ出し、期待値があれば照合する
    void Hash(const char* name, uint64_t value)
    {
        MYE_LOG_INFO("NavDeterminism: %s = 0x%016llX", name, static_cast<unsigned long long>(value));
        for (const ExpectedHash& e : kExpected) {
            if (std::strcmp(e.name, name) == 0) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "hash %s が期待値と一致", name);
                Check(e.value == value, msg);
                return;
            }
        }
        MYE_LOG_ERROR("  FAIL: hash %s の期待値が未登録", name);
        ++failCount;
    }
};

} // namespace

bool RunNavDeterminismSelfTest()
{
    MYE_LOG_INFO("==== NavDeterminism (Recast bit-match & restore) self test ====");
    Checker ck;

    // ---- 1. ベイク (N4) ----
    BakeSet bake;
    if (!MakeBakeSet(bake)) {
        MYE_LOG_ERROR("  FAIL: ベイクに失敗");
        return false;
    }
    BakeSet bakeAgain;
    ck.Check(MakeBakeSet(bakeAgain) && bakeAgain.layers == bake.layers && bakeAgain.altTile11 == bake.altTile11,
             "同じ入力のベイクは同じバイト列");
    uint64_t layerHash = kNavFnvSeed;
    for (const auto& l : bake.layers) {
        layerHash = NavFnv1a(layerHash, l.data(), l.size());
    }
    size_t layerBytes = 0;
    for (const auto& l : bake.layers) {
        layerBytes += l.size();
    }
    MYE_LOG_INFO("  タイル %dx%d、層 %zu 枚、%zu バイト (別版タイル(1,1)は %zu 層)", bake.tilesX, bake.tilesY,
                 bake.layers.size(), layerBytes, bake.altTile11.size());
    ck.Hash("bake.layers", layerHash);

    // ---- 2. dtNavMesh と経路クエリ ----
    NavWorld base;
    if (!BuildWorld(base, bake, 8)) {
        MYE_LOG_ERROR("  FAIL: 世界の構築に失敗");
        return false;
    }
    int polyCount = 0;
    const dtNavMesh* baseNav = base.store.NavMesh();
    for (int i = 0; i < baseNav->getMaxTiles(); ++i) {
        const dtMeshTile* t = baseNav->getTile(i);
        if (t->header) {
            polyCount += t->header->polyCount;
        }
    }
    MYE_LOG_INFO("  dtNavMesh のポリゴン %d 枚、salt %u ビット (maxTiles %d / maxPolys %d)", polyCount,
                 base.store.SaltBits(), bake.storeConfig.mesh.maxTiles, bake.storeConfig.mesh.maxPolys);
    ck.Check(polyCount > 0, "dtNavMesh にポリゴンがある");
    const uint64_t meshInitial = base.store.HashNavMesh(true);
    const uint64_t topoInitial = base.store.HashNavMesh(false);
    ck.Hash("mesh.initial", meshInitial);
    ck.Hash("mesh.initialTopology", topoInitial);
    QueryReport qr0;
    ck.Hash("query.initial", RunQueries(base, &qr0));
    ck.Check(qr0.crossPathFound, "西 -> 東 (段差・柱・壁を越える) の経路が最後まで届く");
    ck.Check(qr0.centerRayT >= 1.0f, "障害物が無い間、中央線のレイは素通し");

    // ---- 3. 障害物の追加と削除 (tick 境界の同期確定) ----
    {
        const float lo[3] = {-1.0f, -0.5f, -3.0f};
        const float hi[3] = {1.0f, 2.0f, 3.0f};
        const float pos[3] = {6.0f, 0.0f, 0.0f};
        const double t0 = NowMicros();
        ck.Check(base.store.AddBoxObstacle(kObstacleBox, lo, hi) && base.store.AddCylinderObstacle(kObstacleCylinder, pos, 1.5f, 2.0f),
                 "障害物を 2 個追加");
        ck.Check(base.store.Commit(), "Commit が全要求を処理して成功");
        const double t1 = NowMicros();
        MYE_LOG_INFO("  障害物 2 個の追加 + Commit: %.0f us (ベイク済み %d 枚のタイルを全て入れ直す)", t1 - t0,
                     base.store.LayerCount());
        QueryReport qr1;
        ck.Hash("mesh.afterObstacles", base.store.HashNavMesh(true));
        ck.Hash("query.afterObstacles", RunQueries(base, &qr1));
        ck.Check(qr1.centerRayT < 1.0f, "障害物を置くと中央線のレイが塞がれる");
        ck.Check(base.store.RemoveObstacle(kObstacleBox) && base.store.RemoveObstacle(kObstacleCylinder) && base.store.Commit(),
                 "障害物を外して Commit");
        ck.Check(base.store.HashNavMesh(false) == topoInitial,
                 "障害物を足して外すと、リンク順を含む dtNavMesh の内容が最初と一致する (正規化)");
        {
            // 変更が無くても Commit は全タイルを入れ直す (正規化の費用だけを測る)
            constexpr int kRounds = 50;
            const double c0 = NowMicros();
            for (int i = 0; i < kRounds; ++i) {
                base.store.Commit();
            }
            MYE_LOG_INFO("  変更なしの Commit (正規化のみ、タイル %d 枚): %.1f us", base.store.LayerCount(),
                         (NowMicros() - c0) / kRounds);
        }
        QueryReport qr2;
        const uint64_t queryAfter = RunQueries(base, &qr2);
        ck.Check(qr2.centerRayT >= 1.0f, "障害物を外すと中央線のレイが素通しに戻る");
        ck.Check(queryAfter != 0, "外した後の問い合わせが走る");
    }

    // 要求キュー (64 件) を超える数の障害物を 1 回の Commit で入れても、1 個ずつ Commit した結果と一致する
    {
        auto addGrid = [&](NavWorld& w, bool commitEach) {
            bool ok = true;
            uint64_t key = 100;
            for (int j = 0; j < 7; ++j) {
                for (int i = 0; i < 10; ++i) {
                    const float pos[3] = {-13.0f + 2.6f * static_cast<float>(i), 0.0f, -12.0f + 4.0f * static_cast<float>(j)};
                    ok = w.store.AddCylinderObstacle(key++, pos, 0.35f, 2.0f) && ok;
                    if (commitEach) {
                        ok = w.store.Commit() && ok;
                    }
                }
            }
            return w.store.Commit() && ok;
        };
        NavWorld batch;
        NavWorld each;
        const bool built = BuildWorld(batch, bake, 8) && BuildWorld(each, bake, 8);
        ck.Check(built && addGrid(batch, false) && addGrid(each, true), "障害物 70 個 (要求キューの上限 64 を超える) を追加できる");
        ck.Check(built && batch.store.ObstacleCount() == 70 && batch.store.HashNavMesh(false) == each.store.HashNavMesh(false),
                 "70 個を 1 回の Commit で入れた結果は、1 個ずつ Commit した結果と一致する");
        ck.Check(built && batch.store.HashNavMesh(false) != topoInitial, "70 個の障害物が dtNavMesh に反映されている");
    }

    // ---- 4. 履歴依存の実証 (正規化しないと割れる) ----
    {
        auto runEvents = [&](bool canonical) {
            NavWorld w;
            BuildWorld(w, bake, 8);
            w.store.SetCanonicalize(canonical);
            ApplyEvents(w, bake, Scenario::Replace, 20);
            ApplyEvents(w, bake, Scenario::Replace, 30);
            ApplyEvents(w, bake, Scenario::Replace, 45);
            ApplyEvents(w, bake, Scenario::Replace, 70);
            return w.store.HashNavMesh(false);
        };
        const uint64_t canonical = runEvents(true);
        const uint64_t naive = runEvents(false);
        MYE_LOG_INFO("  リンク順の比較: 正規化あり 0x%016llX / 正規化なし 0x%016llX (%s)",
                     static_cast<unsigned long long>(canonical), static_cast<unsigned long long>(naive),
                     canonical == naive ? "一致" : "割れる = 履歴依存を確認");
        ck.Check(canonical != naive, "正規化しない dtNavMesh は履歴でリンク順が変わる (復元に表の salt だけでは足りない)");
    }

    // ---- 5. dtCrowd 100 tick と、保存 -> 復元 ----
    const struct {
        Scenario scenario;
        const char* name;
    } scenarios[] = {{Scenario::Obstacles, "A"}, {Scenario::Replace, "B"}};
    for (const auto& sc : scenarios) {
        MYE_LOG_INFO("-- シナリオ %s (%s) --", sc.name, sc.scenario == Scenario::Obstacles ? "障害物の出し入れ" : "タイル差し替え込み");
        NavWorld live;
        TimelineResult cont;
        if (!BuildWorld(live, bake, 8) || !AddAgents(live, kAgentCount)) {
            ck.Check(false, "世界の構築");
            continue;
        }
        float startXZ[kAgentCount][2];
        for (int i = 0; i < kAgentCount; ++i) {
            startXZ[i][0] = live.crowd->getAgent(i)->npos[0];
            startXZ[i][1] = live.crowd->getAgent(i)->npos[2];
        }
        RunTimeline(live, bake, sc.scenario, 0, kTotalTicks, false, cont);
        ck.Check(cont.ok && cont.captured, "連続実行 100 tick が成功し tick 50 を保存できた");
        float movedSum = 0.0f;
        int walking = 0;
        for (int i = 0; i < kAgentCount; ++i) {
            const dtCrowdAgent* ag = live.crowd->getAgent(i);
            movedSum += std::sqrt((ag->npos[0] - startXZ[i][0]) * (ag->npos[0] - startXZ[i][0])
                                  + (ag->npos[2] - startXZ[i][1]) * (ag->npos[2] - startXZ[i][1]));
            walking += ag->state == DT_CROWDAGENT_STATE_WALKING && ag->targetState == DT_CROWDAGENT_TARGET_VALID ? 1 : 0;
        }
        MYE_LOG_INFO("  100 tick 後: 平均移動 %.2f m、経路を持って歩行中 %d / %d 体", movedSum / kAgentCount, walking, kAgentCount);
        ck.Check(movedSum / kAgentCount > 2.0f && walking == kAgentCount, "エージェントが目的地へ向かって動いている");
        char name[64];
        std::snprintf(name, sizeof(name), "crowd.%s.tick50", sc.name);
        ck.Hash(name, cont.tickHash[kSaveTick]);
        std::snprintf(name, sizeof(name), "crowd.%s.tick100", sc.name);
        ck.Hash(name, cont.tickHash[kTotalTicks]);
        std::snprintf(name, sizeof(name), "crowd.%s.allTicks", sc.name);
        ck.Hash(name, FoldTicks(cont.tickHash));
        std::snprintf(name, sizeof(name), "capture.%s.store", sc.name);
        ck.Hash(name, NavFnv1a(kNavFnvSeed, cont.capture.store.data(), cont.capture.store.size()));
        std::snprintf(name, sizeof(name), "capture.%s.crowd", sc.name);
        ck.Hash(name, NavFnv1a(kNavFnvSeed, cont.capture.crowd.data(), cont.capture.crowd.size()));
        MYE_LOG_INFO("  tick 50 の保存: 層・障害物・表 %zu バイト、dtCrowd %zu バイト", cont.capture.store.size(),
                     cont.capture.crowd.size());

        // 新しい世界へ復元して 50 tick 進める
        NavWorld restored;
        TimelineResult res;
        const bool built = BuildWorld(restored, bake, 8);
        ck.Check(built && RestoreWorld(restored, cont.capture), "新しい世界へ tick 50 の状態を復元");
        if (built) {
            res.tickHash[kSaveTick] = TickHash(restored);
            ck.Check(res.tickHash[kSaveTick] == cont.tickHash[kSaveTick], "復元直後のハッシュが保存時と一致");
            RunTimeline(restored, bake, sc.scenario, kSaveTick, kTotalTicks, false, res);
            bool same = res.ok;
            int firstDiff = -1;
            for (int t = kSaveTick; t <= kTotalTicks; ++t) {
                if (res.tickHash[static_cast<size_t>(t)] != cont.tickHash[static_cast<size_t>(t)]) {
                    same = false;
                    if (firstDiff < 0) {
                        firstDiff = t;
                    }
                }
            }
            if (!same && firstDiff >= 0) {
                MYE_LOG_ERROR("  最初に割れた tick: %d", firstDiff);
            }
            ck.Check(same, "復元して 50 tick 進めた結果が連続実行の tick 100 まで毎 tick 一致");
        }

        // 毎 tick の 保存 -> 同じ世界へ復元 -> 再保存
        NavWorld stress;
        TimelineResult st;
        if (BuildWorld(stress, bake, 8) && AddAgents(stress, kAgentCount)) {
            RunTimeline(stress, bake, sc.scenario, 0, kTotalTicks, true, st);
            ck.Check(st.ok && st.stressMismatch == 0, "毎 tick の 保存 -> 復元 -> 再保存 でバイト列が一致");
            ck.Check(st.tickHash == cont.tickHash, "毎 tick 復元しても結果は連続実行と一致");
        } else {
            ck.Check(false, "stress 世界の構築");
        }
    }

    // ---- 6. 保存に掛かる時間 ----
    for (const int agents : {8, 128}) {
        NavWorld w;
        TimelineResult r;
        if (!BuildWorld(w, bake, agents > 8 ? 128 : 8) || !AddAgents(w, agents)) {
            ck.Check(false, "計測用の世界の構築");
            continue;
        }
        RunTimeline(w, bake, Scenario::Replace, 0, kSaveTick, false, r);
        constexpr int kRounds = 200;
        double storeUs = 0.0;
        double crowdUs = 0.0;
        double loadStoreUs = 0.0;
        double loadCrowdUs = 0.0;
        size_t storeBytes = 0;
        size_t crowdBytes = 0;
        Capture cap;
        for (int i = 0; i < kRounds; ++i) {
            const double t0 = NowMicros();
            NavByteWriter ws;
            w.store.SaveState(ws, false);
            const double t1 = NowMicros();
            NavByteWriter wc;
            NavSaveCrowd(*w.crowd, wc);
            const double t2 = NowMicros();
            storeUs += t1 - t0;
            crowdUs += t2 - t1;
            storeBytes = ws.Size();
            crowdBytes = wc.Size();
            if (i == 0) {
                cap.store = ws.Data();
                cap.crowd = wc.Data();
            }
        }
        for (int i = 0; i < 20; ++i) {
            const double t0 = NowMicros();
            NavByteReader rs(cap.store.data(), cap.store.size());
            w.store.LoadState(rs);
            const double t1 = NowMicros();
            NavByteReader rc(cap.crowd.data(), cap.crowd.size());
            NavLoadCrowd(*w.crowd, rc);
            const double t2 = NowMicros();
            loadStoreUs += t1 - t0;
            loadCrowdUs += t2 - t1;
        }
        MYE_LOG_INFO("  保存 (エージェント %3d 体): 状態表 %.1f us / %zu B、dtCrowd %.1f us / %zu B (毎 tick capture に足す分)", agents,
                     storeUs / kRounds, storeBytes, crowdUs / kRounds, crowdBytes);
        MYE_LOG_INFO("  復元 (エージェント %3d 体): 状態表 %.1f us、dtCrowd %.1f us", agents, loadStoreUs / 20.0, loadCrowdUs / 20.0);
    }

    MYE_LOG_INFO("==== NavDeterminism: %s (%d failures) ====", ck.failCount == 0 ? "PASS" : "FAIL", ck.failCount);
    return ck.failCount == 0;
}

} // namespace mye
