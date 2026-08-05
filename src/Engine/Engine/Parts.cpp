#include "Engine/Engine/Parts.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

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
    std::function<void(EntityID)> visit = [&](EntityID e) {
        if (auto* p = world.GetComponent<PartComponent>(e); p && p->tag == tag) {
            out.push_back(e);
        }
        auto* h = world.GetComponent<HierarchyComponent>(e);
        EntityID c = h ? h->firstChild : kNullEntity;
        while (!c.IsNull()) {
            // 次を先に控える (訪問中に破棄されても走査が飛ばない家風)
            auto* ch = world.GetComponent<HierarchyComponent>(c);
            const EntityID next = ch ? ch->nextSibling : kNullEntity;
            visit(c);
            c = next;
        }
    };
    visit(root);
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
    p.shape = (b.shape == 1) ? 0 : 1; // PartBounds 0=箱/1=球 → ShapePose 0=球/1=箱 (★番号が逆)
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

bool RaycastParts(World& world, EntityID root, uint64_t tag, const DirectX::XMFLOAT3& origin,
                  const DirectX::XMFLOAT3& dir, float maxDist, PartRayHit& out)
{
    if (!root.IsNull() && !world.IsAlive(root)) {
        return false;
    }
    // 収集 → index 昇順 sort → 厳密 < のみ更新 (RaycastWorld と同じ決定論規約:
    // アーキタイプ順に依存せず、同 t は低 index が勝つ)
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

    bool found = false;
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
        const bool degenerate = (c.b->shape == 1)
            ? (c.b->halfExtents.x <= 0.0f)
            : (std::max(c.b->halfExtents.x, std::max(c.b->halfExtents.y, c.b->halfExtents.z))
               <= 0.0f);
        if (degenerate) {
            continue;
        }
        const ShapePose pose = MakePartBoundsPose(*c.b, *c.wm);
        float t = 0, nx = 0, ny = 0, nz = 0;
        if (!shapes::Raycast(pose, origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, maxDist, t,
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

bool IsStructureLocked(World& world, EntityID e)
{
    if (!world.IsAlive(e)) {
        return false;
    }
    return world.GetComponent<PartComponent>(e) != nullptr
        && world.GetComponent<PrefabLinkComponent>(e) != nullptr;
}

} // namespace mye::Parts
