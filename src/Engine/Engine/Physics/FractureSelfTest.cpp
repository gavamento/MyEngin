//====================================================================================
//                          FractureSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊分割コア (M80a) のヘッドレス回帰テスト実装
//====================================================================================
#include "Engine/Engine/Physics/FractureSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Components.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Asset/FractureAsset.h"
#include "Engine/Engine/FractureBuilder.h"
#include "Engine/Engine/FractureSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Physics/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/FractureBake.h"
#include "Engine/Engine/Physics/FractureLibrary.h"
#include "Engine/Engine/Physics/FractureMesh.h"
#include "Engine/Engine/Physics/FractureVoxel.h"
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Replay/SimSnapshot.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Renderer/GpuResources.h"
#include "Shared/ScriptAPI.h" // M80l: onBreak (GetBreakFn<T>) のマクロ展開検査

using namespace DirectX;

namespace mye {
namespace {

int32_t AddVert(FractureMesh& m, float x, float y, float z, float nx, float ny, float nz, float u,
                float v)
{
    FractureVertex vert;
    vert.position = { x, y, z };
    vert.normal = { nx, ny, nz };
    vert.uv = { u, v };
    m.verts.push_back(vert);
    return static_cast<int32_t>(m.verts.size()) - 1;
}

void Tri(FractureMesh& m, int32_t a, int32_t b, int32_t c)
{
    m.indices.push_back(a);
    m.indices.push_back(b);
    m.indices.push_back(c);
}

// 共有 8 頂点の箱 (半径 hx,hy,hz)。CCW 外向き
FractureMesh MakeBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, -1, -1, -1, 0, 0), AddVert(m, hx, -hy, -hz, 1, -1, -1, 0, 0),
        AddVert(m, hx, hy, -hz, 1, 1, -1, 0, 0),      AddVert(m, -hx, hy, -hz, -1, 1, -1, 0, 0),
        AddVert(m, -hx, -hy, hz, -1, -1, 1, 0, 0),    AddVert(m, hx, -hy, hz, 1, -1, 1, 0, 0),
        AddVert(m, hx, hy, hz, 1, 1, 1, 0, 0),         AddVert(m, -hx, hy, hz, -1, 1, 1, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    quad(p[4], p[5], p[6], p[7]); // +Z
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 面ごとに頂点を複製した箱 (UV継ぎ目で頂点が割れたモデルの模擬)。位置は共有箱と同一
FractureMesh MakeSeamedBox(float hx, float hy, float hz)
{
    FractureMesh m;
    auto face = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d, XMFLOAT3 n) {
        const int32_t ia = AddVert(m, a.x, a.y, a.z, n.x, n.y, n.z, 0, 0);
        const int32_t ib = AddVert(m, b.x, b.y, b.z, n.x, n.y, n.z, 1, 0);
        const int32_t ic = AddVert(m, c.x, c.y, c.z, n.x, n.y, n.z, 1, 1);
        const int32_t id = AddVert(m, d.x, d.y, d.z, n.x, n.y, n.z, 0, 1);
        Tri(m, ia, ib, ic);
        Tri(m, ia, ic, id);
    };
    const XMFLOAT3 c000{ -hx, -hy, -hz }, c100{ hx, -hy, -hz }, c110{ hx, hy, -hz },
        c010{ -hx, hy, -hz }, c001{ -hx, -hy, hz }, c101{ hx, -hy, hz }, c111{ hx, hy, hz },
        c011{ -hx, hy, hz };
    face(c000, c010, c110, c100, { 0, 0, -1 });
    face(c001, c101, c111, c011, { 0, 0, 1 });
    face(c000, c100, c101, c001, { 0, -1, 0 });
    face(c010, c011, c111, c110, { 0, 1, 0 });
    face(c000, c001, c011, c010, { -1, 0, 0 });
    face(c100, c110, c111, c101, { 1, 0, 0 });
    return m;
}

// 蓋のない箱 (+Z 面を欠く。境界辺 4 本)
FractureMesh MakeOpenBox(float hx, float hy, float hz)
{
    FractureMesh m;
    const int32_t p[8] = {
        AddVert(m, -hx, -hy, -hz, 0, 0, 0, 0, 0), AddVert(m, hx, -hy, -hz, 0, 0, 0, 0, 0),
        AddVert(m, hx, hy, -hz, 0, 0, 0, 0, 0),   AddVert(m, -hx, hy, -hz, 0, 0, 0, 0, 0),
        AddVert(m, -hx, -hy, hz, 0, 0, 0, 0, 0),  AddVert(m, hx, -hy, hz, 0, 0, 0, 0, 0),
        AddVert(m, hx, hy, hz, 0, 0, 0, 0, 0),     AddVert(m, -hx, hy, hz, 0, 0, 0, 0, 0),
    };
    auto quad = [&](int32_t a, int32_t b, int32_t c, int32_t d) {
        Tri(m, a, b, c);
        Tri(m, a, c, d);
    };
    quad(p[0], p[3], p[2], p[1]); // -Z
    // +Z を作らない (穴)
    quad(p[0], p[1], p[5], p[4]); // -Y
    quad(p[3], p[7], p[6], p[2]); // +Y
    quad(p[0], p[4], p[7], p[3]); // -X
    quad(p[1], p[2], p[6], p[5]); // +X
    return m;
}

// 1 枚の平面 (quad)。境界辺 4 本
FractureMesh MakePlaneQuad(float half)
{
    FractureMesh m;
    const int32_t a = AddVert(m, -half, 0, -half, 0, 1, 0, 0, 0);
    const int32_t b = AddVert(m, half, 0, -half, 0, 1, 0, 1, 0);
    const int32_t c = AddVert(m, half, 0, half, 0, 1, 0, 1, 1);
    const int32_t d = AddVert(m, -half, 0, half, 0, 1, 0, 0, 1);
    Tri(m, a, b, c);
    Tri(m, a, c, d);
    return m;
}

// 平行な2枚の開いた壁 (二重壁の模擬)。互いに非連結な三角形群を1つのメッシュに持つ入力
FractureMesh MakeDoubleWallMesh(float half, float gap)
{
    FractureMesh m;
    auto addPlane = [&](float y, float ny) {
        const int32_t a = AddVert(m, -half, y, -half, 0, ny, 0, 0, 0);
        const int32_t b = AddVert(m, half, y, -half, 0, ny, 0, 1, 0);
        const int32_t c = AddVert(m, half, y, half, 0, ny, 0, 1, 1);
        const int32_t d = AddVert(m, -half, y, half, 0, ny, 0, 0, 1);
        if (ny > 0.0f) {
            Tri(m, a, b, c);
            Tri(m, a, c, d);
        } else {
            Tri(m, a, c, b);
            Tri(m, a, d, c);
        }
    };
    addPlane(-gap * 0.5f, -1.0f);
    addPlane(gap * 0.5f, 1.0f);
    return m;
}

// 辺を 3 枚の三角形で共有する非多様体メッシュ (箱 + 余分なフラップ)
FractureMesh MakeNonManifold(float hx, float hy, float hz)
{
    FractureMesh m = MakeBox(hx, hy, hz);
    // 既存の辺 (0,1) (-Z 面と -Y 面で共有済み) に、外へ張り出す第3の三角形を足す
    const int32_t extra = AddVert(m, 0, -hy * 2.0f, -hz, 0, -1, 0, 0, 0);
    Tri(m, 0, 1, extra);
    return m;
}

// -Z 面だけ巻き順を反転した箱 (向き不一致)
FractureMesh MakeFlippedFaceBox(float hx, float hy, float hz)
{
    FractureMesh m = MakeBox(hx, hy, hz);
    // MakeBox の先頭 2 三角形が -Z 面 (quad(p0,p3,p2,p1) → tri(0,3,2), tri(0,2,1))
    std::swap(m.indices[1], m.indices[2]);
    std::swap(m.indices[4], m.indices[5]);
    return m;
}

// 凹んだ L 字の三角柱 (XY 断面が L 字、Z 方向に depth 押し出し)。
// 断面の頂点は 6 個 (A,B,C,D,E,F)。7 個目の頂点 H=(0,1) を A-F 辺の分割点として足す案は
// 捨てた — H は A と F を結ぶ直線 (x=0) の上に乗るため、扇の最後の三角形 (A,F,H) が
// 面積 0 に潰れ、さらに平面で切ったときに同じ交点が 2 つの異なる辺として現れて破綻した
// (実測で確認済み)。6 頂点なら A から見通せない頂点がなく (star-shaped)、共線の三角形もない
FractureMesh MakeLShapePrism(float depth)
{
    const XMFLOAT2 profile[6] = {
        { 0, 0 }, { 2, 0 }, { 2, 1 }, { 1, 1 }, { 1, 2 }, { 0, 2 },
    };
    constexpr int32_t n = 6;
    FractureMesh m;
    int32_t bot[n], top[n];
    for (int32_t i = 0; i < n; ++i) {
        bot[i] = AddVert(m, profile[i].x, profile[i].y, 0.0f, 0, 0, -1, profile[i].x, profile[i].y);
    }
    for (int32_t i = 0; i < n; ++i) {
        top[i] = AddVert(m, profile[i].x, profile[i].y, depth, 0, 0, 1, profile[i].x, profile[i].y);
    }
    for (int32_t i = 0; i < n; ++i) {
        const int32_t j = (i + 1) % n;
        Tri(m, bot[i], bot[j], top[j]);
        Tri(m, bot[i], top[j], top[i]);
    }
    // 上面 (+Z) / 底面 (-Z): A (index0) からの扇
    for (int32_t i = 1; i + 1 < n; ++i) {
        Tri(m, top[0], top[i], top[i + 1]);
        Tri(m, bot[0], bot[i + 1], bot[i]);
    }
    return m;
}

// トーラス (軸 = Y)。majorSeg × minorSeg の格子で分割し、各セルを 2 三角形にする
FractureMesh MakeTorus(float majorR, float minorR, int32_t majorSeg, int32_t minorSeg)
{
    FractureMesh m;
    std::vector<int32_t> ids(static_cast<size_t>(majorSeg) * static_cast<size_t>(minorSeg));
    for (int32_t i = 0; i < majorSeg; ++i) {
        const float theta = 2.0f * XM_PI * static_cast<float>(i) / static_cast<float>(majorSeg);
        const float ct = std::cos(theta), st = std::sin(theta);
        for (int32_t j = 0; j < minorSeg; ++j) {
            const float phi = 2.0f * XM_PI * static_cast<float>(j) / static_cast<float>(minorSeg);
            const float cp = std::cos(phi), sp = std::sin(phi);
            const float ringR = majorR + minorR * cp;
            const float x = ringR * ct;
            const float y = minorR * sp;
            const float z = ringR * st;
            const float nx = cp * ct, ny = sp, nz = cp * st;
            ids[static_cast<size_t>(i) * static_cast<size_t>(minorSeg) + static_cast<size_t>(j)]
                = AddVert(m, x, y, z, nx, ny, nz, theta, phi);
        }
    }
    auto at = [&](int32_t i, int32_t j) {
        i = (i % majorSeg + majorSeg) % majorSeg;
        j = (j % minorSeg + minorSeg) % minorSeg;
        return ids[static_cast<size_t>(i) * static_cast<size_t>(minorSeg) + static_cast<size_t>(j)];
    };
    for (int32_t i = 0; i < majorSeg; ++i) {
        for (int32_t j = 0; j < minorSeg; ++j) {
            const int32_t a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
            // a→b は ∂/∂θ、a→d は ∂/∂φ 方向。∂θ×∂φ は管の中心へ向く内向きなので、
            // 外向きにするには a→d→b→c 側 (∂φ×∂θ) の巻きにする
            Tri(m, a, c, b);
            Tri(m, a, d, c);
        }
    }
    return m;
}

std::vector<uint8_t> SerializeMesh(const FractureMesh& mesh)
{
    std::vector<uint8_t> out;
    auto append = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    const uint32_t vc = static_cast<uint32_t>(mesh.verts.size());
    const uint32_t ic = static_cast<uint32_t>(mesh.indices.size());
    append(&vc, sizeof(vc));
    append(&ic, sizeof(ic));
    if (vc > 0) {
        append(mesh.verts.data(), mesh.verts.size() * sizeof(FractureVertex));
    }
    if (ic > 0) {
        append(mesh.indices.data(), mesh.indices.size() * sizeof(int32_t));
    }
    return out;
}

std::vector<uint8_t> SerializeCutResult(const PlaneCutResult& r)
{
    std::vector<uint8_t> out;
    auto app = [&](const std::vector<uint8_t>& v) { out.insert(out.end(), v.begin(), v.end()); };
    const uint8_t ok = r.success ? 1 : 0;
    out.push_back(ok);
    app(SerializeMesh(r.positive.outer));
    app(SerializeMesh(r.positive.cap));
    app(SerializeMesh(r.negative.outer));
    app(SerializeMesh(r.negative.cap));
    return out;
}

// 蓋込みで閉じているか (外側面+蓋を連結して判定)
bool SideIsClosed(const PlaneCutSide& side)
{
    FractureMesh combined;
    combined.verts = side.outer.verts;
    combined.indices = side.outer.indices;
    const int32_t base = static_cast<int32_t>(combined.verts.size());
    combined.verts.insert(combined.verts.end(), side.cap.verts.begin(), side.cap.verts.end());
    for (int32_t idx : side.cap.indices) {
        combined.indices.push_back(idx + base);
    }
    const ClosedMeshCheck c = CheckClosedMesh(combined);
    if (!c.closed) {
        MYE_LOG_ERROR("    (SideIsClosed detail: boundary=%d nonManifold=%d mismatch=%d)",
                     c.boundaryEdges, c.nonManifoldEdges, c.orientationMismatches);
    }
    return c.closed;
}

double SideVolume(const PlaneCutSide& side)
{
    return SignedVolume(side.outer) + SignedVolume(side.cap);
}

bool Near(double a, double b, double relTol, double absTol = 1e-9)
{
    return std::fabs(a - b) <= absTol + relTol * std::fabs(b);
}

// 破片 (outer+cap) の面が欠けていないか (位相的な閉じは求めず、ベクトル面積の和が
// 表面積に対して十分小さいことで判定する。BakeFracture 内部の ValidatePieceGeometry
// と同じ式を、SelfTest 側でも独立に検算する)
bool PieceGeometryValid(const FracturePieceBake& piece)
{
    double vx = 0.0, vy = 0.0, vz = 0.0, surfaceArea = 0.0;
    auto accumulate = [&](const FractureMesh& mesh) {
        const int32_t triCount = mesh.TriCount();
        for (int32_t t = 0; t < triCount; ++t) {
            const XMFLOAT3& a = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])].position;
            const XMFLOAT3& b = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])].position;
            const XMFLOAT3& c = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])].position;
            const double e1x = static_cast<double>(b.x) - a.x, e1y = static_cast<double>(b.y) - a.y,
                        e1z = static_cast<double>(b.z) - a.z;
            const double e2x = static_cast<double>(c.x) - a.x, e2y = static_cast<double>(c.y) - a.y,
                        e2z = static_cast<double>(c.z) - a.z;
            const double cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
            vx += cx * 0.5;
            vy += cy * 0.5;
            vz += cz * 0.5;
            surfaceArea += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
        }
    };
    accumulate(piece.outer);
    accumulate(piece.cap);
    const double vectorAreaMag = std::sqrt(vx * vx + vy * vy + vz * vz);
    return (piece.volume > 0.0) && (vectorAreaMag <= 1e-4 * surfaceArea);
}

double TotalBakedVolume(const FractureBakeResult& r)
{
    double v = 0.0;
    for (const FracturePieceBake& p : r.pieces) {
        v += p.volume;
    }
    return v;
}

void MeshAabbOf(const FractureMesh& m, XMFLOAT3& lo, XMFLOAT3& hi)
{
    lo = hi = m.verts[0].position;
    for (const FractureVertex& v : m.verts) {
        lo.x = (std::min)(lo.x, v.position.x);
        lo.y = (std::min)(lo.y, v.position.y);
        lo.z = (std::min)(lo.z, v.position.z);
        hi.x = (std::max)(hi.x, v.position.x);
        hi.y = (std::max)(hi.y, v.position.y);
        hi.z = (std::max)(hi.z, v.position.z);
    }
}

// ---- M80g (接着の破断・塊の分離) のテスト用ヘルパー ----

// 一辺 2*h の箱の凸包 (15c の compound_settle と同じ形)
ConvexHullData BoxHull(float hx, float hy, float hz)
{
    ConvexHullData h;
    BuildConvexHull({ { -hx, -hy, -hz }, { hx, -hy, -hz }, { hx, hy, -hz }, { -hx, hy, -hz },
                      { -hx, -hy, hz }, { hx, -hy, hz }, { hx, hy, hz }, { -hx, hy, hz } },
                    h);
    return h;
}

// X 軸方向に隙間なく並んだ count 個の箱の合成 FractureBakeResult (BakeFracture を経由せず
// FractureLibrary::RegisterBaked へ直接渡す — 隣接面積を一様値にして接着の強度計算を
// 検算しやすくするための手組み資産。外側/断面メッシュは空のまま (物理・接着の検証に
// 見た目は要らない。FractureLibrary は 0 頂点のメッシュを登録しない)
FractureBakeResult MakeRowFractureBake(int32_t count, float halfExtent)
{
    FractureBakeResult bake;
    bake.success = true;
    const float step = halfExtent * 2.0f;
    const float offset = (static_cast<float>(count) - 1.0f) * 0.5f;
    for (int32_t i = 0; i < count; ++i) {
        FracturePieceBake p;
        p.origin = { (static_cast<float>(i) - offset) * step, 0.0f, 0.0f };
        p.volume = static_cast<double>(step) * static_cast<double>(step) * static_cast<double>(step);
        p.hull = BoxHull(halfExtent, halfExtent, halfExtent);
        if (i > 0) {
            p.neighbors.push_back({ i - 1, 1.0f });
        }
        if (i + 1 < count) {
            p.neighbors.push_back({ i + 1, 1.0f });
        }
        bake.pieces.push_back(std::move(p));
    }
    return bake;
}

// root の直子から FracturePiece.index==index のものを探す
EntityID FindPieceChild(World& world, EntityID root, int32_t index)
{
    const auto* rh = world.GetComponent<HierarchyComponent>(root);
    for (EntityID c = rh ? rh->firstChild : kNullEntity; !c.IsNull();) {
        if (const auto* fp = world.GetComponent<FracturePieceComponent>(c)) {
            if (fp->index == index) {
                return c;
            }
        }
        const auto* ch = world.GetComponent<HierarchyComponent>(c);
        c = ch ? ch->nextSibling : kNullEntity;
    }
    return kNullEntity;
}

// afterBreak の各挙動テストの共通下ごしらえ: pieceCount 個の row 資産を焼き、root の端
// (index 0) だけを切り離してリーダーへ昇格させる (root の残り (count-1 個) の方が体積が
// 大きいので index 0 側が新規リーダーになる、wall16 (16c) と同じ理屈)。呼び出し側は
// 戻り値の DestructibleComponent* で afterBreak 系のフィールドを設定してから tick を進める
struct DetachedLeaderSetup {
    EntityID root;
    EntityID leader; // piece index 0
    DestructibleComponent* dc;
};

DetachedLeaderSetup SetupDetachedLeader(Scene& s, FractureLibrary& lib, ConvexColliderLibrary& colliders,
                                        RenderResources& resources, const char* assetKey,
                                        int32_t pieceCount)
{
    colliders.Init(&resources);
    lib.Init(&resources, &colliders);
    fracturelib::Install(&lib);
    convexcol::Install(&colliders);
    const FractureBakeResult rowBake = MakeRowFractureBake(pieceCount, 0.25f);
    const FractureAssetHandle* handle = lib.RegisterBaked(
        assetKey, rowBake, HashStr(std::string(assetKey) + "_src"), 0, pieceCount, 0, 0);

    World& w = s.GetWorld();
    GameObject root = s.CreateGameObject("Row");
    root.AddComponent<RigidbodyComponent>();
    root.AddComponent<DestructibleComponent>();
    root.GetComponent<RigidbodyComponent>()->mass = static_cast<float>(pieceCount);
    auto* d = root.GetComponent<DestructibleComponent>();
    d->strength = 100.0f;
    d->fractureAsset = AssetID{ HashStr(assetKey) };
    BuildFracturePieces(w, root.Id(), *handle);
    w.ApplyStructuralChanges();

    const EntityID leader = FindPieceChild(w, root.Id(), 0);
    FractureSystem fsys;
    std::vector<ShapeImpulse> none;
    ApplyFractureDamage(w, root.Id(), rowBake.pieces[0].origin, 0.0f, 150.0f);
    fsys.Update(w, 1.0f / 60.0f, none);
    w.ApplyStructuralChanges();

    return { root.Id(), leader, d };
}

} // namespace

bool RunFractureSelfTest()
{
    MYE_LOG_INFO("==== Fracture mesh core (M80a) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 1. 閉じ判定 ----
    // 平面切断の入力は「閉じていて外向き (signedVolume > 0)」が前提。ここで全テスト入力
    // メッシュの符号付き体積を検査し、生成メッシュが内向きに巻かれる事故を機械的に防ぐ
    {
        const ClosedMeshCheck box = CheckClosedMesh(MakeBox(1, 1, 1));
        check(box.closed, "box: closed");
        check(box.signedVolume > 0.0, "box: outward (signedVolume > 0)");

        const ClosedMeshCheck torus = CheckClosedMesh(MakeTorus(2.0f, 0.6f, 24, 16));
        check(torus.closed, "torus: closed");
        check(torus.signedVolume > 0.0, "torus: outward (signedVolume > 0)");

        const ClosedMeshCheck seamed = CheckClosedMesh(MakeSeamedBox(1, 1, 1));
        check(seamed.closed, "seamed box (UV継ぎ目で頂点が割れた箱): closed");
        check(seamed.signedVolume > 0.0, "seamed box: outward (signedVolume > 0)");

        const ClosedMeshCheck lshape = CheckClosedMesh(MakeLShapePrism(1.0f));
        check(lshape.closed, "lshape prism: closed");
        check(lshape.signedVolume > 0.0, "lshape prism: outward (signedVolume > 0)");

        const ClosedMeshCheck openBox = CheckClosedMesh(MakeOpenBox(1, 1, 1));
        check(!openBox.closed && openBox.boundaryEdges == 4, "open box: 4 boundary edges");

        const ClosedMeshCheck quad = CheckClosedMesh(MakePlaneQuad(1));
        check(!quad.closed && quad.boundaryEdges == 4, "plane quad: 4 boundary edges");

        const ClosedMeshCheck nonManifold = CheckClosedMesh(MakeNonManifold(1, 1, 1));
        check(!nonManifold.closed && nonManifold.nonManifoldEdges >= 1,
              "non-manifold (edge shared by 3 faces): detected");

        const ClosedMeshCheck flipped = CheckClosedMesh(MakeFlippedFaceBox(1, 1, 1));
        check(!flipped.closed && flipped.orientationMismatches >= 1,
              "flipped face box: orientation mismatch detected");
    }

    // ---- 1b. 内向きメッシュの正規化: 全面が内向きに巻かれた閉じたメッシュを検出し、
    // FlipMeshWinding で外向きへ正規化してから切ると両側とも外向きで閉じる ----
    {
        FractureMesh inwardTorus = MakeTorus(2.0f, 0.6f, 24, 16);
        FlipMeshWinding(inwardTorus); // わざと内向きにする
        const ClosedMeshCheck inwardCheck = CheckClosedMesh(inwardTorus);
        check(inwardCheck.closed && inwardCheck.signedVolume < 0.0,
              "inward torus: closed but signedVolume < 0 (detected)");

        FlipMeshWinding(inwardTorus); // 正規化
        const ClosedMeshCheck normalizedCheck = CheckClosedMesh(inwardTorus);
        check(normalizedCheck.closed && normalizedCheck.signedVolume > 0.0,
              "inward torus: FlipMeshWinding normalizes to outward");

        PlaneCutResult r;
        const bool ok = CutMeshByPlane(inwardTorus, { 1, 0, 0 }, 0.5f, r);
        check(ok && r.success, "normalized torus: CutMeshByPlane succeeds");
        if (ok && r.success) {
            auto sideOutward = [](const PlaneCutSide& side) {
                FractureMesh combined;
                combined.verts = side.outer.verts;
                combined.indices = side.outer.indices;
                const int32_t base = static_cast<int32_t>(combined.verts.size());
                combined.verts.insert(combined.verts.end(), side.cap.verts.begin(),
                                      side.cap.verts.end());
                for (int32_t idx : side.cap.indices) {
                    combined.indices.push_back(idx + base);
                }
                const ClosedMeshCheck c = CheckClosedMesh(combined);
                return c.closed && c.signedVolume > 0.0;
            };
            check(sideOutward(r.positive), "normalized torus: positive side closed and outward");
            check(sideOutward(r.negative), "normalized torus: negative side closed and outward");
        }
    }

    // ---- 2. 平面切断 + 蓋: 箱・L字・トーラスを複数平面で切り、両側の閉じ判定と体積和を確認 ----
    auto testCut = [&](const char* label, const FractureMesh& mesh, XMFLOAT3 n, float d) {
        char volBuf[256];
        std::snprintf(volBuf, sizeof(volBuf), "%s: input mesh is outward (signedVolume > 0)", label);
        check(SignedVolume(mesh) > 0.0, volBuf);
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        n = { n.x / len, n.y / len, n.z / len };
        d /= len; // n・x = d は n の縮尺に依存するので、正規化に合わせて d も縮める
        PlaneCutResult r;
        const bool ok = CutMeshByPlane(mesh, n, d, r);
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s: CutMeshByPlane succeeds", label);
        check(ok && r.success, buf);
        if (!ok || !r.success) {
            MYE_LOG_ERROR("    reason: %s", r.failReason.c_str());
            return;
        }
        std::snprintf(buf, sizeof(buf), "%s: positive side closed", label);
        check(SideIsClosed(r.positive), buf);
        std::snprintf(buf, sizeof(buf), "%s: negative side closed", label);
        check(SideIsClosed(r.negative), buf);
        const double total = SignedVolume(mesh);
        const double sum = SideVolume(r.positive) + SideVolume(r.negative);
        std::snprintf(buf, sizeof(buf), "%s: volume conserved (%.6f vs %.6f)", label, sum, total);
        check(Near(sum, total, 1e-4, 1e-7), buf);
    };

    {
        const FractureMesh box = MakeBox(1, 1, 1);
        testCut("box/axis-aligned", box, { 1, 0, 0 }, 0.0f);
        testCut("box/oblique", box, { 1, 1, 1 }, 0.3f);
        testCut("box/near-corner", box, { 0, 1, 0 }, 0.9f);

        const FractureMesh lshape = MakeLShapePrism(1.0f);
        // 凹んだ角の近くを横切る (頂点をちょうど通る配置は §4 のエッジケースで別途確認する)
        testCut("lshape/axis-aligned", lshape, { 1, 0, 0 }, 0.85f);
        testCut("lshape/oblique", lshape, { 1, 1, 0 }, 1.2f);

        const FractureMesh torus = MakeTorus(2.0f, 0.6f, 28, 20);
        testCut("torus/axis-aligned", torus, { 1, 0, 0 }, 0.5f);
        testCut("torus/oblique", torus, { 0.3f, 1, 0.2f }, 0.8f);
    }

    // ---- 3. トーラスの輪切り (穴のある蓋) ----
    {
        const float majorR = 2.0f, minorR = 0.6f;
        // minorSeg は輪切り面 (外周・内周) の円弧近似の細かさを決める。線形補間による誤差は
        // (2π/minorSeg)^2/8 程度で減るので、相対 1e-3 の許容に対して 96 分割の余裕を見る
        const int32_t majorSeg = 48, minorSeg = 96;
        const FractureMesh torus = MakeTorus(majorR, minorR, majorSeg, minorSeg);
        check(SignedVolume(torus) > 0.0, "torus slice: input mesh is outward (signedVolume > 0)");
        const float k = 0.2f; // 赤道からずらした高さ (軸 = Y)
        PlaneCutResult r;
        const bool ok = CutMeshByPlane(torus, { 0, 1, 0 }, k, r);
        check(ok && r.success, "torus slice (annulus): CutMeshByPlane succeeds");
        if (!ok || !r.success) {
            MYE_LOG_ERROR("    reason: %s", r.failReason.c_str());
        }
        if (ok && r.success) {
            check(SideIsClosed(r.positive), "torus slice: positive side closed");
            check(SideIsClosed(r.negative), "torus slice: negative side closed");
            const double total = SignedVolume(torus);
            const double sum = SideVolume(r.positive) + SideVolume(r.negative);
            check(Near(sum, total, 1e-4, 1e-7), "torus slice: volume conserved");

            // 蓋 (穴あき) の面積 = 外周 majorSeg 角形 − 内周 majorSeg 角形の面積 (多角形近似)
            const double half = std::sqrt(static_cast<double>(minorR) * minorR
                                          - static_cast<double>(k) * k);
            const double outerR = majorR + half;
            const double innerR = majorR - half;
            const auto regularNGonArea = [&](double radius) {
                return 0.5 * majorSeg * radius * radius * std::sin(2.0 * XM_PI / majorSeg);
            };
            const double expectedCapArea = regularNGonArea(outerR) - regularNGonArea(innerR);
            double negCapArea = 0.0;
            for (int32_t t = 0; t < r.negative.cap.TriCount(); ++t) {
                const XMFLOAT3& p0 = r.negative.cap.verts[static_cast<size_t>(r.negative.cap.indices[static_cast<size_t>(t) * 3 + 0])].position;
                const XMFLOAT3& p1 = r.negative.cap.verts[static_cast<size_t>(r.negative.cap.indices[static_cast<size_t>(t) * 3 + 1])].position;
                const XMFLOAT3& p2 = r.negative.cap.verts[static_cast<size_t>(r.negative.cap.indices[static_cast<size_t>(t) * 3 + 2])].position;
                const XMFLOAT3 e1 = { p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
                const XMFLOAT3 e2 = { p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
                const XMFLOAT3 cr = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                                      e1.x * e2.y - e1.y * e2.x };
                negCapArea += 0.5 * std::sqrt(static_cast<double>(cr.x) * cr.x
                                              + static_cast<double>(cr.y) * cr.y
                                              + static_cast<double>(cr.z) * cr.z);
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf), "torus slice: hole cap area matches annulus (%.6f vs %.6f)",
                          negCapArea, expectedCapArea);
            check(Near(negCapArea, expectedCapArea, 1e-3, 1e-6), buf);
        }
    }

    // ---- 4. エッジケース ----
    {
        const FractureMesh box = MakeBox(1, 1, 1);
        // 平面がメッシュに触れない (全部片側)
        PlaneCutResult r;
        bool ok = CutMeshByPlane(box, { 0, 1, 0 }, 5.0f, r);
        check(ok && r.success && r.negative.outer.TriCount() == box.TriCount()
                  && r.positive.outer.TriCount() == 0 && r.positive.cap.TriCount() == 0,
              "plane misses mesh entirely: negative side unchanged, positive side empty");

        // 面の対角 (頂点をちょうど通る斜め平面) — 呼び出し自体は落ちない (成功/失敗どちらも許容)。
        // 成功したときだけ、閉じた結果になっていることを確認する
        PlaneCutResult r2;
        ok = CutMeshByPlane(box, { 1, 1, 0 }, 0.0f, r2);
        if (!ok) {
            MYE_LOG_INFO("    (plane through face corners: 安全側に倒して失敗。reason=%s)",
                        r2.failReason.c_str());
        }
        check(!ok || (SideIsClosed(r2.positive) && SideIsClosed(r2.negative)),
              "plane through face corners: succeeds with closed sides, or fails safely");

        // 面上に乗る平面 (箱の +X 面とちょうど一致) — 同様に成功/失敗どちらも許容
        PlaneCutResult r3;
        ok = CutMeshByPlane(box, { 1, 0, 0 }, 1.0f, r3);
        if (!ok) {
            MYE_LOG_INFO("    (plane on face: 安全側に倒して失敗。reason=%s)", r3.failReason.c_str());
        }
        bool r3Ok = !ok;
        if (ok) {
            const double total = SignedVolume(box);
            const double sum = SideVolume(r3.positive) + SideVolume(r3.negative);
            r3Ok = Near(sum, total, 1e-3, 1e-6);
        }
        check(r3Ok, "plane on face: succeeds with volume conserved, or fails safely");
    }

    // ---- 4b. 輪郭が接触・交差する縮退入力 (sub-14、libtess2 の掃引線法への置き換え) ----
    // 「横棒」と「縦棒」の2つの箱を十字に重ねた1メッシュを、両方の胴の途中で水平に切る。
    // 断面は2つの矩形が十字に交差し、輪郭どうしが4点で実際に交差する。
    // 注記 (sub-14 実装時に判明): 頂点そのものを共有する接触 (例: 2つの箱が1頂点/1辺で
    // 触れる) は ChainAllLoops の前提 (溶接後の1位置につき出て行く辺は高々1本) に必ず抵触し、
    // CapLoops より前で「非多様体入力の疑い」として安全に失敗する (sub-14 の変更範囲外)。
    // この十字の重なりは面積を持つ実体交差で ChainAllLoops は通るが、2つの箱の体積が
    // 実際に重なっており、外側面 (両箱の壁をそのまま残す) と蓋 (libtess2 が TESS_WINDING_ODD
    // で重複領域を穴として除く) の間に前提の食い違いが生じるため、CutMeshByPlane は
    // 安全側に倒して失敗を返す (成功はしない)。「落ちない」ことと「安全な理由付きで失敗する」
    // ことを検算する — 密な輪郭の頑健性そのものは受け入れ条件3 (res48/64 の焼き) で検算済み
    {
        FractureMesh bar1 = MakeBox(2.0f, 0.5f, 0.5f); // x:[-2,2] y:[-0.5,0.5] z:[-0.5,0.5]
        FractureMesh bar2 = MakeBox(0.5f, 2.0f, 0.5f); // x:[-0.5,0.5] y:[-2,2] z:[-0.5,0.5]
        FractureMesh cross;
        cross.verts = bar1.verts;
        cross.indices = bar1.indices;
        const int32_t base = static_cast<int32_t>(cross.verts.size());
        cross.verts.insert(cross.verts.end(), bar2.verts.begin(), bar2.verts.end());
        for (int32_t idx : bar2.indices) {
            cross.indices.push_back(idx + base);
        }

        auto sideGeometricallyValid = [&](const PlaneCutSide& side) {
            double vx = 0.0, vy = 0.0, vz = 0.0, surfaceArea = 0.0;
            auto accumulate = [&](const FractureMesh& mesh) {
                const int32_t triCount = mesh.TriCount();
                for (int32_t t = 0; t < triCount; ++t) {
                    const XMFLOAT3& a = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 0])].position;
                    const XMFLOAT3& b = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 1])].position;
                    const XMFLOAT3& c = mesh.verts[static_cast<size_t>(mesh.indices[static_cast<size_t>(t) * 3 + 2])].position;
                    const double e1x = static_cast<double>(b.x) - a.x, e1y = static_cast<double>(b.y) - a.y,
                                e1z = static_cast<double>(b.z) - a.z;
                    const double e2x = static_cast<double>(c.x) - a.x, e2y = static_cast<double>(c.y) - a.y,
                                e2z = static_cast<double>(c.z) - a.z;
                    const double cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z,
                                cz = e1x * e2y - e1y * e2x;
                    vx += cx * 0.5;
                    vy += cy * 0.5;
                    vz += cz * 0.5;
                    surfaceArea += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
                }
            };
            accumulate(side.outer);
            accumulate(side.cap);
            const double vectorAreaMag = std::sqrt(vx * vx + vy * vy + vz * vz);
            return SideVolume(side) > 0.0 && vectorAreaMag <= 1e-4 * surfaceArea;
        };

        // 呼び出しが完了すること自体が「落ちない」ことの検証 (クラッシュしていれば後続の
        // チェックへ到達しない)。体積が実際に重なる縮退入力なので、成功なら結果が幾何的に
        // 閉じていること、失敗なら理由付きで安全に失敗すること (success=false のまま
        // 中途半端な結果を返さない) を確認する
        PlaneCutResult r;
        const bool ok = CutMeshByPlane(cross, { 0, 0, 1 }, 0.0f, r);
        if (ok && r.success) {
            check(sideGeometricallyValid(r.positive), "crossing contours: positive side is geometrically closed");
            check(sideGeometricallyValid(r.negative), "crossing contours: negative side is geometrically closed");
        } else {
            check(!r.failReason.empty(),
                  "crossing contours: fails safely with a reason instead of crashing or returning bad data");
            MYE_LOG_INFO("    (crossing contours: 安全側に倒して失敗。reason=%s)", r.failReason.c_str());
        }
    }

    // ---- 5. 決定論: 同じ入力を2回切って出力のバイト列が一致する ----
    {
        const FractureMesh torus = MakeTorus(2.0f, 0.6f, 28, 20);
        PlaneCutResult a, b;
        CutMeshByPlane(torus, { 0.3f, 1, 0.2f }, 0.8f, a);
        CutMeshByPlane(torus, { 0.3f, 1, 0.2f }, 0.8f, b);
        const std::vector<uint8_t> ba = SerializeCutResult(a);
        const std::vector<uint8_t> bb = SerializeCutResult(b);
        check(ba.size() == bb.size() && std::memcmp(ba.data(), bb.data(), ba.size()) == 0,
              "determinism: cutting the same input twice yields byte-identical output");
    }

    // ---- 6. 処理時間の記録 (焼き時間見積もり用。合否には数えない) ----
    {
        const FractureMesh big = MakeTorus(2.0f, 0.6f, 100, 50); // 100*50*2 = 10000 三角形
        const auto t0 = std::chrono::steady_clock::now();
        for (int32_t i = 0; i < 16; ++i) {
            const float a = static_cast<float>(i) * 0.4f;
            PlaneCutResult r;
            CutMeshByPlane(big, { std::sin(a), std::cos(a) * 0.5f, std::cos(a) }, 0.3f, r);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        MYE_LOG_INFO("  timing: %d tri x 16 planes = %.2f ms (%.3f ms/cut)", big.TriCount(), ms,
                    ms / 16.0);
    }

    // ---- 7. Voronoi 分割: 箱・L字・トーラスの全破片が閉じ、体積和が保存され、凸包が有効 ----
    {
        auto testBake = [&](const char* label, const FractureMesh& mesh, uint32_t seed, int32_t pieceCount) {
            FractureBakeInput in;
            in.sourceMesh = mesh;
            in.seed = seed;
            in.pieceCount = pieceCount;
            FractureBakeResult r;
            const bool ok = BakeFracture(in, r);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s: BakeFracture succeeds (requested=%d placed=%d)", label,
                          pieceCount, r.seedsPlaced);
            check(ok && r.success, buf);
            if (!ok || !r.success) {
                MYE_LOG_ERROR("    reason: %s", r.failReason.c_str());
                return;
            }
            bool allValid = true, allHullValid = true;
            for (const FracturePieceBake& p : r.pieces) {
                if (!PieceGeometryValid(p)) {
                    allValid = false;
                }
                if (!p.hull.Valid()) {
                    allHullValid = false;
                }
            }
            std::snprintf(buf, sizeof(buf), "%s: all %d pieces have volume>0 and no missing faces", label,
                          static_cast<int>(r.pieces.size()));
            check(allValid, buf);
            std::snprintf(buf, sizeof(buf), "%s: all pieces have a valid convex hull", label);
            check(allHullValid, buf);
            const double total = SignedVolume(mesh);
            const double sum = TotalBakedVolume(r);
            std::snprintf(buf, sizeof(buf), "%s: volume conserved (%.6f vs %.6f)", label, sum, total);
            check(Near(sum, total, 1e-4, 1e-7), buf);
        };
        testBake("voronoi box/8", MakeBox(1, 1, 1), 1, 8);
        testBake("voronoi box/32", MakeBox(1, 1, 1), 2, 32);
        testBake("voronoi lshape/12", MakeLShapePrism(1.0f), 3, 12);
        testBake("voronoi torus/8", MakeTorus(2.0f, 0.6f, 24, 16), 1, 8);
        testBake("voronoi torus/32", MakeTorus(2.0f, 0.6f, 24, 16), 4, 32);
    }
    // ---- 8. 非連結の分離: L字をまたぐセルで破片数がシード数より増える ----
    // L字プリズムの断面 (0,0)-(2,0)-(2,1)-(1,1)-(1,2)-(0,2) は x=1,y=1 の角が凹んでいる
    // (x>1 かつ y>1 の正方形が欠けている)。2 点だけの明示シードなら、その 2 分割線は
    // 「垂直二等分面」1 枚だけになる (他シードが無いので候補面が 1 枚だけ)。2 点を
    // (0.25,0.25,z), (2.25,2.25,z) に置くと二等分面は x+y=2.5 になり、これは断面の
    // 凹み (x=1..2, y=1..2 の欠けた正方形) を斜めに横切る。seed1 側 (x+y>2.5) は
    // 「上腕の (1,2) 角付近」と「右腕の (2,1) 角付近」という、凹みで隔てられた
    // 2 つの三角柱に分かれる — 幾何的に本当に非連結になる配置 (乱数探索ではなく設計値)
    {
        const FractureMesh lshape = MakeLShapePrism(1.0f);
        const std::vector<XMFLOAT3> seeds = { { 0.25f, 0.25f, 0.5f }, { 2.25f, 2.25f, 0.5f } };
        FractureBakeResult r;
        const bool ok = BakeFractureWithSeeds(lshape, seeds, 0.0f, r);
        check(ok && r.success, "voronoi lshape: disconnected-cell bake succeeds");
        if (ok && r.success) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "voronoi lshape: disconnected seed cell yields pieces > seeds (placed=%d, pieces=%d)",
                          r.seedsPlaced, static_cast<int>(r.pieces.size()));
            check(static_cast<int32_t>(r.pieces.size()) > r.seedsPlaced, buf);
        }
    }

    // ---- 9. 極小片の統合: 統合後に平均 x minVolumeRatio 未満の破片が無い ----
    {
        FractureBakeInput in;
        in.sourceMesh = MakeBox(1, 1, 1);
        in.seed = 7;
        in.pieceCount = 24;
        in.minVolumeRatio = 0.5f; // 大きめの比で必ず統合を起こす
        FractureBakeResult r;
        const bool ok = BakeFracture(in, r);
        check(ok && r.success, "voronoi merge: BakeFracture succeeds");
        if (ok && r.success) {
            double avg = 0.0;
            for (const FracturePieceBake& p : r.pieces) {
                avg += p.volume;
            }
            avg /= static_cast<double>(r.pieces.size());
            const double threshold = avg * 0.5;
            bool allAbove = true;
            for (const FracturePieceBake& p : r.pieces) {
                if (p.volume < threshold - 1e-9) {
                    allAbove = false;
                }
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "voronoi merge: no piece under avg*ratio after merge (merged=%d, pieces=%d)",
                          r.mergedCount, static_cast<int>(r.pieces.size()));
            check(allAbove, buf);
        }
    }

    // ---- 10. 接着グラフ: 箱を軸平行に2分割する明示シードで隣接1本・面積が断面積と一致 ----
    {
        const FractureMesh box = MakeBox(1, 1, 1);
        const std::vector<XMFLOAT3> seeds = { { -0.5f, 0, 0 }, { 0.5f, 0, 0 } };
        FractureBakeResult r;
        const bool ok = BakeFractureWithSeeds(box, seeds, 0.1f, r);
        check(ok && r.success && r.pieces.size() == 2, "voronoi adjacency: 2 explicit seeds yield 2 pieces");
        if (ok && r.success && r.pieces.size() == 2) {
            const bool eachHasOneNeighbor
                = r.pieces[0].neighbors.size() == 1 && r.pieces[1].neighbors.size() == 1;
            check(eachHasOneNeighbor, "voronoi adjacency: each piece has exactly 1 neighbor");
            if (eachHasOneNeighbor) {
                check(r.pieces[0].neighbors[0].pieceIndex == 1 && r.pieces[1].neighbors[0].pieceIndex == 0,
                      "voronoi adjacency: symmetric (piece0 <-> piece1)");
                const double area0 = r.pieces[0].neighbors[0].area;
                const double area1 = r.pieces[1].neighbors[0].area;
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "voronoi adjacency: area matches cross-section (%.6f, %.6f vs 4.0)", area0, area1);
                check(Near(area0, 4.0, 1e-4, 1e-6) && Near(area1, 4.0, 1e-4, 1e-6), buf);
            }
            check(r.pieces[0].neighbors.size() <= static_cast<size_t>(kMaxFractureNeighbors)
                      && r.pieces[1].neighbors.size() <= static_cast<size_t>(kMaxFractureNeighbors)
                      && r.pieces[0].droppedNeighbors == 0 && r.pieces[1].droppedNeighbors == 0,
                  "voronoi adjacency: neighbor count within 32, none dropped");
        }
    }

    // ---- 11. 決定論: 同じ入力の digest が一致し、seed を変えると変わる (Debug/Release 比較用ログ) ----
    {
        const FractureMesh lshape = MakeLShapePrism(1.0f);
        FractureBakeInput in;
        in.sourceMesh = lshape;
        in.seed = 42;
        in.pieceCount = 10;
        FractureBakeResult a, b, c;
        const bool okA = BakeFracture(in, a);
        const bool okB = BakeFracture(in, b);
        in.seed = 43;
        const bool okC = BakeFracture(in, c);
        check(okA && okB && okC && a.success && b.success && c.success, "voronoi digest: bakes succeed");
        if (okA && okB && okC && a.success && b.success && c.success) {
            const uint64_t digestA = FractureBakeDigest(a);
            const uint64_t digestB = FractureBakeDigest(b);
            const uint64_t digestC = FractureBakeDigest(c);
            check(digestA == digestB, "voronoi digest: same input yields the same digest");
            check(digestA != digestC, "voronoi digest: different seed yields a different digest");
            MYE_LOG_INFO("  fracture bake digest (lshape seed=42 pieces=%d): 0x%016llX",
                        static_cast<int>(a.pieces.size()), static_cast<unsigned long long>(digestA));
        }

        // トーラス (BuildConvexHull の無限ループ修正後に追加) の digest も
        // Debug/Release 比較の対象にする
        FractureBakeInput torusIn;
        torusIn.sourceMesh = MakeTorus(2.0f, 0.6f, 24, 16);
        torusIn.seed = 44;
        torusIn.pieceCount = 8;
        FractureBakeResult ta, tb;
        const bool okTa = BakeFracture(torusIn, ta);
        const bool okTb = BakeFracture(torusIn, tb);
        check(okTa && okTb && ta.success && tb.success, "voronoi digest (torus): bakes succeed");
        if (okTa && okTb && ta.success && tb.success) {
            const uint64_t digestTa = FractureBakeDigest(ta);
            const uint64_t digestTb = FractureBakeDigest(tb);
            check(digestTa == digestTb, "voronoi digest (torus): same input yields the same digest");
            MYE_LOG_INFO("  fracture bake digest (torus seed=44 pieces=%d): 0x%016llX",
                        static_cast<int>(ta.pieces.size()), static_cast<unsigned long long>(digestTa));
        }
    }

    // ---- 12. 焼き時間の記録 (上限決定用。合否には数えない) ----
    {
        auto timeBake = [&](const char* label, const FractureMesh& mesh, uint32_t seed, int32_t pieceCount) {
            FractureBakeInput in;
            in.sourceMesh = mesh;
            in.seed = seed;
            in.pieceCount = pieceCount;
            FractureBakeResult r;
            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = BakeFracture(in, r);
            const auto t1 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            MYE_LOG_INFO("  bake timing: %s (%d tri, pieceCount=%d, placed=%d, pieces=%d) = %.2f ms", label,
                        mesh.TriCount(), pieceCount, r.seedsPlaced, ok ? static_cast<int>(r.pieces.size()) : -1,
                        ms);
        };
        timeBake("box / 8 pieces", MakeBox(1, 1, 1), 200, 8);
        timeBake("box / 32 pieces", MakeBox(1, 1, 1), 201, 32);
        timeBake("lshape / 12 pieces", MakeLShapePrism(1.0f), 202, 12);
        // BuildConvexHull の無限ループ修正後に追加 (768 三角形、32 破片)
        timeBake("torus / 8 pieces", MakeTorus(2.0f, 0.6f, 24, 16), 203, 8);
        timeBake("torus / 32 pieces", MakeTorus(2.0f, 0.6f, 24, 16), 204, 32);
    }

    // ---- 13. 破片資産 (.mfrac、M80c): 書く→読む→書くのバイト一致、壊れた入力の安全な失敗、
    //          MeshLibrary/ConvexColliderLibrary への登録と Clear() 後の再登録 ----
    {
        auto piecesEqual = [](const FractureAsset::PieceRecord& a, const FractureAsset::PieceRecord& b) {
            if (a.outerVerts.size() != b.outerVerts.size() || a.outerIndices.size() != b.outerIndices.size()
                || a.capVerts.size() != b.capVerts.size() || a.capIndices.size() != b.capIndices.size()
                || a.neighbors.size() != b.neighbors.size() || a.hull.verts.size() != b.hull.verts.size()
                || a.boneName != b.boneName || a.volume != b.volume || a.outerIndices != b.outerIndices
                || a.capIndices != b.capIndices) {
                return false;
            }
            if (a.origin.x != b.origin.x || a.origin.y != b.origin.y || a.origin.z != b.origin.z) {
                return false;
            }
            if (!a.outerVerts.empty()
                && std::memcmp(a.outerVerts.data(), b.outerVerts.data(), a.outerVerts.size() * sizeof(MeshVertex))
                    != 0) {
                return false;
            }
            if (!a.capVerts.empty()
                && std::memcmp(a.capVerts.data(), b.capVerts.data(), a.capVerts.size() * sizeof(MeshVertex)) != 0) {
                return false;
            }
            for (size_t i = 0; i < a.neighbors.size(); ++i) {
                if (a.neighbors[i].pieceIndex != b.neighbors[i].pieceIndex
                    || a.neighbors[i].area != b.neighbors[i].area) {
                    return false;
                }
            }
            return true;
        };

        FractureBakeInput in;
        in.sourceMesh = MakeLShapePrism(1.0f);
        in.seed = 55;
        in.pieceCount = 6;
        FractureBakeResult bake;
        const bool baked = BakeFracture(in, bake);
        check(baked && bake.success, "fracture asset: source bake succeeds");
        if (baked && bake.success) {
            const FractureAsset::FractureData data
                = BuildFractureAssetData(bake, 0x1234ULL, in.seed, in.pieceCount, 0, 64);

            // (13a) 書く→読む→書く のバイト一致。読んだ破片メッシュ・凸包・隣接が焼き結果と一致
            std::vector<uint8_t> bytesA, bytesB;
            FractureAsset::Serialize(data, bytesA);
            FractureAsset::FractureData roundTripped;
            const bool readOk = FractureAsset::Deserialize(bytesA, roundTripped);
            check(readOk, "fracture asset: Deserialize(Serialize(data)) succeeds");
            if (readOk) {
                FractureAsset::Serialize(roundTripped, bytesB);
                check(bytesA.size() == bytesB.size()
                          && std::memcmp(bytesA.data(), bytesB.data(), bytesA.size()) == 0,
                      "fracture asset: write->read->write is byte-identical");

                bool piecesMatch = roundTripped.pieces.size() == data.pieces.size();
                for (size_t i = 0; piecesMatch && i < roundTripped.pieces.size(); ++i) {
                    piecesMatch = piecesEqual(data.pieces[i], roundTripped.pieces[i]);
                }
                check(piecesMatch, "fracture asset: read-back pieces/hull/neighbors match the bake result");
            }

            // (13b) 壊れたファイル: 落ちずに失敗を返す
            {
                std::vector<uint8_t> truncated(bytesA.begin(), bytesA.begin() + bytesA.size() / 2);
                FractureAsset::FractureData discard;
                check(!FractureAsset::Deserialize(truncated, discard),
                      "fracture asset: truncated blob fails safely");

                std::vector<uint8_t> badMagic = bytesA;
                badMagic[0] ^= 0xFF;
                check(!FractureAsset::Deserialize(badMagic, discard), "fracture asset: wrong magic fails safely");

                std::vector<uint8_t> badVersion = bytesA;
                badVersion[4] ^= 0xFF; // magic(4B) の直後が version
                check(!FractureAsset::Deserialize(badVersion, discard),
                      "fracture asset: wrong version fails safely");
            }

            // (13c) 件数上限超え: 構造的に超過させたデータは落ちずに失敗する
            {
                FractureAsset::FractureData tooManyPieces;
                tooManyPieces.pieces.resize(static_cast<size_t>(kMaxFracturePieces) + 1);
                std::vector<uint8_t> blob;
                FractureAsset::Serialize(tooManyPieces, blob);
                FractureAsset::FractureData discard;
                check(!FractureAsset::Deserialize(blob, discard),
                      "fracture asset: piece count above the cap fails safely");

                FractureAsset::FractureData tooManyNeighbors;
                tooManyNeighbors.pieces.resize(1);
                tooManyNeighbors.pieces[0].neighbors.resize(static_cast<size_t>(kMaxFractureNeighbors) + 1);
                std::vector<uint8_t> blob2;
                FractureAsset::Serialize(tooManyNeighbors, blob2);
                check(!FractureAsset::Deserialize(blob2, discard),
                      "fracture asset: neighbor count above the cap fails safely");
            }

            // (13c2) 一部のバイトだけ壊れた入力: 範囲外/自己参照/非対称な index は失敗する
            // (フォーマット自体は正しいので、値だけを壊す)
            {
                FractureAsset::FractureData discard;
                auto findAdjacentPair = [&](size_t& outI, size_t& outJ) -> bool {
                    for (size_t i = 0; i < data.pieces.size(); ++i) {
                        if (data.pieces[i].neighbors.empty()) {
                            continue;
                        }
                        outI = i;
                        outJ = static_cast<size_t>(data.pieces[i].neighbors.front().pieceIndex);
                        return true;
                    }
                    return false;
                };

                FractureAsset::FractureData badOuterIdx = data;
                if (!badOuterIdx.pieces.empty() && !badOuterIdx.pieces[0].outerIndices.empty()) {
                    badOuterIdx.pieces[0].outerIndices[0]
                        = static_cast<uint32_t>(badOuterIdx.pieces[0].outerVerts.size()) + 7;
                    std::vector<uint8_t> blob3;
                    FractureAsset::Serialize(badOuterIdx, blob3);
                    check(!FractureAsset::Deserialize(blob3, discard),
                          "fracture asset: an outer index past the vertex count fails safely");
                }

                FractureAsset::FractureData badCapIdx = data;
                if (!badCapIdx.pieces.empty() && !badCapIdx.pieces[0].capIndices.empty()) {
                    badCapIdx.pieces[0].capIndices[0]
                        = static_cast<uint32_t>(badCapIdx.pieces[0].capVerts.size()) + 7;
                    std::vector<uint8_t> blob4;
                    FractureAsset::Serialize(badCapIdx, blob4);
                    check(!FractureAsset::Deserialize(blob4, discard),
                          "fracture asset: a cap index past the vertex count fails safely");
                }

                FractureAsset::FractureData badNeighborRange = data;
                if (!badNeighborRange.pieces.empty()) {
                    badNeighborRange.pieces[0].neighbors.push_back(
                        { static_cast<int32_t>(badNeighborRange.pieces.size()) + 3, 1.0f });
                    std::vector<uint8_t> blob5;
                    FractureAsset::Serialize(badNeighborRange, blob5);
                    check(!FractureAsset::Deserialize(blob5, discard),
                          "fracture asset: a neighbor index past the piece count fails safely");
                }

                FractureAsset::FractureData selfNeighbor = data;
                if (!selfNeighbor.pieces.empty()) {
                    selfNeighbor.pieces[0].neighbors.push_back({ 0, 1.0f }); // 自分自身への隣接
                    std::vector<uint8_t> blob6;
                    FractureAsset::Serialize(selfNeighbor, blob6);
                    check(!FractureAsset::Deserialize(blob6, discard),
                          "fracture asset: a self-referencing neighbor fails safely");
                }

                size_t pi = 0, pj = 0;
                if (findAdjacentPair(pi, pj)) {
                    FractureAsset::FractureData asymmetric = data;
                    auto& backRefs = asymmetric.pieces[pj].neighbors;
                    backRefs.erase(std::remove_if(backRefs.begin(), backRefs.end(),
                                                  [pi](const FractureAsset::NeighborRecord& n) {
                                                      return static_cast<size_t>(n.pieceIndex) == pi;
                                                  }),
                                  backRefs.end());
                    std::vector<uint8_t> blob7;
                    FractureAsset::Serialize(asymmetric, blob7);
                    check(!FractureAsset::Deserialize(blob7, discard),
                          "fracture asset: a one-sided (asymmetric) neighbor fails safely");
                } else {
                    check(false, "fracture asset: setup expected an adjacent piece pair to exist");
                }
            }

            // (13d) FractureLibrary: メモリ登録 (GUID なし) → MeshLibrary/ConvexColliderLibrary へ
            //       登録 → Clear() で凸包を失う → ReregisterAll() で復帰する
            {
                RenderResources resources;
                ConvexColliderLibrary colliders;
                colliders.Init(&resources);
                FractureLibrary fractureLib;
                fractureLib.Init(&resources, &colliders);

                const FractureAssetHandle* handle = fractureLib.RegisterBaked(
                    "fracture://selftest_lshape", bake, 0xABCDULL, in.seed, in.pieceCount, 0, 64);
                check(handle != nullptr && handle->pieces.size() == bake.pieces.size(),
                      "fracture library: RegisterBaked registers every piece (no-GUID path)");
                if (handle != nullptr && !handle->pieces.empty()) {
                    const AssetID hullId = handle->pieces[0].hull;
                    check(resources.meshes.Get(handle->pieces[0].outerMesh) != nullptr,
                          "fracture library: outer mesh registered in MeshLibrary");
                    check(resources.meshes.Get(handle->pieces[0].capMesh) != nullptr,
                          "fracture library: cap mesh registered in MeshLibrary");
                    check(colliders.Get(hullId) != nullptr,
                          "fracture library: hull registered in ConvexColliderLibrary");

                    colliders.Clear();
                    check(colliders.Get(hullId) == nullptr,
                          "fracture library: Clear() forgets the hull (baseline for the next check)");

                    fractureLib.ReregisterAll();
                    check(colliders.Get(hullId) != nullptr,
                          "fracture library: ReregisterAll() restores the hull after Clear()");
                }
            }

            // (13e) 実ファイルの往復: Save/Load がバイト一致、LoadFromFile は同じパスを読み直さない
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                const fs::path tempDir = fs::temp_directory_path(ec) / L"mye_fracture_selftest";
                fs::create_directories(tempDir, ec);
                const fs::path filePath = tempDir / L"test.mfrac";
                fs::remove(filePath, ec);

                const bool saved = FractureAsset::Save(filePath.wstring(), data);
                check(saved, "fracture asset: Save() writes the file");
                if (saved) {
                    FractureAsset::FractureData loaded;
                    check(FractureAsset::Load(filePath.wstring(), loaded), "fracture asset: Load() reads it back");

                    std::vector<uint8_t> savedBytes, loadedBytes;
                    FractureAsset::Serialize(data, savedBytes);
                    FractureAsset::Serialize(loaded, loadedBytes);
                    check(savedBytes.size() == loadedBytes.size()
                              && std::memcmp(savedBytes.data(), loadedBytes.data(), savedBytes.size()) == 0,
                          "fracture asset: file round trip is byte-identical");

                    RenderResources resources2;
                    ConvexColliderLibrary colliders2;
                    colliders2.Init(&resources2);
                    FractureLibrary fractureLib2;
                    fractureLib2.Init(&resources2, &colliders2);
                    const FractureAssetHandle* h1 = fractureLib2.LoadFromFile(filePath.wstring());
                    const FractureAssetHandle* h2 = fractureLib2.LoadFromFile(filePath.wstring());
                    check(h1 != nullptr && h1 == h2,
                          "fracture library: LoadFromFile is idempotent for the same path");
                }
                fs::remove_all(tempDir, ec);
            }
        }

        // (13f) 見つからないファイル: 落ちずに失敗を返す (spec §4.1 エッジケース)
        {
            RenderResources resources3;
            ConvexColliderLibrary colliders3;
            colliders3.Init(&resources3);
            FractureLibrary fractureLib3;
            fractureLib3.Init(&resources3, &colliders3);
            const FractureAssetHandle* missing
                = fractureLib3.LoadFromFile(L"C:\\definitely\\not\\a\\real\\path.mfrac");
            check(missing == nullptr, "fracture library: missing .mfrac fails without crashing");
        }
    }

    // ---- 14. ボクセル化 + surface nets (M80d、開いたメッシュの経路) ----
    {
        auto voxelizeAndCheckClosed = [&](const char* label, const FractureMesh& mesh, int32_t resolution) {
            FractureVoxelizeResult vr;
            const bool ok = VoxelizeMeshForFracture(mesh, resolution, vr);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s: VoxelizeMeshForFracture(res=%d) succeeds", label, resolution);
            check(ok && vr.success, buf);
            if (!ok || !vr.success) {
                MYE_LOG_ERROR("    reason: %s", vr.failReason.c_str());
                return vr;
            }
            const ClosedMeshCheck c = CheckClosedMesh(vr.mesh);
            std::snprintf(buf, sizeof(buf), "%s: voxelized result is closed and outward (tri=%d)", label,
                          vr.mesh.TriCount());
            check(c.closed && c.signedVolume > 0.0, buf);
            return vr;
        };

        voxelizeAndCheckClosed("open box", MakeOpenBox(1, 1, 1), 32);
        voxelizeAndCheckClosed("plane quad", MakePlaneQuad(1.0f), 32);
        voxelizeAndCheckClosed("double wall", MakeDoubleWallMesh(1.0f, 0.5f), 32);

        // ---- 解像度で細かさが変わり、AABB の差が (低解像度の) セル 2 個分以内 ----
        {
            const FractureMesh box = MakeOpenBox(1, 1, 1);
            FractureVoxelizeResult low, high;
            const bool okLow = VoxelizeMeshForFracture(box, 16, low);
            const bool okHigh = VoxelizeMeshForFracture(box, 64, high);
            check(okLow && low.success && okHigh && high.success, "voxelize: resolution 16/64 both succeed");
            if (okLow && low.success && okHigh && high.success) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "voxelize: triangle count increases with resolution (%d -> %d)",
                              low.mesh.TriCount(), high.mesh.TriCount());
                check(high.mesh.TriCount() > low.mesh.TriCount(), buf);

                XMFLOAT3 srcLo, srcHi, outLo, outHi;
                MeshAabbOf(box, srcLo, srcHi);
                const float cellLow = 2.0f / 16.0f; // MakeOpenBox(1,1,1) の最長辺 = 2
                MeshAabbOf(low.mesh, outLo, outHi);
                const float diffLow = (std::max)({ std::fabs(outLo.x - srcLo.x), std::fabs(outLo.y - srcLo.y),
                                                   std::fabs(outLo.z - srcLo.z), std::fabs(outHi.x - srcHi.x),
                                                   std::fabs(outHi.y - srcHi.y), std::fabs(outHi.z - srcHi.z) });
                std::snprintf(buf, sizeof(buf), "voxelize: res16 AABB within 2 cells of source (diff=%.4f, cell=%.4f)",
                              diffLow, cellLow);
                check(diffLow <= 2.0f * cellLow, buf);

                const float cellHigh = 2.0f / 64.0f;
                MeshAabbOf(high.mesh, outLo, outHi);
                const float diffHigh = (std::max)({ std::fabs(outLo.x - srcLo.x), std::fabs(outLo.y - srcLo.y),
                                                    std::fabs(outLo.z - srcLo.z), std::fabs(outHi.x - srcHi.x),
                                                    std::fabs(outHi.y - srcHi.y), std::fabs(outHi.z - srcHi.z) });
                std::snprintf(buf, sizeof(buf), "voxelize: res64 AABB within 2 cells of source (diff=%.4f, cell=%.4f)",
                              diffHigh, cellHigh);
                check(diffHigh <= 2.0f * cellHigh, buf);
            }
        }

        // ---- 決定論: 同じ入力から 2 回のバイト列が一致する ----
        {
            FractureVoxelizeResult a, b;
            const bool okA = VoxelizeMeshForFracture(MakeOpenBox(1, 1, 1), 24, a);
            const bool okB = VoxelizeMeshForFracture(MakeOpenBox(1, 1, 1), 24, b);
            check(okA && a.success && okB && b.success, "voxelize: determinism bakes succeed");
            if (okA && a.success && okB && b.success) {
                check(SerializeMesh(a.mesh) == SerializeMesh(b.mesh),
                      "voxelize: same input yields a byte-identical mesh");
            }
        }

        // ---- BakeFracture の入口: openMeshMode 0 は理由付きで拒否 ----
        {
            FractureBakeInput in;
            in.sourceMesh = MakeOpenBox(1, 1, 1);
            in.seed = 9;
            in.pieceCount = 8;
            in.openMeshMode = 0;
            FractureBakeResult rejected;
            const bool okReject = BakeFracture(in, rejected);
            check(!okReject && !rejected.failReason.empty(),
                  "bake entry: open mesh with openMeshMode=0 is rejected with a reason");
        }

        // ---- surface nets であること (ブロック状ではない): 単独の1セルは、ブロック抽出なら
        // 一辺=hの立方体(体積=h^3)になるはずだが、surface netsは角が中心へ引き寄せられ
        // 体積がそれより小さい八面体状になる (3x3x3、中心の1セルだけ占有) ----
        {
            RawOccupancyGrid grid;
            grid.nx = grid.ny = grid.nz = 3;
            grid.occ.assign(27, 0);
            grid.occ[1 + 3 * (1 + 3 * 1)] = 1; // (1,1,1) だけ占有 (他は外周パディング)
            FractureMesh mesh;
            const bool built = BuildSurfaceNetsFromOccupancy(grid, 1.0f, mesh);
            check(built, "surface nets: single-cell occupancy builds a mesh");
            if (built) {
                const ClosedMeshCheck c = CheckClosedMesh(mesh);
                check(c.closed && c.signedVolume > 0.0, "surface nets: single-cell result is closed and outward");
                char buf[256];
                std::snprintf(buf, sizeof(buf), "surface nets: single-cell volume (%.4f) is smaller than a block cube (1.0)",
                              c.signedVolume);
                check(c.signedVolume < 1.0 - 1e-6, buf);
            }
        }

        // ---- 曖昧な配置 (2x2 で対角に並ぶ2ボクセル) でも閉じる: 4x4x3 の内部2x2x1に
        // (1,1,1)と(2,2,1)だけを占有させる (残る対角(2,1,1)/(1,2,1)は空き) ----
        {
            RawOccupancyGrid grid;
            grid.nx = grid.ny = 4;
            grid.nz = 3;
            grid.occ.assign(static_cast<size_t>(grid.nx) * grid.ny * grid.nz, 0);
            auto idx = [&](int32_t x, int32_t y, int32_t z) { return x + grid.nx * (y + grid.ny * z); };
            grid.occ[static_cast<size_t>(idx(1, 1, 1))] = 1;
            grid.occ[static_cast<size_t>(idx(2, 2, 1))] = 1;
            FractureMesh mesh;
            const bool built = BuildSurfaceNetsFromOccupancy(grid, 1.0f, mesh);
            check(built, "surface nets: diagonal (ambiguous) occupancy builds a mesh");
            if (built) {
                const ClosedMeshCheck c = CheckClosedMesh(mesh);
                check(c.closed && c.signedVolume > 0.0,
                      "surface nets: diagonal (ambiguous) occupancy resolves to a closed, outward mesh");
            }
        }

        // ---- BakeFracture の入口: openMeshMode=1 の解像度ごとの成否 (正しさの被覆だけ)。
        // 解像度 32/48/64 の全部を合否として検証する (sub-14、libtess2 への置き換えで
        // 解像度48/64のEarClip失敗は解消した)。res48/64 は pieceCount を 4 に落として
        // 薄い壁の輪郭の接触 (sub-14 の修正対象そのもの) を踏むことだけを確かめる —
        // 分割コアの計算量は解像度でなく pieceCount に強く効くため、被覆を落とさずに
        // Debug の所要時間を大きく削れる。焼き時間の表 (pieceCount 16 を含む) は
        // --fracture-bench (Release) へ移した (sub-11/sub-12)。時間は参考ログのみで
        // 合否には数えない
        {
            auto bakeOpenMesh = [&](const char* label, const FractureMesh& mesh, int32_t resolution,
                                    int32_t pieceCount) {
                FractureBakeInput in;
                in.sourceMesh = mesh;
                in.seed = 11;
                in.pieceCount = pieceCount;
                in.openMeshMode = 1;
                in.voxelResolution = resolution;
                FractureBakeResult r;
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = BakeFracture(in, r);
                const auto t1 = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                bool allValid = ok && r.success;
                if (allValid) {
                    for (const FracturePieceBake& p : r.pieces) {
                        if (!PieceGeometryValid(p)) {
                            allValid = false;
                        }
                    }
                }
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                              "bake entry: %s openMeshMode=1 pieceCount=%d res=%d succeeds and all pieces close (%.2f ms, pieces=%d)",
                              label, pieceCount, resolution, ms,
                              ok && r.success ? static_cast<int>(r.pieces.size()) : -1);
                check(allValid, buf);
                if (!allValid) {
                    MYE_LOG_ERROR("    reason: %s", r.failReason.c_str());
                }
            };
            bakeOpenMesh("open box", MakeOpenBox(1, 1, 1), 32, 16);
            bakeOpenMesh("plane quad", MakePlaneQuad(1.0f), 32, 16);
            bakeOpenMesh("open box", MakeOpenBox(1, 1, 1), 48, 4);
            bakeOpenMesh("plane quad", MakePlaneQuad(1.0f), 48, 4);
            bakeOpenMesh("open box", MakeOpenBox(1, 1, 1), 64, 4);
            bakeOpenMesh("plane quad", MakePlaneQuad(1.0f), 64, 4);
        }
    }

    // ---- 15. 破片エンティティの事前生成 (BuildFracturePieces、M80f) ----
    {
        // (15a) 子が破片数 x 2 (Frag<i> + _cap) でき、欄の値が正しい。
        // root 自身の Collider は外れ、Rigidbody(compoundColliders) が付く。
        // 2 回呼んでも子は倍にならない (再生成)
        {
            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("Root");
            root.SetLocalPosition(1.0f, 2.0f, 3.0f);

            auto* rootMr = root.AddComponent<MeshRendererComponent>();
            rootMr->mesh = AssetID{ HashStr("fracture-selftest://root_mesh") };
            rootMr->material = AssetID{ HashStr("fracture-selftest://root_mat") };
            const AssetID outerMaterialExpected = rootMr->material; // ポインタ寿命内に値だけ控える

            auto* d = root.AddComponent<DestructibleComponent>();
            d->innerMaterial = AssetID{ HashStr("fracture-selftest://inner_mat") };
            const AssetID innerMaterialExpected = d->innerMaterial;

            // 生成前に既存の Collider を持たせておく (外れることを確認する対象)
            auto* rootCol = root.AddComponent<ColliderComponent>();
            rootCol->shape = collidershape::kBox;

            FractureAssetHandle handle;
            handle.namePrefix = "fracture-selftest://build_pieces";
            for (int i = 0; i < 3; ++i) {
                char nameBuf[64];
                FracturePieceRef pr;
                pr.origin = { static_cast<float>(i) * 0.5f, 0.0f, 0.0f };
                pr.volume = 1.0;
                std::snprintf(nameBuf, sizeof(nameBuf), "fracture-selftest://outer%d", i);
                pr.outerMesh = AssetID{ HashStr(nameBuf) };
                std::snprintf(nameBuf, sizeof(nameBuf), "fracture-selftest://cap%d", i);
                pr.capMesh = AssetID{ HashStr(nameBuf) };
                std::snprintf(nameBuf, sizeof(nameBuf), "fracture-selftest://hull%d", i);
                pr.hull = AssetID{ HashStr(nameBuf) };
                handle.pieces.push_back(pr);
            }

            const int built = BuildFracturePieces(w, root.Id(), handle);
            check(built == 3, "BuildFracturePieces: returns the piece count");

            int fragCount = 0, capCount = 0;
            bool seenIndex[3] = { false, false, false };
            const auto* rootH = w.GetComponent<HierarchyComponent>(root.Id());
            for (EntityID c = rootH ? rootH->firstChild : kNullEntity; !c.IsNull();) {
                const auto* fp = w.GetComponent<FracturePieceComponent>(c);
                if (fp != nullptr) {
                    ++fragCount;
                    check(fp->root == root.Id(), "BuildFracturePieces: FracturePiece.root points at the root");
                    if (fp->index >= 0 && fp->index < 3) {
                        seenIndex[fp->index] = true;
                    }
                    const size_t idx = static_cast<size_t>(fp->index);
                    const auto* lt = w.GetComponent<LocalTransform>(c);
                    check(lt != nullptr && idx < handle.pieces.size()
                              && lt->position.x == handle.pieces[idx].origin.x
                              && lt->position.y == handle.pieces[idx].origin.y
                              && lt->position.z == handle.pieces[idx].origin.z,
                          "BuildFracturePieces: Frag<i> LocalTransform.position == piece.origin");
                    const auto* mr = w.GetComponent<MeshRendererComponent>(c);
                    check(mr != nullptr && mr->mesh == handle.pieces[idx].outerMesh
                              && mr->material == outerMaterialExpected,
                          "BuildFracturePieces: Frag<i> MeshRenderer uses the outer mesh and the root's material");
                    const auto* col = w.GetComponent<ColliderComponent>(c);
                    check(col != nullptr && col->shape == collidershape::kConvex
                              && col->meshAsset == handle.pieces[idx].hull,
                          "BuildFracturePieces: Frag<i> Collider is a convex hull pointing at #hull");

                    int childCount = 0;
                    const auto* fh = w.GetComponent<HierarchyComponent>(c);
                    for (EntityID cc = fh ? fh->firstChild : kNullEntity; !cc.IsNull();) {
                        ++childCount;
                        ++capCount;
                        const auto* capMr = w.GetComponent<MeshRendererComponent>(cc);
                        check(capMr != nullptr && capMr->mesh == handle.pieces[idx].capMesh
                                  && capMr->material == innerMaterialExpected,
                              "BuildFracturePieces: _cap MeshRenderer uses the cap mesh and innerMaterial");
                        const auto* cch = w.GetComponent<HierarchyComponent>(cc);
                        cc = cch ? cch->nextSibling : kNullEntity;
                    }
                    check(childCount == 1, "BuildFracturePieces: Frag<i> has exactly one _cap child");
                }
                const auto* ch = w.GetComponent<HierarchyComponent>(c);
                c = ch ? ch->nextSibling : kNullEntity;
            }
            check(fragCount == 3 && capCount == 3,
                  "BuildFracturePieces: 3 pieces produce 3 Frag + 3 _cap children");
            check(seenIndex[0] && seenIndex[1] && seenIndex[2],
                  "BuildFracturePieces: piece indices cover 0..N-1 with no duplicates");

            check(w.GetComponent<ColliderComponent>(root.Id()) == nullptr,
                  "BuildFracturePieces: the root's own Collider is removed");
            const auto* rb = w.GetComponent<RigidbodyComponent>(root.Id());
            check(rb != nullptr && rb->compoundColliders,
                  "BuildFracturePieces: the root gets a compound Rigidbody");

            // 2 回目: 子は倍にならない (再生成)
            const int builtAgain = BuildFracturePieces(w, root.Id(), handle);
            check(builtAgain == 3, "BuildFracturePieces: rebuilding still returns the piece count");
            int fragCount2 = 0;
            const auto* rootH2 = w.GetComponent<HierarchyComponent>(root.Id());
            for (EntityID c = rootH2 ? rootH2->firstChild : kNullEntity; !c.IsNull();) {
                if (w.GetComponent<FracturePieceComponent>(c) != nullptr) {
                    ++fragCount2;
                }
                const auto* ch = w.GetComponent<HierarchyComponent>(c);
                c = ch ? ch->nextSibling : kNullEntity;
            }
            check(fragCount2 == 3, "BuildFracturePieces: calling it twice does not double the children");
        }

        // (15b) 資産の破片数と子の index 集合が合わない Destructible は無効
        // (ValidateFracturePieces、spec §4.1 エッジケース)
        {
            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("Root2");

            FractureAssetHandle handle;
            handle.namePrefix = "fracture-selftest://validate";
            handle.pieces.assign(2, FracturePieceRef{});
            BuildFracturePieces(w, root.Id(), handle);
            check(ValidateFracturePieces(w, root.Id(), &handle),
                  "ValidateFracturePieces: a freshly built root matches its asset");

            // 焼き直しで破片が増えたと仮定すると、子の index 集合と合わなくなる
            FractureAssetHandle grown = handle;
            grown.pieces.push_back(FracturePieceRef{});
            check(!ValidateFracturePieces(w, root.Id(), &grown),
                  "ValidateFracturePieces: a piece-count mismatch is rejected");

            check(!ValidateFracturePieces(w, root.Id(), nullptr),
                  "ValidateFracturePieces: a missing (nullptr) asset is rejected without crashing");
        }

        // (15c) 動的ルートが 1 剛体として落ちて床で止まり、同じ形の凸包 2 個を手で複合にした
        // 物と同じ高さで静止する (= 重心・慣性が一致し、sub-05 の複合合成を通っている証拠)
        {
            ConvexColliderLibrary colliders;
            auto boxHull = [](float hx, float hy, float hz) {
                ConvexHullData h;
                BuildConvexHull({ { -hx, -hy, -hz }, { hx, -hy, -hz }, { hx, hy, -hz }, { -hx, hy, -hz },
                                  { -hx, -hy, hz }, { hx, -hy, hz }, { hx, hy, hz }, { -hx, hy, hz } },
                                h);
                return h;
            };
            const AssetID kHalfA{ HashStr("fracture-selftest://compound_half_a") };
            const AssetID kHalfB{ HashStr("fracture-selftest://compound_half_b") };
            colliders.Register(kHalfA, boxHull(0.5f, 0.5f, 0.5f));
            colliders.Register(kHalfB, boxHull(0.5f, 0.5f, 0.5f));
            convexcol::Install(&colliders);

            FractureAssetHandle handle;
            handle.namePrefix = "fracture-selftest://compound_settle";
            FracturePieceRef a;
            a.origin = { -0.5f, 0.0f, 0.0f };
            a.volume = 0.5;
            a.hull = kHalfA;
            FracturePieceRef b;
            b.origin = { 0.5f, 0.0f, 0.0f };
            b.volume = 0.5;
            b.hull = kHalfB;
            handle.pieces = { a, b };

            PhysicsSystem phys;
            constexpr float kDt = 1.0f / 60.0f;

            auto settle = [&](bool useFracturePieces) {
                Scene s;
                World& w = s.GetWorld();
                GameObject ground = s.CreateGameObject("Ground");
                ground.SetLocalPosition(0.0f, -0.5f, 0.0f);
                ground.SetLocalScale(10.0f, 1.0f, 10.0f);
                auto* gcol = ground.AddComponent<ColliderComponent>();
                gcol->shape = collidershape::kBox;
                gcol->halfExtents = { 0.5f, 0.5f, 0.5f };

                GameObject root = s.CreateGameObject("Compound");
                root.SetLocalPosition(0.0f, 3.0f, 0.0f);
                if (useFracturePieces) {
                    BuildFracturePieces(w, root.Id(), handle);
                } else {
                    auto* rb = root.AddComponent<RigidbodyComponent>();
                    rb->compoundColliders = true;
                    auto makeChild = [&](const char* name, AssetID hull, float x) {
                        GameObject child = s.CreateGameObject(name);
                        w.SetParent(child.Id(), root.Id());
                        w.ApplyStructuralChanges();
                        auto* col = child.AddComponent<ColliderComponent>();
                        col->shape = collidershape::kConvex;
                        col->meshAsset = hull;
                        if (auto* lt = child.GetComponent<LocalTransform>()) {
                            lt->position = { x, 0.0f, 0.0f };
                        }
                    };
                    makeChild("HalfA", kHalfA, -0.5f);
                    makeChild("HalfB", kHalfB, 0.5f);
                }
                w.ApplyStructuralChanges();
                if (auto* rb = w.GetComponent<RigidbodyComponent>(root.Id())) {
                    rb->mass = 4.0f;
                }
                for (int i = 0; i < 180; ++i) {
                    phys.Update(w, kDt);
                }
                return w.GetComponent<LocalTransform>(root.Id())->position.y;
            };

            const float yBuilt = settle(true);
            const float yManual = settle(false);
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "BuildFracturePieces: settles at the same height as a manually authored "
                          "compound of the same hulls (built=%.5f, manual=%.5f)",
                          static_cast<double>(yBuilt), static_cast<double>(yManual));
            check(std::fabs(yBuilt - yManual) < 1e-4f, buf);
            check(yBuilt > 0.45f && yBuilt < 0.55f,
                  "BuildFracturePieces: the compound rests at half its own height (~0.5)");

            convexcol::Install(nullptr);
        }
    }

    // ---- 16. 接着の破断と塊の剛体化 (FractureSystem、M80g) ----
    {
        // (16a) 荷重が閾値を超えると i<j の接着が両側とも切れ、体積最大側 (root) と
        // 孤立した破片 (新リーダー) へ分かれる。質量・運動量が保存される。
        // shapeImpulses を直接組み立てて FractureSystem::Update だけを検算する
        // (物理の形状単位インパルスそのものは受け入れ条件 8 で別途検算済み)
        {
            constexpr int32_t kRowCount = 8;
            FractureBakeResult rowBake = MakeRowFractureBake(kRowCount, 0.25f);

            RenderResources resources;
            ConvexColliderLibrary colliders;
            colliders.Init(&resources);
            FractureLibrary lib;
            lib.Init(&resources, &colliders);
            fracturelib::Install(&lib);
            const FractureAssetHandle* handle = lib.RegisterBaked(
                "fracture-selftest://row8", rowBake, HashStr("fracture-selftest://row8_src"), 0, kRowCount, 0, 0);
            check(handle != nullptr && handle->pieces.size() == static_cast<size_t>(kRowCount),
                  "fracture system: row8 synthetic asset registers");

            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("Row8");
            // ★AddComponent はアーキタイプ移動を起こし、以前に取ったポインタを無効化する
            //   (spec 4.5)。両方足してから GetComponent で取り直す (Add の戻り値をまたいで
            //   使い回さない)
            root.AddComponent<RigidbodyComponent>();
            root.AddComponent<DestructibleComponent>();
            auto* rootRb = root.GetComponent<RigidbodyComponent>();
            rootRb->mass = 8.0f;
            rootRb->velocity = { 2.0f, 0.0f, 0.0f };
            rootRb->angularVelocity = { 0.0f, 0.0f, 3.0f };
            auto* d = root.GetComponent<DestructibleComponent>();
            d->strength = 100.0f; // 低めにして単純な形状単位インパルスで確実に切れさせる
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://row8") };
            BuildFracturePieces(w, root.Id(), *handle);
            w.ApplyStructuralChanges();
            // root 自身のコンポーネント構成は BuildFracturePieces 後も変わらない
            // (既に Rigidbody 所持・Collider 無し) ので、rootRb/d はここから先も有効
            check(ValidateFracturePieces(w, root.Id(), handle),
                  "fracture system: row8 root matches its (synthetic) asset");

            const EntityID piece0 = FindPieceChild(w, root.Id(), 0);
            check(!piece0.IsNull(), "fracture system: row8 piece 0 exists");

            const double totalMassBefore = static_cast<double>(rootRb->mass);
            const XMFLOAT3 vBefore = rootRb->velocity;
            const double px0 = totalMassBefore * vBefore.x;
            const double py0 = totalMassBefore * vBefore.y;
            const double pz0 = totalMassBefore * vBefore.z;

            // 弱い衝撃では割れない
            {
                std::vector<ShapeImpulse> weak = { { piece0, 0.01f } };
                FractureSystem fsys;
                fsys.Update(w, 1.0f / 60.0f, weak);
                w.ApplyStructuralChanges();
                check(!d->broken, "fracture system: a weak impact does not break any bond");
            }

            // 十分な衝撃では piece0-piece1 の接着が切れる
            std::vector<ShapeImpulse> strong = { { piece0, 1000.0f } };
            FractureSystem fsys;
            fsys.Update(w, 1.0f / 60.0f, strong);
            w.ApplyStructuralChanges();
            check(d->broken && d->detachedCount == 1,
                  "fracture system: a strong impact breaks the bond and detaches exactly one chunk");

            auto* leaderRb = w.GetComponent<RigidbodyComponent>(piece0);
            check(leaderRb != nullptr, "fracture system: the separated piece gets its own Rigidbody");
            if (leaderRb != nullptr) {
                check(!leaderRb->isKinematic, "fracture system: a separated chunk is dynamic");
                check(!leaderRb->compoundColliders,
                      "fracture system: a lone separated piece is not a compound (1 member)");
                check(w.GetParent(piece0) == w.GetParent(root.Id()),
                      "fracture system: the new leader is reparented to the root's parent");
            }

            int32_t remainingChildren = 0;
            const auto* rh2 = w.GetComponent<HierarchyComponent>(root.Id());
            for (EntityID c = rh2 ? rh2->firstChild : kNullEntity; !c.IsNull();) {
                if (w.GetComponent<FracturePieceComponent>(c) != nullptr) {
                    ++remainingChildren;
                }
                const auto* ch = w.GetComponent<HierarchyComponent>(c);
                c = ch ? ch->nextSibling : kNullEntity;
            }
            check(remainingChildren == kRowCount - 1,
                  "fracture system: the largest component (7 pieces) stays under the root");

            const double massAfter = static_cast<double>(rootRb->mass)
                                    + (leaderRb ? static_cast<double>(leaderRb->mass) : 0.0);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "fracture system: total mass is conserved (%.8f vs %.8f)", massAfter,
                          totalMassBefore);
            check(Near(massAfter, totalMassBefore, 1e-6), buf);

            double pxAfter = static_cast<double>(rootRb->mass) * rootRb->velocity.x;
            double pyAfter = static_cast<double>(rootRb->mass) * rootRb->velocity.y;
            double pzAfter = static_cast<double>(rootRb->mass) * rootRb->velocity.z;
            if (leaderRb != nullptr) {
                pxAfter += static_cast<double>(leaderRb->mass) * leaderRb->velocity.x;
                pyAfter += static_cast<double>(leaderRb->mass) * leaderRb->velocity.y;
                pzAfter += static_cast<double>(leaderRb->mass) * leaderRb->velocity.z;
            }
            std::snprintf(buf, sizeof(buf),
                          "fracture system: linear momentum is conserved (%.6f,%.6f,%.6f vs %.6f,%.6f,%.6f)",
                          pxAfter, pyAfter, pzAfter, px0, py0, pz0);
            check(Near(pxAfter, px0, 1e-5) && Near(pyAfter, py0, 1e-5) && Near(pzAfter, pz0, 1e-5), buf);

            fracturelib::Install(nullptr);
        }

        // (16b) 統合: 実際の物理衝突で「十分な速さの球」だけが箱 8 破片を割る
        {
            FractureBakeInput in;
            in.sourceMesh = MakeBox(0.5f, 0.5f, 0.5f);
            in.seed = 300;
            in.pieceCount = 8;
            FractureBakeResult bake;
            const bool baked = BakeFracture(in, bake);
            check(baked && bake.success, "fracture system: box8 bake for the impact test succeeds");
            if (baked && bake.success) {
                auto runImpact = [&](float ballMass, float ballSpeed) {
                    RenderResources resources;
                    ConvexColliderLibrary colliders;
                    colliders.Init(&resources);
                    FractureLibrary lib;
                    lib.Init(&resources, &colliders);
                    convexcol::Install(&colliders);
                    fracturelib::Install(&lib);
                    const FractureAssetHandle* handle = lib.RegisterBaked(
                        "fracture-selftest://box8_impact", bake, HashStr("fracture-selftest://box8_impact_src"),
                        in.seed, in.pieceCount, 0, 32);

                    Scene s;
                    World& w = s.GetWorld();
                    GameObject root = s.CreateGameObject("Box8");
                    root.AddComponent<RigidbodyComponent>();
                    root.AddComponent<DestructibleComponent>();
                    auto* rb = root.GetComponent<RigidbodyComponent>();
                    rb->mass = 8.0f;
                    rb->gravityScale = 0.0f; // 衝突の伝わり方だけを見る (落下と混ぜない)
                    auto* d = root.GetComponent<DestructibleComponent>();
                    // 実物理のインパルスは複数破片に分かれて配られ得るので、既定値 (5000) では
                    // なく低めの値で「十分な速さの球なら確実に切れる」ことを検算する
                    d->strength = 200.0f;
                    d->fractureAsset = AssetID{ HashStr("fracture-selftest://box8_impact") };
                    BuildFracturePieces(w, root.Id(), *handle);

                    GameObject ball = s.CreateGameObject("Ball");
                    ball.SetLocalPosition(-2.0f, 0.0f, 0.0f);
                    ball.AddComponent<ColliderComponent>();
                    ball.AddComponent<RigidbodyComponent>();
                    auto* bcol = ball.GetComponent<ColliderComponent>();
                    bcol->shape = collidershape::kSphere;
                    bcol->radius = 0.3f;
                    auto* brb = ball.GetComponent<RigidbodyComponent>();
                    brb->mass = ballMass;
                    brb->gravityScale = 0.0f;
                    brb->velocity = { ballSpeed, 0.0f, 0.0f };
                    w.ApplyStructuralChanges();

                    PhysicsSystem phys;
                    FractureSystem fsys;
                    std::vector<ShapeImpulse> impulses;
                    constexpr float kDt = 1.0f / 60.0f;
                    for (int i = 0; i < 120; ++i) {
                        phys.Update(w, kDt, nullptr, nullptr, &impulses);
                        fsys.Update(w, kDt, impulses);
                        w.ApplyStructuralChanges();
                    }
                    const bool broken = d->broken;
                    fracturelib::Install(nullptr);
                    convexcol::Install(nullptr);
                    return broken;
                };

                check(!runImpact(0.3f, 1.0f), "fracture system: a weak ball does not break box8");
                // ★速すぎる球は 1 tick の移動量が箱の寸法を超えてすり抜けかねない (CCD 未使用) ので、
                //   質量を稼いで運動量を確保しつつ、tick あたりの移動量は箱の半分以下に抑える
                check(runImpact(50.0f, 10.0f), "fracture system: a fast ball breaks box8");
            }
        }

        // (16c) kinematic ルートの壁 (破片 16): 撃った所だけ抜け、体積最大の塊が固定のまま
        // 残る (ルートの姿勢が不変)
        {
            constexpr int32_t kWallCount = 16;
            FractureBakeResult wallBake = MakeRowFractureBake(kWallCount, 0.25f);

            RenderResources resources;
            ConvexColliderLibrary colliders;
            colliders.Init(&resources);
            FractureLibrary lib;
            lib.Init(&resources, &colliders);
            fracturelib::Install(&lib);
            const FractureAssetHandle* handle = lib.RegisterBaked(
                "fracture-selftest://wall16", wallBake, HashStr("fracture-selftest://wall16_src"), 0, kWallCount, 0,
                0);
            check(handle != nullptr, "fracture system: wall16 synthetic asset registers");

            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("Wall16");
            root.SetLocalPosition(5.0f, 1.0f, -2.0f);
            root.SetLocalRotationEuler(0.0f, 30.0f, 0.0f); // 単位でない姿勢にして「不変」を厳密に見る
            root.AddComponent<RigidbodyComponent>();
            root.AddComponent<DestructibleComponent>();
            auto* rootRb = root.GetComponent<RigidbodyComponent>();
            rootRb->isKinematic = true;
            auto* d = root.GetComponent<DestructibleComponent>();
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://wall16") };
            BuildFracturePieces(w, root.Id(), *handle);
            w.ApplyStructuralChanges();

            const XMFLOAT3 posBefore = w.GetComponent<LocalTransform>(root.Id())->position;
            const XMFLOAT4 rotBefore = w.GetComponent<LocalTransform>(root.Id())->rotation;
            const EntityID piece0 = FindPieceChild(w, root.Id(), 0);

            std::vector<ShapeImpulse> hit = { { piece0, 100000.0f } };
            FractureSystem fsys;
            fsys.Update(w, 1.0f / 60.0f, hit);
            w.ApplyStructuralChanges();

            check(d->broken && d->detachedCount == 1,
                  "fracture system: hitting one end of a kinematic wall detaches it");

            const auto* ltAfter = w.GetComponent<LocalTransform>(root.Id());
            check(ltAfter != nullptr && ltAfter->position.x == posBefore.x && ltAfter->position.y == posBefore.y
                      && ltAfter->position.z == posBefore.z && ltAfter->rotation.x == rotBefore.x
                      && ltAfter->rotation.y == rotBefore.y && ltAfter->rotation.z == rotBefore.z
                      && ltAfter->rotation.w == rotBefore.w,
                  "fracture system: the kinematic root's own transform is unchanged");
            check(rootRb->isKinematic, "fracture system: the kinematic root stays kinematic");

            auto* leaderRb = w.GetComponent<RigidbodyComponent>(piece0);
            check(leaderRb != nullptr && !leaderRb->isKinematic,
                  "fracture system: the detached piece becomes dynamic even though the root is kinematic");

            fracturelib::Install(nullptr);
        }

        // (16d) 決定論: 同じシーンを 2 本並べて 240 tick のハッシュ列が一致する
        // (PhysicsSelfTest の並走比較と同じ形)
        {
            FractureBakeInput in;
            in.sourceMesh = MakeBox(0.5f, 0.5f, 0.5f);
            in.seed = 301;
            in.pieceCount = 8;
            FractureBakeResult bake;
            const bool baked = BakeFracture(in, bake);
            check(baked && bake.success, "fracture system: box8 bake for the determinism test succeeds");
            if (baked && bake.success) {
                auto runAndHash = [&](std::vector<uint64_t>& outHashes) {
                    RenderResources resources;
                    ConvexColliderLibrary colliders;
                    colliders.Init(&resources);
                    FractureLibrary lib;
                    lib.Init(&resources, &colliders);
                    convexcol::Install(&colliders);
                    fracturelib::Install(&lib);
                    const FractureAssetHandle* handle = lib.RegisterBaked(
                        "fracture-selftest://box8_determinism", bake,
                        HashStr("fracture-selftest://box8_determinism_src"), in.seed, in.pieceCount, 0, 32);

                    Scene s;
                    World& w = s.GetWorld();
                    GameObject root = s.CreateGameObject("Box8");
                    root.AddComponent<RigidbodyComponent>();
                    root.AddComponent<DestructibleComponent>();
                    auto* rb = root.GetComponent<RigidbodyComponent>();
                    rb->mass = 8.0f;
                    rb->gravityScale = 0.0f;
                    auto* d = root.GetComponent<DestructibleComponent>();
                    d->strength = 200.0f; // 確実に割れさせ、分離・付け替え経路もハッシュ被覆に含める
                    d->fractureAsset = AssetID{ HashStr("fracture-selftest://box8_determinism") };
                    BuildFracturePieces(w, root.Id(), *handle);

                    GameObject ball = s.CreateGameObject("Ball");
                    ball.SetLocalPosition(-2.0f, 0.0f, 0.0f);
                    ball.AddComponent<ColliderComponent>();
                    ball.AddComponent<RigidbodyComponent>();
                    auto* bcol = ball.GetComponent<ColliderComponent>();
                    bcol->shape = collidershape::kSphere;
                    bcol->radius = 0.3f;
                    auto* brb = ball.GetComponent<RigidbodyComponent>();
                    brb->mass = 50.0f;
                    brb->gravityScale = 0.0f;
                    brb->velocity = { 10.0f, 0.0f, 0.0f }; // CCD 未使用のためすり抜けない速さに抑える
                    w.ApplyStructuralChanges();

                    PhysicsSystem phys;
                    FractureSystem fsys;
                    std::vector<ShapeImpulse> impulses;
                    constexpr float kDt = 1.0f / 60.0f;
                    outHashes.clear();
                    for (int i = 0; i < 240; ++i) {
                        phys.Update(w, kDt, nullptr, nullptr, &impulses);
                        fsys.Update(w, kDt, impulses);
                        w.ApplyStructuralChanges();
                        outHashes.push_back(HashWorld(w));
                    }
                    fracturelib::Install(nullptr);
                    convexcol::Install(nullptr);
                };

                std::vector<uint64_t> hashesA, hashesB;
                runAndHash(hashesA);
                runAndHash(hashesB);
                check(hashesA.size() == 240 && hashesB.size() == 240,
                      "fracture system: determinism run produced 240 ticks");
                bool allMatch = hashesA.size() == hashesB.size();
                for (size_t i = 0; allMatch && i < hashesA.size(); ++i) {
                    if (hashesA[i] != hashesB[i]) {
                        allMatch = false;
                        MYE_LOG_ERROR("    (determinism mismatch at tick %zu: 0x%016llX vs 0x%016llX)", i,
                                     static_cast<unsigned long long>(hashesA[i]),
                                     static_cast<unsigned long long>(hashesB[i]));
                    }
                }
                check(allMatch,
                      "fracture system: two independent runs produce byte-identical hash sequences over 240 ticks");
            }
        }

        // (16e) ApplyFractureDamage: strength 以上を 1 回、または未満を 2 回の蓄積で外れる
        {
            FractureBakeResult pairBake = MakeRowFractureBake(2, 0.25f);

            auto makeDamageScene = [&](Scene& s, FractureLibrary& lib, ConvexColliderLibrary& colliders,
                                       RenderResources& resources, const char* prefix) {
                colliders.Init(&resources);
                lib.Init(&resources, &colliders);
                fracturelib::Install(&lib);
                const FractureAssetHandle* handle
                    = lib.RegisterBaked(prefix, pairBake, HashStr(std::string(prefix) + "_src"), 0, 2, 0, 0);
                World& w = s.GetWorld();
                GameObject root = s.CreateGameObject("Pair");
                root.AddComponent<RigidbodyComponent>();
                root.AddComponent<DestructibleComponent>();
                auto* rb = root.GetComponent<RigidbodyComponent>();
                rb->mass = 2.0f;
                auto* d = root.GetComponent<DestructibleComponent>();
                d->strength = 100.0f;
                d->fractureAsset = AssetID{ HashStr(prefix) };
                BuildFracturePieces(w, root.Id(), *handle);
                w.ApplyStructuralChanges();
                return root.Id();
            };

            // (e1) strength 以上を 1 回で外れる
            {
                Scene s;
                RenderResources resources;
                ConvexColliderLibrary colliders;
                FractureLibrary lib;
                const EntityID root = makeDamageScene(s, lib, colliders, resources, "fracture-selftest://pair_damage1");
                World& w = s.GetWorld();
                const EntityID piece0 = FindPieceChild(w, root, 0);
                ApplyFractureDamage(w, root, pairBake.pieces[0].origin, 0.0f, 150.0f);
                FractureSystem fsys;
                std::vector<ShapeImpulse> none;
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
                const auto* d = w.GetComponent<DestructibleComponent>(root);
                check(d != nullptr && d->broken,
                      "fracture system: ApplyFractureDamage >= strength in one shot detaches the piece");
                (void)piece0;
                fracturelib::Install(nullptr);
            }

            // (e2) strength 未満を 2 回の蓄積で外れる
            {
                Scene s;
                RenderResources resources;
                ConvexColliderLibrary colliders;
                FractureLibrary lib;
                const EntityID root = makeDamageScene(s, lib, colliders, resources, "fracture-selftest://pair_damage2");
                World& w = s.GetWorld();
                std::vector<ShapeImpulse> none;

                ApplyFractureDamage(w, root, pairBake.pieces[0].origin, 0.0f, 60.0f);
                FractureSystem fsys1;
                fsys1.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
                const auto* d1 = w.GetComponent<DestructibleComponent>(root);
                check(d1 != nullptr && !d1->broken,
                      "fracture system: a single sub-threshold ApplyFractureDamage does not break it yet");

                ApplyFractureDamage(w, root, pairBake.pieces[0].origin, 0.0f, 60.0f); // 累計 120 >= 100
                FractureSystem fsys2;
                fsys2.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
                const auto* d2 = w.GetComponent<DestructibleComponent>(root);
                check(d2 != nullptr && d2->broken,
                      "fracture system: accumulated ApplyFractureDamage across two calls detaches it");

                fracturelib::Install(nullptr);
            }
        }

        // (16f) root proxy の可視規則 (RenderSystem::CollectDrawables と共有): root の
        // Destructible が見つからない (Destroy された/参照切れ) ときは「隠す対象が無い」ので
        // 描く側にする。逆にすると、分離後にルートを Destroy しただけで分かれた破片まで
        // 描画から消える不具合になる
        {
            DestructibleComponent notBroken;
            notBroken.broken = false;
            DestructibleComponent broken;
            broken.broken = true;
            check(ShouldHideUnbrokenFracturePiece(&notBroken),
                  "fracture system: root proxy hides an unbroken piece while its root exists");
            check(!ShouldHideUnbrokenFracturePiece(&broken),
                  "fracture system: root proxy draws a piece once its root has broken");
            check(!ShouldHideUnbrokenFracturePiece(nullptr),
                  "fracture system: root proxy draws a piece whose root Destructible cannot be found "
                  "(root Destroy 後も破片が描画から消えない)");
        }

        // (16g、受け入れ条件 6b) 割れた後の状態から、新しい FractureSystem のインスタンス
        // (キャッシュが空、新しいプロセスでのセーブ読み込みを模す) で再開しても、同じ状態から
        // 途切れずに進めた実行とハッシュ列が一致することを確認する
        {
            constexpr int32_t kCount = 8;
            FractureBakeResult rowBake = MakeRowFractureBake(kCount, 0.25f);

            auto buildScene = [&](Scene& s, FractureLibrary& lib, ConvexColliderLibrary& colliders,
                                  RenderResources& resources, const char* prefix) {
                colliders.Init(&resources);
                lib.Init(&resources, &colliders);
                fracturelib::Install(&lib);
                const FractureAssetHandle* handle
                    = lib.RegisterBaked(prefix, rowBake, HashStr(std::string(prefix) + "_src"), 0, kCount, 0, 0);
                World& w = s.GetWorld();
                GameObject root = s.CreateGameObject("Row8_6b");
                root.AddComponent<RigidbodyComponent>();
                root.AddComponent<DestructibleComponent>();
                auto* rb = root.GetComponent<RigidbodyComponent>();
                rb->mass = 8.0f;
                auto* d = root.GetComponent<DestructibleComponent>();
                d->strength = 100.0f;
                d->fractureAsset = AssetID{ HashStr(prefix) };
                BuildFracturePieces(w, root.Id(), *handle);
                w.ApplyStructuralChanges();
                return root.Id();
            };

            RenderResources resourcesA;
            ConvexColliderLibrary collidersA;
            FractureLibrary libA;
            Scene sceneA;
            const EntityID rootA
                = buildScene(sceneA, libA, collidersA, resourcesA, "fracture-selftest://snap6b");
            World& wA = sceneA.GetWorld();
            FractureSystem fsysA;
            std::vector<ShapeImpulse> none;

            // 1 回目の分離 (piece0 がリーダーへ昇格し、ルートの親の下 = ルート階層の外へ出る)
            ApplyFractureDamage(wA, rootA, rowBake.pieces[0].origin, 0.0f, 150.0f);
            fsysA.Update(wA, 1.0f / 60.0f, none);
            wA.ApplyStructuralChanges();
            {
                const auto* dA = wA.GetComponent<DestructibleComponent>(rootA);
                check(dA != nullptr && dA->broken && dA->detachedCount == 1,
                      "fracture system (6b): the first ApplyFractureDamage detaches piece 0");
            }

            // ---- ここでスナップショットを撮る (割れた直後、= 検証が必ず「今の状態」を見る
            // 局面に FractureSystem を初めて出会わせる) ----
            std::vector<std::byte> blob;
            const SimRefs refsA{ &sceneA };
            const bool captured = CaptureSimSnapshot(refsA, blob);
            check(captured, "fracture system (6b): snapshot capture succeeds right after the first break");

            Scene sceneB; // 空のシーン = 新しいプロセスでの読み込みを模す
            const SimRefs refsB{ &sceneB };
            const bool restored = captured && RestoreSimSnapshot(refsB, blob.data(), blob.size());
            check(restored, "fracture system (6b): snapshot restore succeeds");
            World& wB = sceneB.GetWorld();
            const EntityID rootB = rootA; // SnapshotRead は index/generation をそのまま復元する
            FractureSystem fsysB;         // ★キャッシュが空の「新しいインスタンス」

            if (restored) {
                // 継続実行 (A) と復元後の実行 (B) の両方へ、同じ以後の刺激 (残り 7 個の 1 つへ
                // 2 回目のダメージ) を与えて N tick 進め、ハッシュ列を比較する
                std::vector<uint64_t> hashesA, hashesB;
                constexpr int kN = 30;
                for (int t = 0; t < kN; ++t) {
                    if (t == 0) {
                        ApplyFractureDamage(wA, rootA, rowBake.pieces[7].origin, 0.0f, 150.0f);
                        ApplyFractureDamage(wB, rootB, rowBake.pieces[7].origin, 0.0f, 150.0f);
                    }
                    fsysA.Update(wA, 1.0f / 60.0f, none);
                    wA.ApplyStructuralChanges();
                    hashesA.push_back(HashWorld(wA));

                    fsysB.Update(wB, 1.0f / 60.0f, none);
                    wB.ApplyStructuralChanges();
                    hashesB.push_back(HashWorld(wB));
                }

                const auto* dAfterA = wA.GetComponent<DestructibleComponent>(rootA);
                const auto* dAfterB = wB.GetComponent<DestructibleComponent>(rootB);
                check(dAfterA != nullptr && dAfterA->detachedCount == 2, "fracture system (6b): the "
                      "continuous run detaches a second chunk after the snapshot point");
                check(dAfterB != nullptr && dAfterB->detachedCount == 2, "fracture system (6b): the "
                      "restored run (fresh FractureSystem) also detaches a second chunk");

                bool allMatch = hashesA.size() == hashesB.size();
                for (size_t i = 0; allMatch && i < hashesA.size(); ++i) {
                    if (hashesA[i] != hashesB[i]) {
                        allMatch = false;
                        MYE_LOG_ERROR("    (6b mismatch at tick %zu: continuous=0x%016llX restored=0x%016llX)",
                                     i, static_cast<unsigned long long>(hashesA[i]),
                                     static_cast<unsigned long long>(hashesB[i]));
                    }
                }
                check(allMatch, "fracture system (6b): resuming from a post-break snapshot with a fresh "
                      "FractureSystem instance matches an uninterrupted run byte-for-byte");
            }
            fracturelib::Install(nullptr);
        }

        // (16h、受け入れ条件 6b) 同じ検査を「割れる前」のスナップショットでも行う (broken==false
        // の分岐、個数一致の検査そのものが復元後も壊れていないことを確認する)
        {
            constexpr int32_t kCount = 8;
            FractureBakeResult rowBake = MakeRowFractureBake(kCount, 0.25f);

            RenderResources resourcesA;
            ConvexColliderLibrary collidersA;
            FractureLibrary libA;
            collidersA.Init(&resourcesA);
            libA.Init(&resourcesA, &collidersA);
            fracturelib::Install(&libA);
            const FractureAssetHandle* handle = libA.RegisterBaked(
                "fracture-selftest://snap6b_prebreak", rowBake, HashStr("fracture-selftest://snap6b_prebreak_src"),
                0, kCount, 0, 0);

            Scene sceneA;
            World& wA = sceneA.GetWorld();
            GameObject root = sceneA.CreateGameObject("Row8_6b_pre");
            root.AddComponent<RigidbodyComponent>();
            root.AddComponent<DestructibleComponent>();
            auto* rb = root.GetComponent<RigidbodyComponent>();
            rb->mass = 8.0f;
            auto* d = root.GetComponent<DestructibleComponent>();
            d->strength = 100.0f;
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://snap6b_prebreak") };
            BuildFracturePieces(wA, root.Id(), *handle);
            wA.ApplyStructuralChanges();
            const EntityID rootA = root.Id();

            FractureSystem fsysA;
            std::vector<ShapeImpulse> none;
            fsysA.Update(wA, 1.0f / 60.0f, none); // まだ何も割れない tick を 1 回通す

            std::vector<std::byte> blob;
            const SimRefs refsA{ &sceneA };
            const bool captured = CaptureSimSnapshot(refsA, blob);
            check(captured, "fracture system (6b, pre-break): snapshot capture succeeds before any break");

            Scene sceneB;
            const SimRefs refsB{ &sceneB };
            const bool restored = captured && RestoreSimSnapshot(refsB, blob.data(), blob.size());
            check(restored, "fracture system (6b, pre-break): snapshot restore succeeds");
            World& wB = sceneB.GetWorld();
            const EntityID rootB = rootA;
            FractureSystem fsysB;

            if (restored) {
                std::vector<uint64_t> hashesA, hashesB;
                constexpr int kN = 20;
                for (int t = 0; t < kN; ++t) {
                    if (t == 0) {
                        ApplyFractureDamage(wA, rootA, rowBake.pieces[0].origin, 0.0f, 150.0f);
                        ApplyFractureDamage(wB, rootB, rowBake.pieces[0].origin, 0.0f, 150.0f);
                    }
                    fsysA.Update(wA, 1.0f / 60.0f, none);
                    wA.ApplyStructuralChanges();
                    hashesA.push_back(HashWorld(wA));
                    fsysB.Update(wB, 1.0f / 60.0f, none);
                    wB.ApplyStructuralChanges();
                    hashesB.push_back(HashWorld(wB));
                }
                const auto* dAfterA = wA.GetComponent<DestructibleComponent>(rootA);
                const auto* dAfterB = wB.GetComponent<DestructibleComponent>(rootB);
                check(dAfterA != nullptr && dAfterA->broken && dAfterB != nullptr && dAfterB->broken,
                      "fracture system (6b, pre-break): both runs break after the snapshot point");
                bool allMatch = hashesA.size() == hashesB.size();
                for (size_t i = 0; allMatch && i < hashesA.size(); ++i) {
                    if (hashesA[i] != hashesB[i]) {
                        allMatch = false;
                    }
                }
                check(allMatch, "fracture system (6b, pre-break): resuming from a pre-break snapshot with a "
                      "fresh FractureSystem instance matches an uninterrupted run byte-for-byte");
            }
            fracturelib::Install(nullptr);
        }

        // (16h、M80l) ScriptAPI.h の GetBreakFn<T>: GameLogic.dll を経由せず、
        // マクロ展開だけを検査する (SchemaSelfTest.cpp の「GameLogic.dll をロードせずに
        // マクロの展開を検査する」流儀と同じ)。MakeDesc<T> は Registrar を経由しないので
        // グローバルなスクリプト表は汚さない
        {
            struct BreakProbeScript : Script<BreakProbeScript> {
                int32_t brokenCount = 0;
                MyeEntityId lastPiece{};
                MyeVec3 lastPoint{};
                float lastImpulse = 0.0f;
                void OnBreak(MyeUpdateContext&, MyeEntityId piece, MyeVec3 point, float impulse)
                {
                    ++brokenCount;
                    lastPiece = piece;
                    lastPoint = point;
                    lastImpulse = impulse;
                }
            };
            const MyeScriptDesc desc = mye_script_detail::MakeDesc<BreakProbeScript>(
                "BreakProbeScript", nullptr, 0);
            check(desc.onBreak != nullptr,
                  "ScriptAPI: GetBreakFn<T> detects OnBreak and wires MyeScriptDesc::onBreak");
            if (desc.onBreak != nullptr) {
                BreakProbeScript state;
                MyeUpdateContext ctx;
                const MyeEntityId piece{ 7u, 1u };
                const MyeVec3 point{ 1.0f, 2.0f, 3.0f };
                desc.onBreak(&state, &ctx, piece, point, 42.0f);
                check(state.brokenCount == 1 && state.lastPiece.index == 7u
                          && state.lastImpulse == 42.0f && state.lastPoint.x == 1.0f
                          && state.lastPoint.y == 2.0f && state.lastPoint.z == 3.0f,
                      "ScriptAPI: onBreak forwards (piece, point, impulse) to T::OnBreak unchanged");
            }
        }

        // (16i、M80l) onBreak の通知内容: 中間の破片 (index 3) だけに強い衝撃を与えると
        // 両側の接着が同時に切れ、{0,1,2} と {3} の 2 つの新リーダーへ分かれる (残留は
        // {4..7} — 体積最大)。LastBreakEvents() は新リーダー index 昇順 (0 → 3) で、
        // 荷重を受けていない側 ({0,1,2}) は同値タイで index 最小 (piece0) の原点・荷重 0、
        // 直接衝撃を受けた側 ({3}) は piece3 の原点・荷重そのものを返す。
        // スクリプト (ScriptHost/ManagedHost) を経由しない配信内容そのものの検算
        // (DLL を介した配信経路自体は --fracture-demo + replay_verify が実行経路で確かめる)
        {
            constexpr int32_t kCount = 8;
            FractureBakeResult rowBake = MakeRowFractureBake(kCount, 0.25f);

            RenderResources resources;
            ConvexColliderLibrary colliders;
            colliders.Init(&resources);
            FractureLibrary lib;
            lib.Init(&resources, &colliders);
            fracturelib::Install(&lib);
            const FractureAssetHandle* handle = lib.RegisterBaked(
                "fracture-selftest://break_event_row8", rowBake,
                HashStr("fracture-selftest://break_event_row8_src"), 0, kCount, 0, 0);

            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("BreakEventRow8");
            root.AddComponent<RigidbodyComponent>();
            root.AddComponent<DestructibleComponent>();
            auto* rootRb = root.GetComponent<RigidbodyComponent>();
            rootRb->mass = 8.0f;
            auto* d = root.GetComponent<DestructibleComponent>();
            d->strength = 100.0f;
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://break_event_row8") };
            BuildFracturePieces(w, root.Id(), *handle);
            w.ApplyStructuralChanges();

            const EntityID piece0 = FindPieceChild(w, root.Id(), 0);
            const EntityID piece3 = FindPieceChild(w, root.Id(), 3);
            check(!piece0.IsNull() && !piece3.IsNull(), "onBreak: row8 pieces 0/3 exist");

            std::vector<ShapeImpulse> impulses = { { piece3, 1000.0f } };
            FractureSystem fsys;
            fsys.Update(w, 1.0f / 60.0f, impulses); // scripts/managed 省略 = 観測だけ
            w.ApplyStructuralChanges();

            check(d->broken && d->detachedCount == 2,
                  "onBreak: a strong impact on an interior piece detaches both sides at once");
            const auto& events = fsys.LastBreakEvents();
            check(events.size() == 2, "onBreak: LastBreakEvents records exactly 2 new leaders");
            if (events.size() == 2) {
                check(events[0].root == root.Id() && events[1].root == root.Id(),
                      "onBreak: both events carry this Destructible's root");
                check(events[0].leader == piece0,
                      "onBreak: event order is new-leader-index ascending (piece0 first)");
                check(events[1].leader == piece3,
                      "onBreak: event order is new-leader-index ascending (piece3 second)");
                check(events[0].impulse == 0.0f,
                      "onBreak: the untouched side reports zero load (no piece in {0,1,2} took an impulse)");
                const float expectedImpulse = 1000.0f / (1.0f / 60.0f);
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "onBreak: the directly-hit side reports its own load (%.1f, expected %.1f)",
                              events[1].impulse, expectedImpulse);
                check(std::fabs(events[1].impulse - expectedImpulse) < 1.0f, buf);
                check(std::fabs(events[1].point.x - rowBake.pieces[3].origin.x) < 1e-4f
                          && std::fabs(events[1].point.y - rowBake.pieces[3].origin.y) < 1e-4f
                          && std::fabs(events[1].point.z - rowBake.pieces[3].origin.z) < 1e-4f,
                      "onBreak: point is the world origin of the max-load piece (piece3 itself)");
            }
            fracturelib::Install(nullptr);
        }

        // (16g) 資産キャッシュのキーは fractureAsset の値であって root ではない: 同じ
        // FractureSystem インスタンスのまま fractureAsset を差し替えても、古い資産のハンドルを
        // 使い回さない (Play 中に Inspector で資産を差し替える操作に相当)
        {
            const FractureBakeResult bakeA = MakeRowFractureBake(4, 0.25f);
            const FractureBakeResult bakeB = MakeRowFractureBake(8, 0.25f);

            RenderResources resources;
            ConvexColliderLibrary colliders;
            colliders.Init(&resources);
            FractureLibrary lib;
            lib.Init(&resources, &colliders);
            fracturelib::Install(&lib);
            const FractureAssetHandle* handleA = lib.RegisterBaked(
                "fracture-selftest://cache_a", bakeA, HashStr("fracture-selftest://cache_a_src"), 0, 4, 0, 0);
            const FractureAssetHandle* handleB = lib.RegisterBaked(
                "fracture-selftest://cache_b", bakeB, HashStr("fracture-selftest://cache_b_src"), 0, 8, 0, 0);
            check(handleA != nullptr && handleB != nullptr, "asset cache: both synthetic assets register");

            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("CacheSwapRoot");
            root.AddComponent<RigidbodyComponent>();
            root.AddComponent<DestructibleComponent>();
            auto* rootRb = root.GetComponent<RigidbodyComponent>();
            rootRb->gravityScale = 0.0f;
            auto* d = root.GetComponent<DestructibleComponent>();
            d->strength = 100.0f;
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://cache_a") };
            BuildFracturePieces(w, root.Id(), *handleA);
            w.ApplyStructuralChanges();

            FractureSystem fsys; // 1 個の長生き FractureSystem (EngineLoop が持つのと同じ運用)
            fsys.Update(w, 1.0f / 60.0f, {}); // 資産 A をこの root でキャッシュさせる
            w.ApplyStructuralChanges();

            // Play 中に Inspector で資産を差し替えたのと同じ操作: 資産 B へ切り替えて組み直す
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://cache_b") };
            BuildFracturePieces(w, root.Id(), *handleB);
            w.ApplyStructuralChanges();

            const EntityID piece7 = FindPieceChild(w, root.Id(), 7); // 資産 B にしか無い index
            check(!piece7.IsNull(), "asset cache: piece 7 exists after switching to the 8-piece asset");
            ApplyFractureDamage(w, piece7, { 0, 0, 0 }, 0.0f, 1.0e6f);
            for (int i = 0; i < 3; ++i) {
                fsys.Update(w, 1.0f / 60.0f, {});
                w.ApplyStructuralChanges();
            }
            check(d->broken,
                  "asset cache: the same FractureSystem instance re-resolves after fractureAsset "
                  "changes (a root-keyed cache would reject piece 7 as out of range)");
            fracturelib::Install(nullptr);
        }
    }

    // ---- 17. 割れた後の後始末 6 種 (FractureSystem、M80h) ----
    {
        // (17a) afterBreak=0 (残す): 何 tick 進めても消えない。releaseTicks は進み続ける
        {
            RenderResources resources;
            ConvexColliderLibrary colliders;
            FractureLibrary lib;
            Scene s;
            const DetachedLeaderSetup setup
                = SetupDetachedLeader(s, lib, colliders, resources, "fracture-selftest://afterbreak0", 3);
            setup.dc->afterBreak = 0;
            World& w = s.GetWorld();
            FractureSystem fsys;
            std::vector<ShapeImpulse> none;
            for (int i = 0; i < 500; ++i) {
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
            }
            check(w.IsAlive(setup.leader), "afterBreak=0 (keep): the detached piece is never destroyed");
            const auto* leaderFp = w.GetComponent<FracturePieceComponent>(setup.leader);
            check(leaderFp != nullptr && leaderFp->releaseTicks == 500,
                  "afterBreak=0 (keep): releaseTicks keeps advancing (500 ticks after detaching)");
            fracturelib::Install(nullptr);
            convexcol::Install(nullptr);
        }

        // (17b) afterBreak=1 (N tick 後に消える): releaseTicks が afterBreakTicks に達した
        // tick ちょうどで Destroy される
        {
            RenderResources resources;
            ConvexColliderLibrary colliders;
            FractureLibrary lib;
            Scene s;
            const DetachedLeaderSetup setup
                = SetupDetachedLeader(s, lib, colliders, resources, "fracture-selftest://afterbreak1", 3);
            setup.dc->afterBreak = 1;
            setup.dc->afterBreakTicks = 5;
            World& w = s.GetWorld();
            FractureSystem fsys;
            std::vector<ShapeImpulse> none;
            for (int i = 0; i < 4; ++i) {
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
            }
            check(w.IsAlive(setup.leader),
                  "afterBreak=1 (destroy after N ticks): still alive one tick before the threshold");
            fsys.Update(w, 1.0f / 60.0f, none);
            w.ApplyStructuralChanges();
            check(!w.IsAlive(setup.leader),
                  "afterBreak=1 (destroy after N ticks): destroyed exactly on the threshold tick");
            fracturelib::Install(nullptr);
            convexcol::Install(nullptr);
        }

        // (17c) afterBreak=2 (沈んで消える): afterBreakTicks で Collider.mask がクリアされ、
        // 床をすり抜けて沈み、afterBreakTicks+fadeTicks で Destroy される
        {
            RenderResources resources;
            ConvexColliderLibrary colliders;
            FractureLibrary lib;
            Scene s;
            const DetachedLeaderSetup setup
                = SetupDetachedLeader(s, lib, colliders, resources, "fracture-selftest://afterbreak2", 3);
            constexpr int32_t kAfterTicks = 30;
            constexpr int32_t kFadeTicks = 40;
            setup.dc->afterBreak = 2;
            setup.dc->afterBreakTicks = kAfterTicks;
            setup.dc->fadeTicks = kFadeTicks;
            World& w = s.GetWorld();

            // leader のすぐ下 (凸包の半径ぶんだけ隙間を詰めた位置) に床を置く
            const XMFLOAT3 leaderPos = w.GetComponent<LocalTransform>(setup.leader)->position;
            GameObject floor = s.CreateGameObject("Floor");
            floor.SetLocalPosition(leaderPos.x, leaderPos.y - 0.3f, leaderPos.z);
            auto* floorCol = floor.AddComponent<ColliderComponent>();
            floorCol->shape = collidershape::kBox;
            floorCol->halfExtents = { 2.0f, 0.1f, 2.0f };
            w.ApplyStructuralChanges();

            PhysicsSystem phys;
            FractureSystem fsys;
            std::vector<ShapeImpulse> impulses;
            constexpr float kDt = 1.0f / 60.0f;
            float restY = leaderPos.y;
            bool aliveBeforeThreshold = false;
            bool maskClearedAtThreshold = false;
            bool aliveBeforeFinalDestroy = false;
            float fallenY = leaderPos.y;
            bool destroyedAtFinalTick = false;
            for (int32_t tick = 1; tick <= kAfterTicks + kFadeTicks; ++tick) {
                phys.Update(w, kDt, nullptr, nullptr, &impulses);
                fsys.Update(w, kDt, impulses);
                w.ApplyStructuralChanges();

                if (tick == kAfterTicks - 1) {
                    aliveBeforeThreshold = w.IsAlive(setup.leader);
                    restY = w.GetComponent<LocalTransform>(setup.leader)->position.y;
                }
                if (tick == kAfterTicks) {
                    const auto* col = w.GetComponent<ColliderComponent>(setup.leader);
                    maskClearedAtThreshold = (col != nullptr && col->mask == 0);
                }
                if (tick == kAfterTicks + kFadeTicks - 1) {
                    aliveBeforeFinalDestroy = w.IsAlive(setup.leader);
                    if (aliveBeforeFinalDestroy) {
                        fallenY = w.GetComponent<LocalTransform>(setup.leader)->position.y;
                    }
                }
                if (tick == kAfterTicks + kFadeTicks) {
                    destroyedAtFinalTick = !w.IsAlive(setup.leader);
                }
            }
            check(aliveBeforeThreshold, "afterBreak=2 (sink): still alive one tick before afterBreakTicks");
            check(restY > leaderPos.y - 0.5f,
                  "afterBreak=2 (sink): still resting on the floor just before afterBreakTicks");
            check(maskClearedAtThreshold,
                  "afterBreak=2 (sink): the collider mask is cleared exactly at afterBreakTicks");
            check(aliveBeforeFinalDestroy,
                  "afterBreak=2 (sink): still alive one tick before afterBreakTicks+fadeTicks");
            check(fallenY < leaderPos.y - 1.0f,
                  "afterBreak=2 (sink): once the mask is cleared, gravity sinks it well below the floor");
            check(destroyedAtFinalTick,
                  "afterBreak=2 (sink): destroyed exactly at afterBreakTicks+fadeTicks");
            fracturelib::Install(nullptr);
            convexcol::Install(nullptr);
        }

        // (17d) afterBreak=3 (縮んで消える): afterBreakTicks から fadeTicks かけて scale が
        // 線形に下がり、afterBreakTicks+fadeTicks で Destroy される
        {
            RenderResources resources;
            ConvexColliderLibrary colliders;
            FractureLibrary lib;
            Scene s;
            const DetachedLeaderSetup setup
                = SetupDetachedLeader(s, lib, colliders, resources, "fracture-selftest://afterbreak3", 3);
            constexpr int32_t kAfterTicks = 10;
            constexpr int32_t kFadeTicks = 10;
            setup.dc->afterBreak = 3;
            setup.dc->afterBreakTicks = kAfterTicks;
            setup.dc->fadeTicks = kFadeTicks;
            World& w = s.GetWorld();
            FractureSystem fsys;
            std::vector<ShapeImpulse> none;

            for (int i = 0; i < kAfterTicks - 1; ++i) {
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
            }
            check(w.GetComponent<LocalTransform>(setup.leader)->scale.x == 1.0f,
                  "afterBreak=3 (shrink): scale is unchanged before afterBreakTicks");

            fsys.Update(w, 1.0f / 60.0f, none); // releaseTicks == afterBreakTicks (t=0)
            w.ApplyStructuralChanges();
            check(w.GetComponent<LocalTransform>(setup.leader)->scale.x == 1.0f,
                  "afterBreak=3 (shrink): scale starts at 1 exactly on afterBreakTicks");

            for (int i = 0; i < kFadeTicks / 2; ++i) { // releaseTicks == afterBreakTicks + fadeTicks/2
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
            }
            const float scaleHalf = w.GetComponent<LocalTransform>(setup.leader)->scale.x;
            check(scaleHalf > 0.4f && scaleHalf < 0.6f,
                  "afterBreak=3 (shrink): scale is roughly halfway down midway through fadeTicks");

            for (int i = 0; i < kFadeTicks / 2 - 1; ++i) { // releaseTicks == afterBreakTicks+fadeTicks-1
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
            }
            check(w.IsAlive(setup.leader),
                  "afterBreak=3 (shrink): still alive one tick before afterBreakTicks+fadeTicks");

            fsys.Update(w, 1.0f / 60.0f, none); // releaseTicks == afterBreakTicks + fadeTicks
            w.ApplyStructuralChanges();
            check(!w.IsAlive(setup.leader),
                  "afterBreak=3 (shrink): destroyed exactly at afterBreakTicks+fadeTicks");
            fracturelib::Install(nullptr);
            convexcol::Install(nullptr);
        }

        // (17e) afterBreak=4 (静止したら静的化): isSleeping になった tick に Rigidbody が
        // 外れ、Collider は残って静的形状として当たる
        {
            RenderResources resources;
            ConvexColliderLibrary colliders;
            FractureLibrary lib;
            Scene s;
            const DetachedLeaderSetup setup
                = SetupDetachedLeader(s, lib, colliders, resources, "fracture-selftest://afterbreak4", 3);
            setup.dc->afterBreak = 4;
            World& w = s.GetWorld();
            FractureSystem fsys;
            std::vector<ShapeImpulse> none;

            fsys.Update(w, 1.0f / 60.0f, none);
            w.ApplyStructuralChanges();
            check(w.GetComponent<RigidbodyComponent>(setup.leader) != nullptr,
                  "afterBreak=4 (static once asleep): the leader still has a Rigidbody while awake");

            // 実際にスリープへ落ちるまで待つ代わりに、物理が決めた isSleeping そのものを直接
            // 立てる (FractureSystem はこのフラグを読むだけで、眠りの判定自体は関知しない)
            w.GetComponent<RigidbodyComponent>(setup.leader)->isSleeping = true;
            fsys.Update(w, 1.0f / 60.0f, none);
            w.ApplyStructuralChanges();
            check(w.GetComponent<RigidbodyComponent>(setup.leader) == nullptr,
                  "afterBreak=4 (static once asleep): the Rigidbody is removed once isSleeping becomes true");
            check(w.GetComponent<ColliderComponent>(setup.leader) != nullptr,
                  "afterBreak=4 (static once asleep): the piece's Collider remains (now a static shape)");
            fracturelib::Install(nullptr);
            convexcol::Install(nullptr);
        }

        // (17f) afterBreak=5 (上限超過で古い順に消す): 2 つ目の塊が分かれて maxDebris を
        // 超えたら、古い方 (releaseTicks が大きい方) が消える
        {
            RenderResources resources;
            ConvexColliderLibrary colliders;
            FractureLibrary lib;
            colliders.Init(&resources);
            lib.Init(&resources, &colliders);
            fracturelib::Install(&lib);

            constexpr int32_t kCount = 5;
            const FractureBakeResult rowBake = MakeRowFractureBake(kCount, 0.25f);
            const FractureAssetHandle* handle = lib.RegisterBaked(
                "fracture-selftest://afterbreak5", rowBake, HashStr("fracture-selftest://afterbreak5_src"), 0,
                kCount, 0, 0);

            Scene s;
            World& w = s.GetWorld();
            GameObject root = s.CreateGameObject("Row5");
            root.AddComponent<RigidbodyComponent>();
            root.AddComponent<DestructibleComponent>();
            root.GetComponent<RigidbodyComponent>()->mass = static_cast<float>(kCount);
            auto* d = root.GetComponent<DestructibleComponent>();
            d->strength = 100.0f;
            d->afterBreak = 5;
            d->maxDebris = 1;
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://afterbreak5") };
            BuildFracturePieces(w, root.Id(), *handle);
            w.ApplyStructuralChanges();

            const EntityID piece0 = FindPieceChild(w, root.Id(), 0);
            const EntityID piece4 = FindPieceChild(w, root.Id(), kCount - 1);

            FractureSystem fsys;
            std::vector<ShapeImpulse> none;

            // 端 (piece 0) を先に切り離す (releaseTicks が先に進み始める = 後で「古い」側になる)
            ApplyFractureDamage(w, root.Id(), rowBake.pieces[0].origin, 0.0f, 150.0f);
            fsys.Update(w, 1.0f / 60.0f, none);
            w.ApplyStructuralChanges();
            check(w.GetComponent<RigidbodyComponent>(piece0) != nullptr,
                  "afterBreak=5 (cap oldest debris): the first detached piece becomes a leader");

            for (int i = 0; i < 5; ++i) { // releaseTicks に差をつける
                fsys.Update(w, 1.0f / 60.0f, none);
                w.ApplyStructuralChanges();
            }
            check(w.IsAlive(piece0), "afterBreak=5 (cap oldest debris): still within the cap while alone");

            // 反対側の端 (piece 4) を切り離す。塊が 2 つになり maxDebris=1 を超える
            ApplyFractureDamage(w, root.Id(), rowBake.pieces[kCount - 1].origin, 0.0f, 150.0f);
            fsys.Update(w, 1.0f / 60.0f, none);
            w.ApplyStructuralChanges();

            check(!w.IsAlive(piece0),
                  "afterBreak=5 (cap oldest debris): exceeding maxDebris destroys the older chunk");
            check(w.IsAlive(piece4),
                  "afterBreak=5 (cap oldest debris): the newly detached (younger) chunk survives");
            fracturelib::Install(nullptr);
        }

        // (17g) 決定論: 沈む/縮む の挙動を混在させた 2 つの Destructible を同じシーンに置き、
        // 120 tick のハッシュ列が 2 回の独立実行で一致する
        {
            constexpr int32_t kCount = 3;
            const FractureBakeResult rowBake = MakeRowFractureBake(kCount, 0.25f);

            auto buildScene = [&](Scene& s, FractureLibrary& lib, ConvexColliderLibrary& colliders,
                                  RenderResources& resources, const char* prefix) {
                colliders.Init(&resources);
                lib.Init(&resources, &colliders);
                fracturelib::Install(&lib);
                convexcol::Install(&colliders);
                const FractureAssetHandle* handle = lib.RegisterBaked(
                    prefix, rowBake, HashStr(std::string(prefix) + "_src"), 0, kCount, 0, 0);

                World& w = s.GetWorld();
                std::vector<EntityID> roots;
                for (int32_t k = 0; k < 2; ++k) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "Row%d", k);
                    GameObject root = s.CreateGameObject(name);
                    root.SetLocalPosition(static_cast<float>(k) * 4.0f, 0.0f, 0.0f);
                    root.AddComponent<RigidbodyComponent>();
                    root.AddComponent<DestructibleComponent>();
                    auto* rb = root.GetComponent<RigidbodyComponent>();
                    rb->mass = static_cast<float>(kCount);
                    rb->gravityScale = 0.0f; // 落下と混ぜず after-break の帳簿だけを検算する
                    auto* d = root.GetComponent<DestructibleComponent>();
                    d->strength = 100.0f;
                    d->fractureAsset = AssetID{ HashStr(prefix) };
                    d->afterBreak = (k == 0) ? 2 : 3; // 沈む / 縮む を混在させる
                    d->afterBreakTicks = 20;
                    d->fadeTicks = 20;
                    BuildFracturePieces(w, root.Id(), *handle);
                    roots.push_back(root.Id());
                }
                w.ApplyStructuralChanges();
                return roots;
            };

            auto runAndHash = [&](std::vector<uint64_t>& outHashes) {
                RenderResources resources;
                ConvexColliderLibrary colliders;
                FractureLibrary lib;
                Scene s;
                const std::vector<EntityID> roots
                    = buildScene(s, lib, colliders, resources, "fracture-selftest://afterbreak_det");
                World& w = s.GetWorld();

                ApplyFractureDamage(w, roots[0], rowBake.pieces[0].origin, 0.0f, 150.0f);
                XMFLOAT3 p1 = rowBake.pieces[0].origin;
                p1.x += 4.0f; // roots[1] の world 位置ぶんを足す (ApplyFractureDamage は world 座標)
                ApplyFractureDamage(w, roots[1], p1, 0.0f, 150.0f);

                PhysicsSystem phys;
                FractureSystem fsys;
                std::vector<ShapeImpulse> impulses;
                constexpr float kDt = 1.0f / 60.0f;
                outHashes.clear();
                for (int i = 0; i < 120; ++i) {
                    phys.Update(w, kDt, nullptr, nullptr, &impulses);
                    fsys.Update(w, kDt, impulses);
                    w.ApplyStructuralChanges();
                    outHashes.push_back(HashWorld(w));
                }
                fracturelib::Install(nullptr);
                convexcol::Install(nullptr);
            };

            std::vector<uint64_t> hashesA, hashesB;
            runAndHash(hashesA);
            runAndHash(hashesB);
            check(hashesA.size() == 120 && hashesB.size() == 120,
                  "afterBreak determinism: mixed sink/shrink run produced 120 ticks");
            bool allMatch = hashesA.size() == hashesB.size();
            for (size_t i = 0; allMatch && i < hashesA.size(); ++i) {
                if (hashesA[i] != hashesB[i]) {
                    allMatch = false;
                    MYE_LOG_ERROR("    (afterBreak determinism mismatch at tick %zu: 0x%016llX vs 0x%016llX)", i,
                                 static_cast<unsigned long long>(hashesA[i]),
                                 static_cast<unsigned long long>(hashesB[i]));
                }
            }
            check(allMatch, "afterBreak determinism: two independent runs with mixed after-break behaviors "
                  "produce byte-identical hash sequences over 120 ticks");
        }
    }

    // ---- 18. kinematic ルートから分かれた破片の速さの上限 (物理的な妥当性の固定検査) ----
    // 静止した kinematic に球がぶつかって破片が分かれるとき、反発係数が最大 1 でも破片が
    // 受け取れる速さは衝突直前の球の速さの高々 2 倍 (静止した的が完全弾性衝突で跳ね返す上限)。
    // これを大きく超えるなら、分離速度の計算のどこかで質量に依存しない量 (接触インパルス
    // そのものなど) を誤って速度として使っている
    {
        RenderResources resources;
        ConvexColliderLibrary colliders;
        FractureLibrary lib;
        colliders.Init(&resources);
        lib.Init(&resources, &colliders);
        fracturelib::Install(&lib);
        convexcol::Install(&colliders);

        FractureBakeInput in;
        in.sourceMesh = MakeBox(0.5f, 0.5f, 0.5f);
        in.seed = 2;
        in.pieceCount = 12;
        FractureBakeResult bake;
        const bool baked = BakeFracture(in, bake);
        check(baked && bake.success, "fracture system: wall12 bake for the speed-bound check succeeds");
        if (baked && bake.success) {
            const FractureAssetHandle* handle = lib.RegisterBaked(
                "fracture-selftest://speedbound_wall12", bake, HashStr("fracture-selftest://speedbound_wall12_src"),
                in.seed, in.pieceCount, 0, 32);

            Scene s;
            World& w = s.GetWorld();
            GameObject wall = s.CreateGameObject("Wall");
            wall.AddComponent<RigidbodyComponent>()->isKinematic = true;
            auto* d = wall.AddComponent<DestructibleComponent>();
            d->strength = 200.0f;
            d->fractureAsset = AssetID{ HashStr("fracture-selftest://speedbound_wall12") };
            BuildFracturePieces(w, wall.Id(), *handle);

            constexpr float kBallSpeed = 30.0f;
            GameObject ball = s.CreateGameObject("Ball");
            ball.SetLocalPosition(-5.0f, 0.0f, 0.0f);
            auto* bcol = ball.AddComponent<ColliderComponent>();
            bcol->shape = collidershape::kSphere;
            bcol->radius = 0.5f;
            auto* brb = ball.AddComponent<RigidbodyComponent>();
            brb->mass = 20.0f;
            brb->gravityScale = 0.0f;
            brb->velocity = { kBallSpeed, 0.0f, 0.0f };
            w.ApplyStructuralChanges();
            const EntityID wallId = wall.Id();
            std::vector<EntityID> pieceOf(12, kNullEntity);
            for (int32_t i = 0; i < 12; ++i) {
                pieceOf[static_cast<size_t>(i)] = FindPieceChild(w, wallId, i);
            }

            PhysicsSystem phys;
            FractureSystem fsys;
            std::vector<ShapeImpulse> impulses;
            constexpr float kDt = 1.0f / 60.0f;
            float maxDetachedSpeed = 0.0f;
            for (int32_t tick = 0; tick < 30; ++tick) {
                phys.Update(w, kDt, nullptr, nullptr, &impulses);
                fsys.Update(w, kDt, impulses);
                w.ApplyStructuralChanges();
                for (int32_t i = 0; i < 12; ++i) {
                    const EntityID pe = pieceOf[static_cast<size_t>(i)];
                    if (pe.IsNull() || !w.IsAlive(pe)) {
                        continue;
                    }
                    if (const auto* prb = w.GetComponent<RigidbodyComponent>(pe)) {
                        const float sp = std::sqrt(prb->velocity.x * prb->velocity.x
                                                   + prb->velocity.y * prb->velocity.y
                                                   + prb->velocity.z * prb->velocity.z);
                        maxDetachedSpeed = (std::max)(maxDetachedSpeed, sp);
                    }
                }
            }
            const auto* dAfter = w.GetComponent<DestructibleComponent>(wallId);
            check(dAfter != nullptr && dAfter->detachedCount > 0,
                  "fracture system: the speed-bound check actually detaches a piece");
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "fracture system: a piece detached from a struck kinematic wall never exceeds 2x "
                          "the ball's speed (%.3f <= %.3f)",
                          static_cast<double>(maxDetachedSpeed), static_cast<double>(kBallSpeed * 2.0f * 1.05f));
            check(maxDetachedSpeed <= kBallSpeed * 2.0f * 1.05f, buf);

            fracturelib::Install(nullptr);
            convexcol::Install(nullptr);
        }
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Fracture mesh core self test: ALL PASS ====");
    } else {
        MYE_LOG_ERROR("==== Fracture mesh core self test: %d FAILURE(S) ====", failCount);
    }
    return failCount == 0;
}

} // namespace mye
