#include "Engine/Engine/Animation/Parts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/HierarchyWalk.h"
#include "Engine/Core/Ecs/World.h"

namespace mye::Parts {

EntityID FindPart(World& world, EntityID root, std::string_view utf8Path)
{
    if (!world.IsAlive(root)) {
        return kNullEntity;
    }
    EntityID cur = root;
    size_t i = 0;
    while (i < utf8Path.size()) {
        const size_t sep = utf8Path.find('/', i);
        const size_t end = (sep == std::string_view::npos) ? utf8Path.size() : sep;
        const std::string_view seg = utf8Path.substr(i, end - i);
        i = (sep == std::string_view::npos) ? utf8Path.size() : sep + 1;
        if (seg.empty()) {
            continue; // 先頭/末尾/連続の '/' は読み飛ばす
        }
        auto* h = world.GetComponent<HierarchyComponent>(cur);
        EntityID c = h ? h->firstChild : kNullEntity;
        EntityID hit = kNullEntity;
        while (!c.IsNull()) {
            if (seg == world.GetName(c)) {
                hit = c;
                break; // 最初の一致 (兄弟名の一意化は M48b がエディタ操作時に保証する)
            }
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            c = ch ? ch->nextSibling : kNullEntity;
        }
        if (hit.IsNull()) {
            return kNullEntity;
        }
        cur = hit;
    }
    return cur;
}

void FindPartsByTag(World& world, EntityID root, uint64_t tag, std::vector<EntityID>& out)
{
    if (!world.IsAlive(root)) {
        return;
    }
    // 入れ子インスタンスの境界は見ない (フラット走査) — Parts.h の設計判断
    ForEachInSubtree(world, root, [&](EntityID e, uint32_t) {
        if (auto* p = world.GetComponent<PartComponent>(e); p && p->tag == tag) {
            out.push_back(e);
        }
        return WalkStep::Continue;
    });
}

EntityID ResolvePartSource(World& world, EntityID part, EntityID explicitSource)
{
    if (!explicitSource.IsNull() && world.IsAlive(explicitSource)
        && world.GetComponent<SkinnedMeshComponent>(explicitSource)) {
        return explicitSource;
    }
    for (EntityID a = world.GetParent(part); !a.IsNull(); a = world.GetParent(a)) {
        if (world.GetComponent<SkinnedMeshComponent>(a)) {
            return a;
        }
    }
    return kNullEntity;
}

ShapePose MakePartBoundsPose(const PartBoundsComponent& b, const DirectX::XMFLOAT4X4& wm)
{
    ShapePose p;
    // ★PartBounds と ShapePose は番号が逆 (partboundsshape::kBox == collidershape::kSphere == 0)
    p.shape = (b.shape == partboundsshape::kSphere) ? collidershape::kSphere : collidershape::kBox;
    // スケール近似: 行ベクトル長 (shapes::MakePoseFromMatrix と同一式)
    const float sx = std::sqrt(wm._11 * wm._11 + wm._12 * wm._12 + wm._13 * wm._13);
    const float sy = std::sqrt(wm._21 * wm._21 + wm._22 * wm._22 + wm._23 * wm._23);
    const float sz = std::sqrt(wm._31 * wm._31 + wm._32 * wm._32 + wm._33 * wm._33);
    if (sx > 1e-8f) {
        p.bx[0] = wm._11 / sx;
        p.bx[1] = wm._12 / sx;
        p.bx[2] = wm._13 / sx;
    }
    if (sy > 1e-8f) {
        p.by[0] = wm._21 / sy;
        p.by[1] = wm._22 / sy;
        p.by[2] = wm._23 / sy;
    }
    if (sz > 1e-8f) {
        p.bz[0] = wm._31 / sz;
        p.bz[1] = wm._32 / sz;
        p.bz[2] = wm._33 / sz;
    }
    // ColliderComponent と違い center を持つので、平行移動は wm._41.. 直読みではなく
    // ローカルオフセットのフル変換 (行ベクトル流儀)
    p.px = b.center.x * wm._11 + b.center.y * wm._21 + b.center.z * wm._31 + wm._41;
    p.py = b.center.x * wm._12 + b.center.y * wm._22 + b.center.z * wm._32 + wm._42;
    p.pz = b.center.x * wm._13 + b.center.y * wm._23 + b.center.z * wm._33 + wm._43;
    p.identityRot = (p.bx[0] == 1.0f && p.bx[1] == 0.0f && p.bx[2] == 0.0f && p.by[0] == 0.0f
                     && p.by[1] == 1.0f && p.by[2] == 0.0f && p.bz[0] == 0.0f && p.bz[1] == 0.0f
                     && p.bz[2] == 1.0f)
        ? 1
        : 0;
    // 球の半径 = x × 最大軸スケール (ApplyScaledExtents の max 規約)、箱は軸別
    p.radius = b.halfExtents.x * std::max(sx, std::max(sy, sz));
    p.hx = b.halfExtents.x * sx;
    p.hy = b.halfExtents.y * sy;
    p.hz = b.halfExtents.z * sz;
    return p;
}

namespace {

// 判定に使える部位 (収集済み: tag 一致 / root 配下 / アクティブ / 非縮退) とそのワールド pose
struct PartCandidate {
    EntityID e;
    ShapePose pose;
};

// RaycastParts / Overlap* / SphereCastParts 共通の候補収集。entity.index 昇順で out へ積む
// (アーキタイプ順に依存させない = 同 t / 同結果は低 index が勝つ決定論規約の土台)
void CollectPartCandidates(World& world, EntityID root, uint64_t tag,
                           std::vector<PartCandidate>& out)
{
    out.clear();
    if (!root.IsNull() && !world.IsAlive(root)) {
        return;
    }
    struct Cand {
        EntityID e;
        const PartBoundsComponent* b;
        const DirectX::XMFLOAT4X4* wm;
    };
    std::vector<Cand> cands;
    const ComponentTypeId req[] = { PartComponent::sTypeId, PartBoundsComponent::sTypeId,
                                    WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int pi = arch.FindTypeIndex(PartComponent::sTypeId);
        const int bi = arch.FindTypeIndex(PartBoundsComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const auto* pc = static_cast<const PartComponent*>(arch.GetPtr(pi, row));
            if (tag != 0 && pc->tag != tag) {
                continue;
            }
            cands.push_back(
                { arch.EntityAt(row), static_cast<const PartBoundsComponent*>(arch.GetPtr(bi, row)),
                  &static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row))->value });
        }
    });
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.e.index < b.e.index; });

    for (const Cand& c : cands) {
        if (!IsEntityActive(world, c.e)) {
            continue; // 物理クエリと同じ規約 (無効エンティティは当たらない)
        }
        if (!root.IsNull()) {
            bool under = false;
            for (EntityID a = c.e; !a.IsNull(); a = world.GetParent(a)) {
                if (a == root) {
                    under = true;
                    break;
                }
            }
            if (!under) {
                continue;
            }
        }
        // ゼロ/負寸法はスキップ (入力のみ依存の分岐 = 決定論に無害)
        const bool degenerate = (c.b->shape == partboundsshape::kSphere)
            ? (c.b->halfExtents.x <= 0.0f)
            : (std::max(c.b->halfExtents.x, std::max(c.b->halfExtents.y, c.b->halfExtents.z))
               <= 0.0f);
        if (degenerate) {
            continue;
        }
        out.push_back({ c.e, MakePartBoundsPose(*c.b, *c.wm) });
    }
}

// 球 pose (無回転)
ShapePose MakeSpherePose(const DirectX::XMFLOAT3& center, float radius)
{
    ShapePose p;
    p.shape = collidershape::kSphere;
    p.identityRot = 1;
    p.px = center.x;
    p.py = center.y;
    p.pz = center.z;
    p.radius = radius;
    return p;
}

// 両端点 (半球の中心) と半径のカプセル pose。軸は Shapes.cpp の CapsuleSegment に合わせて by。
// 残り 2 軸は、軸との直交に最も近い座標軸との外積で決定的に作る
ShapePose MakeCapsulePose(const DirectX::XMFLOAT3& p0, const DirectX::XMFLOAT3& p1, float radius)
{
    const float dx = p1.x - p0.x, dy = p1.y - p0.y, dz = p1.z - p0.z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const DirectX::XMFLOAT3 mid = { (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f,
                                    (p0.z + p1.z) * 0.5f };
    if (len < 1e-6f) {
        return MakeSpherePose(mid, radius);
    }
    const float ax = dx / len, ay = dy / len, az = dz / len;
    const float aax = std::fabs(ax), aay = std::fabs(ay), aaz = std::fabs(az);
    float ux = 0.0f, uy = 0.0f, uz = 0.0f;
    if (aax <= aay && aax <= aaz) {
        ux = 1.0f;
    } else if (aay <= aaz) {
        uy = 1.0f;
    } else {
        uz = 1.0f;
    }
    // bx = normalize(axis × u)、bz = bx × axis
    float bxx = ay * uz - az * uy, bxy = az * ux - ax * uz, bxz = ax * uy - ay * ux;
    const float bl = std::sqrt(bxx * bxx + bxy * bxy + bxz * bxz);
    bxx /= bl;
    bxy /= bl;
    bxz /= bl;
    ShapePose p;
    p.shape = collidershape::kCapsule;
    p.identityRot = 0;
    p.px = mid.x;
    p.py = mid.y;
    p.pz = mid.z;
    p.bx[0] = bxx; p.bx[1] = bxy; p.bx[2] = bxz;
    p.by[0] = ax; p.by[1] = ay; p.by[2] = az;
    p.bz[0] = bxy * az - bxz * ay;
    p.bz[1] = bxz * ax - bxx * az;
    p.bz[2] = bxx * ay - bxy * ax;
    p.radius = radius;
    p.halfSeg = len * 0.5f;
    return p;
}

void OverlapPartsWithPose(World& world, EntityID root, uint64_t tag, const ShapePose& probe,
                          std::vector<EntityID>& out)
{
    std::vector<PartCandidate> cands;
    CollectPartCandidates(world, root, tag, cands);
    for (const PartCandidate& c : cands) {
        if (shapes::Overlap(probe, c.pose)) {
            out.push_back(c.e);
        }
    }
}

} // namespace

bool RaycastParts(World& world, EntityID root, uint64_t tag, const DirectX::XMFLOAT3& origin,
                  const DirectX::XMFLOAT3& dir, float maxDist, PartRayHit& out)
{
    // 厳密 < のみ更新 (RaycastWorld と同じ決定論規約: 同 t は低 index が勝つ)
    std::vector<PartCandidate> cands;
    CollectPartCandidates(world, root, tag, cands);
    bool found = false;
    for (const PartCandidate& c : cands) {
        float t = 0, nx = 0, ny = 0, nz = 0;
        if (!shapes::Raycast(c.pose, origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, maxDist, t,
                             nx, ny, nz)) {
            continue;
        }
        if (!found || t < out.distance) {
            found = true;
            out.entity = c.e;
            out.distance = t;
            out.point = { origin.x + dir.x * t, origin.y + dir.y * t, origin.z + dir.z * t };
            out.normal = { nx, ny, nz };
        }
    }
    return found;
}

void OverlapSphereParts(World& world, EntityID root, uint64_t tag, const DirectX::XMFLOAT3& center,
                        float radius, std::vector<EntityID>& out)
{
    out.clear();
    if (radius <= 0.0f) {
        return;
    }
    OverlapPartsWithPose(world, root, tag, MakeSpherePose(center, radius), out);
}

void OverlapCapsuleParts(World& world, EntityID root, uint64_t tag, const DirectX::XMFLOAT3& p0,
                         const DirectX::XMFLOAT3& p1, float radius, std::vector<EntityID>& out)
{
    out.clear();
    if (radius <= 0.0f) {
        return;
    }
    OverlapPartsWithPose(world, root, tag, MakeCapsulePose(p0, p1, radius), out);
}

bool SphereCastParts(World& world, EntityID root, uint64_t tag, const DirectX::XMFLOAT3& origin,
                     const DirectX::XMFLOAT3& dir, float radius, float maxDist, PartRayHit& out)
{
    if (radius <= 0.0f) {
        return RaycastParts(world, root, tag, origin, dir, maxDist, out);
    }
    // 以降は PhysicsQueries.cpp の SphereCastWorld の対象 1 件分と同じ式・同じ定数
    if (maxDist <= 0.0f) {
        maxDist = 1e9f;
    }
    std::vector<PartCandidate> cands;
    CollectPartCandidates(world, root, tag, cands);

    float bestT = maxDist;
    bool hit = false;
    for (const PartCandidate& c : cands) {
        if (c.pose.shape == collidershape::kSphere) {
            // 半径を radius だけ膨らませた球へのレイ = 掃引球の解析解
            ShapePose inflated = c.pose;
            inflated.radius += radius;
            float ht, nx, ny, nz;
            if (!shapes::Raycast(inflated, origin.x, origin.y, origin.z, dir.x, dir.y, dir.z,
                                 bestT, ht, nx, ny, nz)
                || ht >= bestT) { // 昇順走査 + 厳密 < なので同 t は低 index が残る
                continue;
            }
            bestT = ht;
            hit = true;
            out.entity = c.e;
            out.distance = ht;
            // 掃引球の中心位置から元形状の表面点を復元 (法線は膨張面と同方向)
            const float cx = origin.x + dir.x * ht, cy = origin.y + dir.y * ht,
                        cz = origin.z + dir.z * ht;
            out.point = { cx - nx * radius, cy - ny * radius, cz - nz * radius };
            out.normal = { nx, ny, nz };
        } else {
            // 箱: 保守的前進 (固定 32 回。ヒット後も回数は消化する)
            float ct = 0.0f;
            bool found = false;
            float foundT = 0.0f;
            for (int step = 0; step < 32; ++step) {
                if (found || ct > bestT) {
                    continue;
                }
                const float px = origin.x + dir.x * ct;
                const float py = origin.y + dir.y * ct;
                const float pz = origin.z + dir.z * ct;
                const float d = shapes::DistanceToShape(c.pose, px, py, pz) - radius;
                if (d < 1e-4f) {
                    found = true;
                    foundT = ct;
                } else {
                    ct += d;
                }
            }
            if (!found || foundT >= bestT) {
                continue;
            }
            const float px = origin.x + dir.x * foundT;
            const float py = origin.y + dir.y * foundT;
            const float pz = origin.z + dir.z * foundT;
            float qx, qy, qz;
            shapes::ClosestPointOnShape(c.pose, px, py, pz, qx, qy, qz);
            const float vx = px - qx, vy = py - qy, vz = pz - qz;
            const float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (vlen < 1e-6f) {
                continue; // 始点が形状内 (縮退) は報告しない
            }
            bestT = foundT;
            hit = true;
            out.entity = c.e;
            out.distance = foundT;
            out.point = { qx, qy, qz };
            out.normal = { vx / vlen, vy / vlen, vz / vlen };
        }
    }
    return hit;
}

bool IsStructureLocked(World& world, EntityID e)
{
    if (!world.IsAlive(e)) {
        return false;
    }
    return world.GetComponent<PartComponent>(e) != nullptr
        && world.GetComponent<PrefabLinkComponent>(e) != nullptr;
}

} // namespace mye::Parts
