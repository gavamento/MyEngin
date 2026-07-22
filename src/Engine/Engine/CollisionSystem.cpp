#include "Engine/Engine/CollisionSystem.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Physics/Broadphase.h"
#include "Engine/Engine/Physics/Shapes.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Engine/Script/ScriptHost.h"

using namespace DirectX;

namespace mye {
namespace {

// 判定は Physics/Shapes.cpp に統合 (M28a)。sphere/box/capsule + 回転 (OBB) 対応。
// 境界 (ちょうど接触 = 距離が厳密に一致) はソリッド判定と同じ「重なりのみ true」に統一
// (M7 は境界含みだったが float 同値の測度ゼロ事象のため実挙動差なし)
struct Body {
    EntityID entity;
    ShapePose pose;
    int32_t isTrigger = 0;
    int32_t layer = 0;           // M36a: 衝突レイヤー (Collider から複製)
    uint32_t mask = 0xFFFFFFFFu;
};

// key 昇順の SolidContact 列から法線を引く (無ければ +Y)
void FindContactNormal(const std::vector<SolidContact>& contacts, uint64_t key, float& nx,
                       float& ny, float& nz)
{
    auto it = std::lower_bound(contacts.begin(), contacts.end(), key,
                               [](const SolidContact& c, uint64_t k) { return c.key < k; });
    if (it != contacts.end() && it->key == key) {
        nx = it->nx;
        ny = it->ny;
        nz = it->nz;
    } else {
        nx = 0;
        ny = 1;
        nz = 0; // Exit ペア (今 tick に接触なし) は前 tick 法線を持たないため既定値
    }
}

} // namespace

void CollisionSystem::Update(World& world, ScriptHost* scripts, ManagedHost* managed,
                             const std::vector<SolidContact>* solidContacts)
{
    // ---- 収集 (index 昇順 = 決定論) ----
    std::vector<Body> bodies;
    const ComponentTypeId req[] = { ColliderComponent::sTypeId, WorldMatrixComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ci = arch.FindTypeIndex(ColliderComponent::sTypeId);
        const int wi = arch.FindTypeIndex(WorldMatrixComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            if (!IsEntityActive(world, arch.EntityAt(row))) {
                continue; // 無効エンティティのコライダーは判定から除外 (M10)
            }
            const auto* col = static_cast<const ColliderComponent*>(arch.GetPtr(ci, row));
            const auto* wm = static_cast<const WorldMatrixComponent*>(arch.GetPtr(wi, row));
            bodies.push_back({ arch.EntityAt(row), shapes::MakePoseFromMatrix(*col, wm->value),
                               col->isTrigger, col->layer, col->mask });
        }
    });
    std::sort(bodies.begin(), bodies.end(),
              [](const Body& a, const Body& b) { return a.entity.index < b.entity.index; });

    // ---- トリガー: ブロードフェーズ候補 (M28d) → 重なり判定 → 現 tick のペア集合 ----
    // M28c: ペアの少なくとも片方が isTrigger のものだけがトリガーイベント対象
    // (ソリッド同士の接触は PhysicsSystem 由来の OnCollision 系に移管)
    std::vector<uint64_t> pairs;
    {
        std::vector<BroadphaseEntry> entries;
        entries.reserve(bodies.size());
        for (size_t i = 0; i < bodies.size(); ++i) {
            BroadphaseEntry e;
            e.id = static_cast<uint32_t>(i);
            shapes::ComputeAabb(bodies[i].pose, e.minX, e.minY, e.minZ, e.maxX, e.maxY, e.maxZ);
            entries.push_back(e); // margin 0 (静止判定なので現在位置の AABB で十分)
        }
        std::vector<uint64_t> candidates;
        ComputeCandidatePairs(entries, candidates);
        for (const uint64_t key : candidates) {
            const Body& a = bodies[static_cast<size_t>(key >> 32)];
            const Body& b = bodies[static_cast<size_t>(key & 0xFFFFFFFFu)];
            if (a.isTrigger == 0 && b.isTrigger == 0) {
                continue;
            }
            if (!shapes::CanCollide(a.layer, a.mask, b.layer, b.mask)) {
                continue; // M36a: レイヤー非マッチはトリガーイベントも出さない
            }
            if (shapes::Overlap(a.pose, b.pose)) {
                pairs.push_back((static_cast<uint64_t>(a.entity.index) << 32) | b.entity.index);
            }
        }
    }
    std::sort(pairs.begin(), pairs.end());

    // ---- トリガー差分 → enter / exit (昇順配信 = 決定論) ----
    trigEnter_.clear();
    trigExit_.clear();
    std::set_difference(pairs.begin(), pairs.end(), prevPairs_.begin(), prevPairs_.end(),
                        std::back_inserter(trigEnter_));
    std::set_difference(prevPairs_.begin(), prevPairs_.end(), pairs.begin(), pairs.end(),
                        std::back_inserter(trigExit_));
    prevPairs_ = std::move(pairs);

    // ---- ソリッド差分 → enter / stay / exit (M28c) ----
    solidEnter_.clear();
    solidStay_.clear();
    solidExit_.clear();
    static const std::vector<SolidContact> kNoContacts;
    const std::vector<SolidContact>& solid = solidContacts ? *solidContacts : kNoContacts;
    std::vector<uint64_t> solidKeys;
    solidKeys.reserve(solid.size());
    for (const SolidContact& c : solid) {
        solidKeys.push_back(c.key); // PhysicsSystem 出力は key 昇順
    }
    std::set_difference(solidKeys.begin(), solidKeys.end(), prevSolidPairs_.begin(),
                        prevSolidPairs_.end(), std::back_inserter(solidEnter_));
    std::set_intersection(solidKeys.begin(), solidKeys.end(), prevSolidPairs_.begin(),
                          prevSolidPairs_.end(), std::back_inserter(solidStay_));
    std::set_difference(prevSolidPairs_.begin(), prevSolidPairs_.end(), solidKeys.begin(),
                        solidKeys.end(), std::back_inserter(solidExit_));
    prevSolidPairs_ = std::move(solidKeys);

    if (!scripts && !managed) {
        return;
    }
    auto resolve = [&world](uint32_t index) {
        // index から現世代のハンドルを引く (死んでいれば null)
        for (const auto& arch : world.Archetypes()) {
            for (uint32_t row = 0; row < arch->Count(); ++row) {
                if (arch->EntityAt(row).index == index) {
                    return arch->EntityAt(row);
                }
            }
        }
        return kNullEntity;
    };

    // ---- トリガー配信 (enter → exit、各 key 昇順) ----
    auto dispatchTrigger = [&](const std::vector<uint64_t>& list, bool enter) {
        for (uint64_t key : list) {
            const EntityID a = resolve(static_cast<uint32_t>(key >> 32));
            const EntityID b = resolve(static_cast<uint32_t>(key & 0xFFFFFFFFu));
            if (!a.IsNull()) {
                if (scripts) { scripts->DispatchTrigger(a, b, enter); }
                if (managed) { managed->DispatchTrigger(a, b, enter); }
            }
            if (!b.IsNull()) {
                if (scripts) { scripts->DispatchTrigger(b, a, enter); }
                if (managed) { managed->DispatchTrigger(b, a, enter); }
            }
        }
    };
    dispatchTrigger(trigEnter_, true);
    dispatchTrigger(trigExit_, false);

    // ---- ソリッド配信 (enter → stay → exit、各 key 昇順)。kind: 0=enter 1=stay 2=exit ----
    // 法線は「相手→自分」方向で渡す (SolidContact.n は大 index→小 index = 小側の自分向き)
    auto dispatchCollision = [&](const std::vector<uint64_t>& list, int kind) {
        for (uint64_t key : list) {
            const EntityID a = resolve(static_cast<uint32_t>(key >> 32));
            const EntityID b = resolve(static_cast<uint32_t>(key & 0xFFFFFFFFu));
            float nx, ny, nz;
            FindContactNormal(solid, key, nx, ny, nz);
            if (!a.IsNull()) {
                if (scripts) { scripts->DispatchCollision(a, b, kind, { nx, ny, nz }); }
                if (managed) { managed->DispatchCollision(a, b, kind, { nx, ny, nz }); }
            }
            if (!b.IsNull()) {
                if (scripts) { scripts->DispatchCollision(b, a, kind, { -nx, -ny, -nz }); }
                if (managed) { managed->DispatchCollision(b, a, kind, { -nx, -ny, -nz }); }
            }
        }
    };
    dispatchCollision(solidEnter_, 0);
    dispatchCollision(solidStay_, 1);
    dispatchCollision(solidExit_, 2);
}

} // namespace mye
