//====================================================================================
//                          Camera2DFollow.cpp
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          2D カメラの追従の実装
//====================================================================================
#include "Engine/Engine/Rendering/Camera2DFollow.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"
#include "Engine/Engine/Tilemap/TilemapAssets.h"
#include "Engine/Engine/Tilemap/TilemapSystem.h"

namespace mye {
namespace {

// 先読みの向きを決める速度の下限 (m/s)。これ未満は静止扱い (0 付近で向きが振動しない)
constexpr float kLookAheadSpeedEpsilon = 0.01f;

// 対象の水平速度。Rigidbody があればそれ、無ければ CharacterController
float TargetVelocityX(World& world, EntityID target)
{
    if (const RigidbodyComponent* rb = world.GetComponent<RigidbodyComponent>(target)) {
        return rb->velocity.x;
    }
    if (const CharacterControllerComponent* cc = world.GetComponent<CharacterControllerComponent>(target)) {
        return cc->velocity.x;
    }
    return 0.0f;
}

// 境界の矩形 (ワールド XY)。boundsFromTilemap が引ければ衝突レイヤーの外接矩形、無ければ手書きの値
void ResolveBounds(World& world, const TilemapSystem* tiles, const Camera2DFollowComponent& f, float& minX,
                   float& minY, float& maxX, float& maxY)
{
    minX = f.boundsMin.x;
    minY = f.boundsMin.y;
    maxX = f.boundsMax.x;
    maxY = f.boundsMax.y;
    if (tiles != nullptr && !f.boundsFromTilemap.IsNull() && world.IsAlive(f.boundsFromTilemap)) {
        const TilemapAsset* map = TilemapSystem::AssetOf(world, f.boundsFromTilemap);
        int32_t cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;
        if (map != nullptr && tiles->GetCollisionBounds(world, f.boundsFromTilemap, cx0, cy0, cx1, cy1)) {
            const float halfW = 0.5f * map->cellSize[0];
            const float halfH = 0.5f * map->cellSize[1];
            const DirectX::XMFLOAT3 lo = TilemapSystem::CellToWorld(world, f.boundsFromTilemap, cx0, cy0);
            const DirectX::XMFLOAT3 hi = TilemapSystem::CellToWorld(world, f.boundsFromTilemap, cx1, cy1);
            minX = lo.x - halfW;
            minY = lo.y - halfH;
            maxX = hi.x + halfW;
            maxY = hi.y + halfH;
        }
    }
    if (minX > maxX) {
        std::swap(minX, maxX);
    }
    if (minY > maxY) {
        std::swap(minY, maxY);
    }
}

// 視野の半分 (halfView) を境界の内側に収める。視野が境界より大きい軸は境界の中心に置く
float ClampAxis(float pos, float boundMin, float boundMax, float halfView)
{
    const float lo = boundMin + halfView;
    const float hi = boundMax - halfView;
    if (lo > hi) {
        return 0.5f * (boundMin + boundMax);
    }
    return std::clamp(pos, lo, hi);
}

void UpdateOne(World& world, const TilemapSystem* tiles, EntityID e)
{
    Camera2DFollowComponent* f = world.GetComponent<Camera2DFollowComponent>(e);
    LocalTransform* lt = world.GetComponent<LocalTransform>(e);
    if (f == nullptr || lt == nullptr || f->target.IsNull() || f->target == e || !world.IsAlive(f->target)) {
        return;
    }
    float px = 0.0f, py = 0.0f, pz = 0.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    ComposeEntityWorldPose(world, f->target, px, py, pz, qx, qy, qz, qw);

    const float vx = TargetVelocityX(world, f->target);
    const float lookAhead = (vx > kLookAheadSpeedEpsilon ? 1.0f : (vx < -kLookAheadSpeedEpsilon ? -1.0f : 0.0f)) * f->lookAheadX;
    float desiredX = px + f->offset.x + lookAhead;
    float desiredY = py + f->offset.y;

    // デッドゾーン (軸ごと): 内側なら動かさない
    float camX = lt->position.x;
    float camY = lt->position.y;
    if (std::fabs(desiredX - camX) <= f->deadZone.x) {
        desiredX = camX;
    }
    if (std::fabs(desiredY - camY) <= f->deadZone.y) {
        desiredY = camY;
    }
    const float dampX = std::clamp(f->damping.x, 0.0f, 1.0f);
    const float dampY = std::clamp(f->damping.y, 0.0f, 1.0f);
    camX += (desiredX - camX) * dampX;
    camY += (desiredY - camY) * dampY;

    if (f->useBounds) {
        float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
        ResolveBounds(world, tiles, *f, minX, minY, maxX, maxY);
        // 画面寸法は sim に持ち込まない: 半幅半高は orthoSize と boundsAspect だけで決める (正射影でなければ 0)
        float halfH = 0.0f;
        if (const CameraComponent* cam = world.GetComponent<CameraComponent>(e)) {
            if (cam->orthographic) {
                halfH = std::max(cam->orthoSize, 0.0f);
            }
        }
        const float halfW = halfH * std::max(f->boundsAspect, 0.0f);
        camX = ClampAxis(camX, minX, maxX, halfW);
        camY = ClampAxis(camY, minY, maxY, halfH);
    }
    lt->position.x = camX;
    lt->position.y = camY;
}

} // namespace

void UpdateCamera2DFollow(World& world, const TilemapSystem* tiles)
{
    std::vector<EntityID> cameras;
    const ComponentTypeId req[] = { Camera2DFollowComponent::sTypeId, LocalTransform::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (IsEntityActive(world, e)) {
                cameras.push_back(e);
            }
        }
    });
    if (cameras.empty()) {
        return;
    }
    // 別のカメラを追うカメラがありうるので、アーキタイプの走査順に依らず entity.index 昇順に処理する
    std::sort(cameras.begin(), cameras.end(), [](EntityID a, EntityID b) {
        return a.index != b.index ? a.index < b.index : a.generation < b.generation;
    });
    for (const EntityID e : cameras) {
        UpdateOne(world, tiles, e);
    }
}

} // namespace mye
