//====================================================================================
//                          SpriteShapeCollider.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          SpriteShape2D の輪郭を静的な辺の箱として物理へ供給する実装
//====================================================================================
#include "Engine/Engine/Physics/Collider/SpriteShapeCollider.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Physics/Rigid/Shapes.h"
#include "Engine/Engine/Rendering/Sprite2D/SpriteShape.h"

using namespace DirectX;

namespace mye {
namespace {

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

void FnvMix(uint64_t& h, uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        h = (h ^ static_cast<uint64_t>((v >> (8 * i)) & 0xFFu)) * kFnvPrime;
    }
}

// 輪郭を決める入力 (点列・閉じる・滑らか) の内容ハッシュ。浮動小数はビット列で混ぜる
uint64_t ContentHash(const SpriteShape2DComponent& s, int count)
{
    uint64_t h = kFnvOffset;
    FnvMix(h, static_cast<uint32_t>(count));
    FnvMix(h, (s.closed ? 1u : 0u) | (s.smooth ? 2u : 0u));
    for (int i = 0; i < count; ++i) {
        uint32_t bits[2];
        std::memcpy(&bits[0], &s.points[i].x, sizeof(uint32_t));
        std::memcpy(&bits[1], &s.points[i].y, sizeof(uint32_t));
        FnvMix(h, bits[0]);
        FnvMix(h, bits[1]);
    }
    return h;
}

} // namespace

void SpriteShapeColliders::Collect(World& world, std::vector<StaticShape>& out)
{
    CollectImpl(world, nullptr, out);
}

void SpriteShapeColliders::CollectWithin(World& world, MotionRegions& regions, std::vector<StaticShape>& out)
{
    CollectImpl(world, &regions, out);
}

void SpriteShapeColliders::CollectImpl(World& world, MotionRegions* regions, std::vector<StaticShape>& out)
{
    lastBoxCount_ = 0;
    struct Live {
        EntityID entity;
        SpriteShape2DComponent shape;
    };
    std::vector<Live> live;
    const ComponentTypeId req[] = { SpriteShape2DComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int si = arch.FindTypeIndex(SpriteShape2DComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (!IsEntityActive(world, e)) {
                continue;
            }
            const auto* s = static_cast<const SpriteShape2DComponent*>(arch.GetPtr(si, row));
            if (s->collider) {
                live.push_back({ e, *s });
            }
        }
    });
    // アーキタイプの走査順に依存しない: 持ち主の index 昇順 (明示キー)
    std::sort(live.begin(), live.end(), [](const Live& a, const Live& b) { return a.entity.index < b.entity.index; });

    // 生きていない / collider を外した持ち主のキャッシュを捨てる (live は index 昇順なので鍵の並びも昇順)
    std::vector<EntityKey> liveKeys;
    liveKeys.reserve(live.size());
    for (const Live& l : live) {
        liveKeys.push_back(EntityKey{ l.entity.index, l.entity.generation });
    }
    for (auto it = cache_.begin(); it != cache_.end();) {
        it = std::binary_search(liveKeys.begin(), liveKeys.end(), it->first) ? std::next(it) : cache_.erase(it);
    }

    for (const Live& l : live) {
        const size_t before = out.size();
        CollectEntity(world, l.entity, l.shape, regions, out);
        lastBoxCount_ += out.size() - before;
    }
}

void SpriteShapeColliders::AppendEntityShapes(World& world, EntityID e, std::vector<StaticShape>& out)
{
    const auto* shape = world.GetComponent<SpriteShape2DComponent>(e);
    if (shape == nullptr || !shape->collider || !IsEntityActive(world, e)
        || world.GetComponent<LocalTransform>(e) == nullptr) {
        return;
    }
    CollectEntity(world, e, *shape, nullptr, out);
}

void SpriteShapeColliders::CollectEntity(World& world, EntityID e, const SpriteShape2DComponent& shape,
                                         MotionRegions* regions, std::vector<StaticShape>& out)
{
    const int count = std::clamp(shape.pointCount, 0, kSpriteShape2DMaxPoints);
    EntityCache& ec = cache_[EntityKey{ e.index, e.generation }];
    const uint64_t contentHash = ContentHash(shape, count);
    if (!ec.valid || ec.contentHash != contentHash) {
        spriteshape::EvaluateSpline(shape.points, count, shape.closed, shape.smooth, ec.polyline);
        ec.closed = spriteshape::IsClosedOutline(count, shape.closed);
        ec.contentHash = contentHash;
        ec.valid = true;
    }
    // コンポーネントの値は毎 tick 写す (Inspector での変更が次の tick から効く)
    ec.col = ColliderComponent{};
    ec.col.shape = collidershape::kBox;
    ec.col.isTrigger = false;
    ec.col.oneWay = shape.oneWay;

    const size_t n = ec.polyline.size();
    if (n < 2) {
        return;
    }

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    ComposeEntityWorldPose(world, e, px, py, pz, qx, qy, qz, qw);
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    ComposeEntityWorldScale(world, e, sx, sy, sz);
    // 基底は単位寸法の箱の MakePose から取る (位置と回転だけを使う)
    ColliderComponent plain;
    plain.shape = collidershape::kBox;
    const ShapePose base = shapes::MakePose(plain, { px, py, pz }, { qx, qy, qz, qw }, { 1.0f, 1.0f, 1.0f });
    // 半厚は XY のうち大きいほうのスケール (球の半径と同じ「最大成分」規約)、z の厚みは z スケール
    const float radius = kSpriteShapeColliderHalfThickness * std::max(std::fabs(sx), std::fabs(sy));
    const float halfDepth = kSpriteShapeColliderDepth * 0.5f * std::fabs(sz);

    const size_t segments = ec.closed ? n : n - 1;
    std::vector<ShapePose> boxes;
    boxes.reserve(segments);
    for (size_t i = 0; i < segments; ++i) {
        const XMFLOAT2& a = ec.polyline[i];
        const XMFLOAT2& b = ec.polyline[(i + 1) % n];
        // スケールは符号つきで点へ掛ける (負のスケールの鏡映を失わない)
        ShapePose box;
        if (shapes::MakeSegmentBox2D({ a.x * sx, a.y * sy }, { b.x * sx, b.y * sy }, radius, halfDepth, base, box)) {
            boxes.push_back(box);
        }
    }
    if (boxes.empty()) {
        return;
    }

    if (regions != nullptr) {
        float minX = 0.0f, minY = 0.0f, minZ = 0.0f, maxX = 0.0f, maxY = 0.0f, maxZ = 0.0f;
        bool first = true;
        for (const ShapePose& box : boxes) {
            float bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ;
            shapes::ComputeAabb(box, bMinX, bMinY, bMinZ, bMaxX, bMaxY, bMaxZ);
            if (first) {
                minX = bMinX; minY = bMinY; minZ = bMinZ; maxX = bMaxX; maxY = bMaxY; maxZ = bMaxZ;
                first = false;
            } else {
                minX = std::min(minX, bMinX); minY = std::min(minY, bMinY); minZ = std::min(minZ, bMinZ);
                maxX = std::max(maxX, bMaxX); maxY = std::max(maxY, bMaxY); maxZ = std::max(maxZ, bMaxZ);
            }
        }
        bool reached = false;
        for (const StaticRegion& r : regions->Get()) {
            if (r.maxX >= minX && r.minX <= maxX && r.maxY >= minY && r.minY <= maxY && r.maxZ >= minZ && r.minZ <= maxZ) {
                reached = true;
                break;
            }
        }
        if (!reached) {
            return;
        }
    }
    for (const ShapePose& box : boxes) {
        out.push_back(StaticShape{ e, box, &ec.col });
    }
}

} // namespace mye
