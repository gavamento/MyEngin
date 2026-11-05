//====================================================================================
//                          SpringFollowSystem.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          ばね追従 (SpringFollow、M94g、ADR-031) の更新の実装
//====================================================================================
#include "Engine/Engine/Feel/SpringFollowSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Shared/FeelMath.h"

using namespace DirectX;

namespace mye {

namespace {

constexpr int kMaxHierarchyDepth = 64; // 親子の連鎖をこの深さで打ち切る (壊れた循環で止まらないため)
constexpr float kLn2 = 0.693147180559945f;

// LocalTransform の親子連鎖から world 行列を組む (行ベクトル規約: world = local * parentWorld)。
// TransformSystem と同じ合成 (XMMatrixAffineTransformation) だが、WorldMatrix (描画時にも更新される派生値) は読まない
XMMATRIX WorldFromLocals(World& world, EntityID e)
{
    XMMATRIX m = XMMatrixIdentity();
    EntityID cur = e;
    for (int depth = 0; depth < kMaxHierarchyDepth && !cur.IsNull(); ++depth) {
        const auto* lt = world.GetComponent<LocalTransform>(cur);
        if (lt == nullptr) {
            break;
        }
        const XMMATRIX local = XMMatrixAffineTransformation(XMLoadFloat3(&lt->scale), XMVectorZero(),
                                                            XMLoadFloat4(&lt->rotation), XMLoadFloat3(&lt->position));
        m = m * local;
        const auto* h = world.GetComponent<HierarchyComponent>(cur);
        cur = (h != nullptr) ? h->parent : kNullEntity;
    }
    return m;
}

struct Row {
    EntityID entity;
};

} // namespace

uint32_t UpdateSpringFollow(World& world, float dt)
{
    std::vector<Row> rows;
    const ComponentTypeId req[] = { SpringFollowComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            rows.push_back({ arch.EntityAt(row) });
        }
    });
    if (rows.empty()) {
        return 0;
    }
    // ★index 昇順で回す: 追従の連鎖 (A が B を、B が C を追う) の結果が回す順で変わるので、順序を明示的な鍵で決める
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.entity.index < b.entity.index; });
    uint32_t moved = 0;
    for (const Row& r : rows) {
        auto* sf = world.GetComponent<SpringFollowComponent>(r.entity);
        auto* lt = world.GetComponent<LocalTransform>(r.entity);
        if (sf == nullptr || lt == nullptr || sf->target.IsNull() || !world.IsAlive(sf->target)
            || sf->target == r.entity) {
            continue;
        }
        // 目標の world 姿勢 (target の空間の offset を含む) を、このエンティティの親の空間へ直す
        XMVECTOR ts = XMVectorZero();
        XMVECTOR tr = XMQuaternionIdentity();
        XMVECTOR tp = XMVectorZero();
        if (!XMMatrixDecompose(&ts, &tr, &tp, WorldFromLocals(world, sf->target))) {
            continue;
        }
        XMVECTOR goalPos = XMVectorAdd(tp, XMVector3Rotate(XMLoadFloat3(&sf->offset), tr));
        XMVECTOR goalRot = tr;
        const auto* h = world.GetComponent<HierarchyComponent>(r.entity);
        if (h != nullptr && !h->parent.IsNull()) {
            XMVECTOR ps = XMVectorZero();
            XMVECTOR pr = XMQuaternionIdentity();
            XMVECTOR pp = XMVectorZero();
            const XMMATRIX parentWorld = WorldFromLocals(world, h->parent);
            if (XMMatrixDecompose(&ps, &pr, &pp, parentWorld)) {
                goalPos = XMVector3TransformCoord(goalPos, XMMatrixInverse(nullptr, parentWorld));
                goalRot = XMQuaternionMultiply(goalRot, XMQuaternionInverse(pr));
            }
        }
        XMFLOAT3 goal;
        XMStoreFloat3(&goal, goalPos);
        if (sf->initialized == 0) {
            lt->position = goal; // 初回は瞬間移動 (原点から飛んでくる絵を出さない)
            if (sf->rotationFrequencyHz > 0.0f) {
                XMStoreFloat4(&lt->rotation, XMQuaternionNormalize(goalRot));
            }
            sf->velocity = { 0.0f, 0.0f, 0.0f };
            sf->initialized = 1;
            ++moved;
            continue;
        }
        const float freq = std::isfinite(sf->frequencyHz) ? std::max(0.0f, sf->frequencyHz) : 0.0f;
        const float zeta = std::isfinite(sf->dampingRatio) ? std::max(0.0f, sf->dampingRatio) : 1.0f;
        MyeSpringStep(lt->position.x, sf->velocity.x, goal.x, freq, zeta, dt);
        MyeSpringStep(lt->position.y, sf->velocity.y, goal.y, freq, zeta, dt);
        MyeSpringStep(lt->position.z, sf->velocity.z, goal.z, freq, zeta, dt);
        if (sf->rotationFrequencyHz > 0.0f && std::isfinite(sf->rotationFrequencyHz)) {
            // 指数的に寄せる: 1 - exp(-2π f dt) を 2 の冪 (MyeFeelExp2) で書く (CRT の exp を sim に入れない)
            const float k = 1.0f - MyeFeelExp2(-2.0f * kMyeFeelPi * sf->rotationFrequencyHz * dt / kLn2);
            const XMVECTOR cur = XMQuaternionNormalize(XMLoadFloat4(&lt->rotation));
            XMStoreFloat4(&lt->rotation, XMQuaternionNormalize(XMQuaternionSlerp(cur, XMQuaternionNormalize(goalRot), k)));
        }
        ++moved;
    }
    return moved;
}

} // namespace mye
