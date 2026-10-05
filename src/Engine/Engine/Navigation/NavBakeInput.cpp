//====================================================================================
//                          NavBakeInput.cpp
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMesh のベイク入力の収集 (World -> 三角形)
//====================================================================================
#include "Engine/Engine/Navigation/NavBakeInput.h"

#include <algorithm>
#include <cmath>

#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"
#include "Engine/Engine/Physics/Collider/TerrainColliderLibrary.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"

namespace mye {
namespace {

// 決定論: 三角関数を使わない (CRT の sin / cos は構成ごとの定数畳み込みで最後の桁が変わりうる)。
// 球は正八面体の細分 + 正規化 (四則演算と sqrt だけ) で作る

struct Candidate {
    EntityID entity;
    ColliderComponent col;
    DirectX::XMFLOAT4X4 wm;
};

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// ベイク範囲 (ワールド AABB) との重なり判定。範囲外の三角形は捨てる
class BoundsFilter {
public:
    BoundsFilter(const float* bmin, const float* bmax, NavTriangleSoup& out) : out_(out)
    {
        for (int i = 0; i < 3; ++i) {
            min_[i] = bmin[i];
            max_[i] = bmax[i];
        }
    }

    bool OverlapsAabb(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) const
    {
        return minX <= max_[0] && maxX >= min_[0] && minY <= max_[1] && maxY >= min_[1] && minZ <= max_[2]
            && maxZ >= min_[2];
    }

    // 三角形が範囲と重なれば追加する
    void Add(const Vec3& a, const Vec3& b, const Vec3& c)
    {
        const float lo[3] = {(std::min)({a.x, b.x, c.x}), (std::min)({a.y, b.y, c.y}), (std::min)({a.z, b.z, c.z})};
        const float hi[3] = {(std::max)({a.x, b.x, c.x}), (std::max)({a.y, b.y, c.y}), (std::max)({a.z, b.z, c.z})};
        if (!OverlapsAabb(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2])) {
            return;
        }
        const float pa[3] = {a.x, a.y, a.z};
        const float pb[3] = {b.x, b.y, b.z};
        const float pc[3] = {c.x, c.y, c.z};
        out_.AddTriangle(pa, pb, pc);
    }

    // 外向きの基準ベクトル outward に向くよう巻き順を揃えて追加する (凸な立体用)
    void AddOutward(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& outward)
    {
        const Vec3 n = Cross(Sub(b, a), Sub(c, a));
        if (Dot(n, outward) < 0.0f) {
            Add(a, c, b);
        } else {
            Add(a, b, c);
        }
    }

private:
    NavTriangleSoup& out_;
    float min_[3];
    float max_[3];
};

// 形状のローカル軸 (ワールドでの向き) と原点
struct Frame {
    Vec3 origin;
    Vec3 ax, ay, az; // ローカル X / Y / Z 軸のワールド方向 (単位長)

    Vec3 ToWorld(float lx, float ly, float lz) const
    {
        return {origin.x + ax.x * lx + ay.x * ly + az.x * lz, origin.y + ax.y * lx + ay.y * ly + az.y * lz,
                origin.z + ax.z * lx + ay.z * ly + az.z * lz};
    }
    Vec3 DirToWorld(float lx, float ly, float lz) const
    {
        return {ax.x * lx + ay.x * ly + az.x * lz, ax.y * lx + ay.y * ly + az.y * lz,
                ax.z * lx + ay.z * ly + az.z * lz};
    }
};

Frame FrameOf(const ShapePose& p)
{
    Frame f;
    f.origin = {p.px, p.py, p.pz};
    f.ax = {p.bx[0], p.bx[1], p.bx[2]};
    f.ay = {p.by[0], p.by[1], p.by[2]};
    f.az = {p.bz[0], p.bz[1], p.bz[2]};
    return f;
}

void EmitBox(const ShapePose& p, BoundsFilter& filter)
{
    const Frame f = FrameOf(p);
    const float hx = p.hx;
    const float hy = p.hy;
    const float hz = p.hz;
    Vec3 c[8];
    for (int i = 0; i < 8; ++i) {
        const float sx = (i & 1) ? 1.0f : -1.0f;
        const float sy = (i & 2) ? 1.0f : -1.0f;
        const float sz = (i & 4) ? 1.0f : -1.0f;
        c[i] = f.ToWorld(sx * hx, sy * hy, sz * hz);
    }
    // 6 面 (軸ごとの -側 / +側)。各面は四角形 2 枚。外向きは面の中心 - 立体の中心
    const int faces[6][4] = {{0, 2, 6, 4}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 5, 7, 6}};
    const Vec3 center = f.origin;
    for (const auto& q : faces) {
        const Vec3 mid = {(c[q[0]].x + c[q[2]].x) * 0.5f, (c[q[0]].y + c[q[2]].y) * 0.5f,
                          (c[q[0]].z + c[q[2]].z) * 0.5f};
        const Vec3 outward = Sub(mid, center);
        filter.AddOutward(c[q[0]], c[q[1]], c[q[2]], outward);
        filter.AddOutward(c[q[0]], c[q[2]], c[q[3]], outward);
    }
}

// 正八面体の 1 つの八分円を n 分割した格子点 (i, j) (i + j <= n)。正規化して単位球の上へ
Vec3 OctantPoint(float sx, float sy, float sz, int n, int i, int j)
{
    const float wa = static_cast<float>(n - i - j);
    const float wb = static_cast<float>(i);
    const float wc = static_cast<float>(j);
    const float x = wa * sx;
    const float y = wb * sy;
    const float z = wc * sz;
    const float len = std::sqrt(x * x + y * y + z * z);
    return {x / len, y / len, z / len};
}

// 球 (halfSeg = 0) またはカプセル (ローカル Y 軸、線分半長 halfSeg) を三角形へ
void EmitSphereOrCapsule(const ShapePose& p, float halfSeg, BoundsFilter& filter)
{
    const Frame f = FrameOf(p);
    const float r = p.radius;
    constexpr int n = kNavSphereSubdivisions;
    for (int o = 0; o < 8; ++o) {
        const float sx = (o & 1) ? 1.0f : -1.0f;
        const float sy = (o & 2) ? 1.0f : -1.0f;
        const float sz = (o & 4) ? 1.0f : -1.0f;
        const float shift = sy * halfSeg; // 上半球は +Y、下半球は -Y へずらす
        auto world = [&](int i, int j) {
            const Vec3 u = OctantPoint(sx, sy, sz, n, i, j);
            return f.ToWorld(u.x * r, u.y * r + shift, u.z * r);
        };
        auto outwardAt = [&](int i, int j) {
            const Vec3 u = OctantPoint(sx, sy, sz, n, i, j);
            return f.DirToWorld(u.x, u.y, u.z);
        };
        for (int i = 0; i < n; ++i) {
            for (int j = 0; i + j < n; ++j) {
                // 上向きの三角形 (i,j) (i+1,j) (i,j+1)
                filter.AddOutward(world(i, j), world(i + 1, j), world(i, j + 1), outwardAt(i, j));
                if (i + j < n - 1) {
                    // 下向きの三角形 (i+1,j) (i+1,j+1) (i,j+1)
                    filter.AddOutward(world(i + 1, j), world(i + 1, j + 1), world(i, j + 1),
                                      outwardAt(i + 1, j));
                }
            }
        }
        // カプセルの側面。赤道 (y = 0 の縁 = i == 0) を上半球の八分円からだけ橋渡しする
        if (halfSeg > 0.0f && sy > 0.0f) {
            for (int j = 0; j < n; ++j) {
                const Vec3 u0 = OctantPoint(sx, sy, sz, n, 0, j);
                const Vec3 u1 = OctantPoint(sx, sy, sz, n, 0, j + 1);
                const Vec3 top0 = f.ToWorld(u0.x * r, halfSeg, u0.z * r);
                const Vec3 top1 = f.ToWorld(u1.x * r, halfSeg, u1.z * r);
                const Vec3 bot0 = f.ToWorld(u0.x * r, -halfSeg, u0.z * r);
                const Vec3 bot1 = f.ToWorld(u1.x * r, -halfSeg, u1.z * r);
                const Vec3 outward = f.DirToWorld(u0.x + u1.x, 0.0f, u0.z + u1.z);
                filter.AddOutward(bot0, bot1, top1, outward);
                filter.AddOutward(bot0, top1, top0, outward);
            }
        }
    }
}

// ワールド AABB をメッシュ / 地形のローカル (スケール前) AABB へ。逆回転 = 基底の転置。保守的
void WorldBoxToLocal(const ShapePose& p, const float* bmin, const float* bmax, float* lmin, float* lmax)
{
    const float cx = (bmin[0] + bmax[0]) * 0.5f - p.px;
    const float cy = (bmin[1] + bmax[1]) * 0.5f - p.py;
    const float cz = (bmin[2] + bmax[2]) * 0.5f - p.pz;
    const float ex = (bmax[0] - bmin[0]) * 0.5f;
    const float ey = (bmax[1] - bmin[1]) * 0.5f;
    const float ez = (bmax[2] - bmin[2]) * 0.5f;
    const float lc[3] = {p.bx[0] * cx + p.bx[1] * cy + p.bx[2] * cz, p.by[0] * cx + p.by[1] * cy + p.by[2] * cz,
                         p.bz[0] * cx + p.bz[1] * cy + p.bz[2] * cz};
    const float le[3] = {
        std::fabs(p.bx[0]) * ex + std::fabs(p.bx[1]) * ey + std::fabs(p.bx[2]) * ez,
        std::fabs(p.by[0]) * ex + std::fabs(p.by[1]) * ey + std::fabs(p.by[2]) * ez,
        std::fabs(p.bz[0]) * ex + std::fabs(p.bz[1]) * ey + std::fabs(p.bz[2]) * ez,
    };
    const float inv[3] = {1.0f / (std::max)(p.sx, 1e-6f), 1.0f / (std::max)(p.sy, 1e-6f),
                          1.0f / (std::max)(p.sz, 1e-6f)};
    for (int i = 0; i < 3; ++i) {
        lmin[i] = (lc[i] - le[i]) * inv[i];
        lmax[i] = (lc[i] + le[i]) * inv[i];
    }
}

void EmitMesh(const ShapePose& p, const float* bmin, const float* bmax, BoundsFilter& filter)
{
    const auto* md = static_cast<const MeshColliderData*>(p.meshData);
    if (md == nullptr || md->TriCount() == 0) {
        return;
    }
    float lmin[3];
    float lmax[3];
    WorldBoxToLocal(p, bmin, bmax, lmin, lmax);
    std::vector<int32_t> hits(static_cast<size_t>(md->TriCount()));
    const int count = MeshGatherTris(*md, lmin[0], lmin[1], lmin[2], lmax[0], lmax[1], lmax[2], hits.data(),
                                     static_cast<int>(hits.size()));
    const Frame f = FrameOf(p);
    auto world = [&](uint32_t index) {
        const DirectX::XMFLOAT3& v = md->positions[index];
        return f.ToWorld(v.x * p.sx, v.y * p.sy, v.z * p.sz);
    };
    for (int k = 0; k < count; ++k) {
        const size_t base = static_cast<size_t>(hits[static_cast<size_t>(k)]) * 3;
        filter.Add(world(md->indices[base]), world(md->indices[base + 1]), world(md->indices[base + 2]));
    }
}

void EmitConvex(const ShapePose& p, BoundsFilter& filter)
{
    const auto* hull = static_cast<const ConvexHullData*>(p.meshData);
    if (hull == nullptr || !hull->Valid()) {
        return;
    }
    const Frame f = FrameOf(p);
    for (const ConvexFace& face : hull->faces) {
        const Vec3 outward = f.DirToWorld(face.nx, face.ny, face.nz);
        auto world = [&](int32_t k) {
            const DirectX::XMFLOAT3& v = hull->verts[static_cast<size_t>(hull->faceVerts[static_cast<size_t>(k)])];
            return f.ToWorld(v.x * p.sx, v.y * p.sy, v.z * p.sz);
        };
        for (int32_t k = 1; k + 1 < face.count; ++k) {
            filter.AddOutward(world(face.first), world(face.first + k), world(face.first + k + 1), outward);
        }
    }
}

// 地形の頂点 (texel の x, z)。TerrainWorldTri (Shapes.cpp) と同じ式 — 見た目と当たりと歩ける面を揃える
Vec3 TerrainVertex(const ShapePose& p, const TerrainAsset::TerrainData& d, int32_t tx, int32_t tz)
{
    const int32_t maxX = static_cast<int32_t>(d.heightW) - 1;
    const int32_t maxZ = static_cast<int32_t>(d.heightH) - 1;
    const int32_t cx = (std::min)((std::max)(tx, 0), maxX);
    const int32_t cz = (std::min)((std::max)(tz, 0), maxZ);
    const float u = static_cast<float>(cx) / static_cast<float>(maxX);
    const float v = static_cast<float>(cz) / static_cast<float>(maxZ);
    const float lx = (u - 0.5f) * d.worldSizeX;
    const float ly = d.HeightAtTexel(static_cast<uint32_t>(cx), static_cast<uint32_t>(cz));
    const float lz = (v - 0.5f) * d.worldSizeZ;
    return FrameOf(p).ToWorld(lx * p.sx, ly * p.sy, lz * p.sz);
}

void EmitTerrain(const ShapePose& p, const float* bmin, const float* bmax, BoundsFilter& filter)
{
    const auto* t = static_cast<const TerrainCollisionData*>(p.meshData);
    if (t == nullptr) {
        return;
    }
    const TerrainAsset::TerrainData& d = t->data;
    if (d.heightW < 2 || d.heightH < 2 || d.worldSizeX <= 0.0f || d.worldSizeZ <= 0.0f) {
        return;
    }
    float lmin[3];
    float lmax[3];
    WorldBoxToLocal(p, bmin, bmax, lmin, lmax);
    const int32_t tilesX = static_cast<int32_t>(d.heightW) - 1;
    const int32_t tilesZ = static_cast<int32_t>(d.heightH) - 1;
    const float stepX = d.worldSizeX / static_cast<float>(tilesX);
    const float stepZ = d.worldSizeZ / static_cast<float>(tilesZ);
    const int32_t ix0 = (std::max)(0, static_cast<int32_t>(std::floor((lmin[0] + d.worldSizeX * 0.5f) / stepX)));
    const int32_t ix1 = (std::min)(tilesX - 1, static_cast<int32_t>(std::floor((lmax[0] + d.worldSizeX * 0.5f) / stepX)));
    const int32_t iz0 = (std::max)(0, static_cast<int32_t>(std::floor((lmin[2] + d.worldSizeZ * 0.5f) / stepZ)));
    const int32_t iz1 = (std::min)(tilesZ - 1, static_cast<int32_t>(std::floor((lmax[2] + d.worldSizeZ * 0.5f) / stepZ)));
    // 巻き順は TerrainWorldTri と同じ (i00, i01, i11) / (i00, i11, i10) — 法線が +Y になる
    for (int32_t iz = iz0; iz <= iz1; ++iz) {
        for (int32_t ix = ix0; ix <= ix1; ++ix) {
            const Vec3 v00 = TerrainVertex(p, d, ix, iz);
            const Vec3 v01 = TerrainVertex(p, d, ix, iz + 1);
            const Vec3 v11 = TerrainVertex(p, d, ix + 1, iz + 1);
            const Vec3 v10 = TerrainVertex(p, d, ix + 1, iz);
            filter.Add(v00, v01, v11);
            filter.Add(v00, v11, v10);
        }
    }
}

} // namespace

void NavTriangleSoup::AddTriangle(const float* a, const float* b, const float* c)
{
    const int base = static_cast<int>(verts.size() / 3);
    verts.insert(verts.end(), a, a + 3);
    verts.insert(verts.end(), b, b + 3);
    verts.insert(verts.end(), c, c + 3);
    tris.push_back(base);
    tris.push_back(base + 1);
    tris.push_back(base + 2);
}

NavTriangleInput NavTriangleSoup::View() const
{
    NavTriangleInput in;
    in.verts = verts.data();
    in.vertCount = static_cast<int>(verts.size() / 3);
    in.tris = tris.data();
    in.triCount = TriangleCount();
    return in;
}

void NavCollectTriangles(World& world, const std::vector<NavCollectRange>& ranges, NavTriangleSoup& out)
{
    if (ranges.empty()) {
        return;
    }
    float boundsMin[3];
    float boundsMax[3];
    for (int i = 0; i < 3; ++i) {
        boundsMin[i] = ranges[0].boundsMin[i];
        boundsMax[i] = ranges[0].boundsMax[i];
        for (const NavCollectRange& r : ranges) {
            boundsMin[i] = (std::min)(boundsMin[i], r.boundsMin[i]);
            boundsMax[i] = (std::max)(boundsMax[i], r.boundsMax[i]);
        }
    }
    std::vector<Candidate> candidates;
    const ComponentTypeId req[] = {ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId};
    world.ForEachArchetype(req, [&](Archetype& arch) {
        // 動く物はベイクに入れない (AcousticField::BakeOccupancy と同じ規則)
        if (arch.HasType(RigidbodyComponent::sTypeId) || arch.HasType(CharacterControllerComponent::sTypeId)) {
            return;
        }
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            if (col->isTrigger) {
                continue;
            }
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            Candidate c;
            c.entity = e;
            c.col = *col;
            c.wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value;
            candidates.push_back(c);
        }
    });
    // アーキタイプの列挙順は生成順に依るので、明示的なキーで並べる
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.entity.index < b.entity.index; });

    BoundsFilter filter(boundsMin, boundsMax, out);
    for (const Candidate& c : candidates) {
        const ShapePose pose = shapes::MakePoseFromMatrix(c.col, c.wm);
        float minX = 0.0f, minY = 0.0f, minZ = 0.0f, maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
        shapes::ComputeAabb(pose, minX, minY, minZ, maxX, maxY, maxZ);
        // どれかの範囲と重なり、その範囲のレイヤー集合に入るものだけ
        bool wanted = false;
        for (const NavCollectRange& r : ranges) {
            wanted = wanted
                || (shapes::LayerHit(r.collectLayerMask, c.col.layer) && minX <= r.boundsMax[0] && maxX >= r.boundsMin[0]
                    && minY <= r.boundsMax[1] && maxY >= r.boundsMin[1] && minZ <= r.boundsMax[2]
                    && maxZ >= r.boundsMin[2]);
        }
        if (!wanted) {
            continue;
        }
        switch (pose.shape) {
        case collidershape::kBox:
            EmitBox(pose, filter);
            break;
        case collidershape::kSphere:
            EmitSphereOrCapsule(pose, 0.0f, filter);
            break;
        case collidershape::kCapsule:
            EmitSphereOrCapsule(pose, pose.halfSeg, filter);
            break;
        case collidershape::kMesh:
            EmitMesh(pose, boundsMin, boundsMax, filter);
            break;
        case collidershape::kTerrain:
            EmitTerrain(pose, boundsMin, boundsMax, filter);
            break;
        case collidershape::kConvex:
            EmitConvex(pose, filter);
            break;
        default:
            break;
        }
    }
}

namespace {

constexpr float kRadiansToDegrees = 57.2957795f;
constexpr float kDegreesToRadians = 0.0174532925f;
// tan が 0 に近い傾斜 (ほぼ水平) では式が発散する。この角度以下は半径側の上限だけを使う
constexpr float kMinSlopeDegForCellSize = 0.5f;
// 実効の坂の上限との比較に許す余裕 (度)。45 度ちょうどの設定が浮動小数点誤差で警告にならないように
constexpr float kSlopeWarnToleranceDeg = 0.5f;
constexpr float kClimbCompareTolerance = 1e-4f;

// Recast に渡る段差 (m)。walkableClimb はセルの高さの整数倍に切り捨てられる
float QuantizedClimb(const NavMeshSurfaceComponent& surface, float cellHeight)
{
    if (!(cellHeight > 0.0f) || !(surface.maxClimb > 0.0f)) {
        return 0.0f;
    }
    return std::floor(surface.maxClimb / cellHeight) * cellHeight;
}

// 自動のとき段差を何セルで表すか。Recast は段差を cellHeight の整数倍で比べるので、細かいほど
// 「maxClimb をわずかに超える段差」の接続が減る。6 セルなら誤差は 1/6 (0.3 m で 5 cm)
constexpr float kAutoClimbCells = 6.0f;
// maxClimb / 6 が浮動小数点で 5.9999995 になり floor で 5 セルに落ちるのを避ける余裕
constexpr float kAutoClimbRoundingGuard = 0.9999f;
// cellHeight の下限。Surface.cellHeight の最小値と同じ (これより細かいとボクセルの高さ方向が破綻する)
constexpr float kNavMinCellHeight = 0.02f;

} // namespace

NavCellSize NavResolveCellSize(const NavMeshSurfaceComponent& surface)
{
    NavCellSize out;
    if (!surface.autoCellSize) {
        out.cellSize = surface.cellSize;
        out.cellHeight = surface.cellHeight;
        return out;
    }
    float cs = surface.agentRadius * 0.5f;
    if (surface.maxSlopeDeg > kMinSlopeDegForCellSize) {
        const float tanSlope = std::tan((std::min)(surface.maxSlopeDeg, 89.0f) * kDegreesToRadians);
        cs = (std::min)(cs, surface.maxClimb / (2.0f * tanSlope));
    }
    if (cs < kNavMinCellSize) {
        cs = kNavMinCellSize;
        out.clampedToMinimum = true;
    }
    out.cellSize = cs;
    // 目標の高さ: cs の半分と maxClimb の 1/6 の細かいほう。maxClimb がちょうど整数セルになるよう分割数へ丸める
    const float targetHeight = (std::max)(kNavMinCellHeight, (std::min)(cs * 0.5f, surface.maxClimb / kAutoClimbCells));
    if (surface.maxClimb > 0.0f) {
        const float cells = (std::max)(1.0f, std::ceil(surface.maxClimb / targetHeight - 1e-3f));
        out.cellHeight = (std::max)(kNavMinCellHeight, surface.maxClimb / cells * kAutoClimbRoundingGuard);
    } else {
        out.cellHeight = kNavMinCellHeight;
    }
    return out;
}

float NavEffectiveSlopeLimitDeg(const NavMeshSurfaceComponent& surface, float cellSize)
{
    if (!(cellSize > 0.0f)) {
        return 0.0f;
    }
    return std::atan(QuantizedClimb(surface, NavResolveCellSize(surface).cellHeight) / (2.0f * cellSize)) * kRadiansToDegrees;
}

bool NavSurfaceSlopeUnreachable(const NavMeshSurfaceComponent& surface)
{
    const NavCellSize cs = NavResolveCellSize(surface);
    return surface.maxSlopeDeg > NavEffectiveSlopeLimitDeg(surface, cs.cellSize) + kSlopeWarnToleranceDeg;
}

bool NavAgentStepBelowClimb(const CharacterControllerComponent& cc, float worldScaleY,
                            const NavMeshSurfaceComponent& surface)
{
    return cc.stepOffset * std::fabs(worldScaleY) + kClimbCompareTolerance < surface.maxClimb;
}

NavBakeConfig NavMakeBakeConfig(const NavMeshSurfaceComponent& surface, const DirectX::XMFLOAT4X4& wm)
{
    NavBakeConfig c;
    const NavCellSize resolved = NavResolveCellSize(surface);
    c.cellSize = resolved.cellSize;
    c.cellHeight = resolved.cellHeight;
    c.tileSize = surface.tileSize;
    c.agentHeight = surface.agentHeight;
    c.agentRadius = surface.agentRadius;
    c.agentMaxClimb = surface.maxClimb;
    c.agentMaxSlopeDeg = surface.maxSlopeDeg;
    c.generateLinks = surface.generateLinks ? 1 : 0;
    c.linkDropHeight = surface.dropHeight;
    c.linkJumpDistance = surface.jumpDistance;
    // ローカル箱 8 隅をワールドへ送った AABB (Recast は軸平行の範囲しか焼けない)
    float lo[3] = {0.0f, 0.0f, 0.0f};
    float hi[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 8; ++i) {
        const float lx = surface.center.x + ((i & 1) ? 0.5f : -0.5f) * surface.size.x;
        const float ly = surface.center.y + ((i & 2) ? 0.5f : -0.5f) * surface.size.y;
        const float lz = surface.center.z + ((i & 4) ? 0.5f : -0.5f) * surface.size.z;
        const float w[3] = {lx * wm._11 + ly * wm._21 + lz * wm._31 + wm._41,
                            lx * wm._12 + ly * wm._22 + lz * wm._32 + wm._42,
                            lx * wm._13 + ly * wm._23 + lz * wm._33 + wm._43};
        for (int k = 0; k < 3; ++k) {
            lo[k] = (i == 0) ? w[k] : (std::min)(lo[k], w[k]);
            hi[k] = (i == 0) ? w[k] : (std::max)(hi[k], w[k]);
        }
    }
    for (int k = 0; k < 3; ++k) {
        c.boundsMin[k] = lo[k];
        c.boundsMax[k] = hi[k];
    }
    return c;
}

namespace {

bool SurfaceKeyLess(const EntityID& a, const EntityID& b)
{
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

} // namespace

void NavCollectSurfaceGroups(World& world, std::vector<NavSurfaceGroup>& out)
{
    out.clear();
    struct Entry {
        EntityID entity;
        int32_t agentTypeId = 0;
    };
    std::vector<Entry> entries;
    const ComponentTypeId req[] = { NavMeshSurfaceComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(NavMeshSurfaceComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (IsEntityActive(world, e)) {
                entries.push_back({ e, static_cast<const NavMeshSurfaceComponent*>(arch.GetPtr(si, row))->agentTypeId });
            }
        }
    });
    // アーキタイプの列挙順は生成順に依るので、エンティティキー順に並べる。先に出た型ほど leader のキーが小さい
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return SurfaceKeyLess(a.entity, b.entity); });
    for (const Entry& entry : entries) {
        NavSurfaceGroup* group = nullptr;
        for (NavSurfaceGroup& g : out) {
            if (g.agentTypeId == entry.agentTypeId) {
                group = &g;
                break;
            }
        }
        if (group == nullptr) {
            group = &out.emplace_back();
            group->agentTypeId = entry.agentTypeId;
            group->leader = entry.entity;
        }
        group->members.push_back(entry.entity);
    }
}

bool NavFindSurfaceGroup(World& world, EntityID surface, NavSurfaceGroup& out)
{
    if (!world.IsAlive(surface) || world.GetComponent<NavMeshSurfaceComponent>(surface) == nullptr
        || !IsEntityActive(world, surface)) {
        return false;
    }
    std::vector<NavSurfaceGroup> groups;
    NavCollectSurfaceGroups(world, groups);
    for (NavSurfaceGroup& g : groups) {
        if (std::find(g.members.begin(), g.members.end(), surface) != g.members.end()) {
            out = std::move(g);
            return true;
        }
    }
    return false;
}

} // namespace mye
