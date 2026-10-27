// 物理マテリアル・物理環境・形状量・質量の解決 (M59a2 / M59b)。ソルバの収集と ABI が共有する純関数で、
// 契約 (結合則・既定値・存在ゲート) は宣言側 PhysicsSystem.h のコメントが正本
#include "Engine/Engine/Physics/Rigid/PhysicsSystem.h"

#include <algorithm>
#include <cmath>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Physics/Collider/ConvexColliderLibrary.h" // M60f: convexcol::Resolve
#include "Engine/Engine/Physics/Collider/ConvexHull.h"            // M60f: 凸包の質量特性
#include "Engine/Engine/Physics/Rigid/PhysMatLibrary.h" // M59a2: physmat::Resolve (材料解決)
#include "Engine/Engine/Physics/Rigid/Shapes.h"

using namespace DirectX;

namespace mye {

// ==== 物理マテリアル解決 (M59a2) ====
// 宣言側 (PhysicsSystem.h) のコメントが契約の正本。ここは実装の注意のみ:
// Select* は fp 演算ゼロの値選択に保つこと — 未割当シーンのビット同一はそれだけで自明に立つ

float SelectFriction(const ColliderComponent& col, const PhysMat* mat)
{
    if ((col.materialOverrideBits & kPhysMatOverrideFriction) != 0u) {
        return col.friction;
    }
    return mat ? mat->dynamicFriction : col.friction;
}

float SelectStaticFriction(const ColliderComponent& col, const PhysMat* mat)
{
    if ((col.materialOverrideBits & kPhysMatOverrideFriction) != 0u) {
        return col.friction;
    }
    // ★材料なしは **col.friction** — SelectFriction と同じ式・同じ入力なので μs と μd が
    //   ビットまで一致し、ソルバの静止/動の分岐が従来の 1 本のクランプに畳まれる
    return mat ? mat->staticFriction : col.friction;
}

float SelectRollingResistance(const ColliderComponent& col, const PhysMat* mat)
{
    if ((col.materialOverrideBits & kPhysMatOverrideRolling) != 0u) {
        return 0.0f;
    }
    return mat ? mat->rollingResistance : 0.0f;
}

// M60d: 粘着力 [N]。**Collider 側に旧フィールドが無い**ので上書きビットも取らない —
// 「材料が無ければ 0」しか状態が無く、未割当シーンのビット同一が式の上で自明に立つ
float SelectAdhesion(const PhysMat* mat)
{
    return mat ? mat->adhesion : 0.0f;
}

// M65c: 音響 3 本。SelectAdhesion と同型 (旧フィールド無し = 上書きビット無し)
float SelectAcousticLoudness(const PhysMat* mat)
{
    return mat ? mat->acousticLoudness : 0.0f;
}

float SelectAcousticRadiusM(const PhysMat* mat)
{
    return mat ? mat->acousticRadiusM : 0.0f;
}

int32_t SelectAcousticTone(const PhysMat* mat)
{
    return mat ? mat->acousticTone : 0;
}

float SelectRestitution(const ColliderComponent* col, const RigidbodyComponent* rb,
                        const PhysMat* mat)
{
    // 既存フィールドの格納庫は Rigidbody 側 (静的コライダーは持たない = 従来どおり 0)
    const float legacy = rb ? rb->restitution : 0.0f;
    if (col && (col->materialOverrideBits & kPhysMatOverrideRestitution) != 0u) {
        return legacy;
    }
    return mat ? mat->restitution : legacy;
}

const PhysicsEnvironmentComponent* ResolvePhysicsEnvironment(World& world)
{
    // Skybox / Fog と同じ「entity.index 最小の active な 1 個」。アーキタイプの走査順に
    // 依存させないため、早期 return せず全件見てから最小 index を採る
    const PhysicsEnvironmentComponent* best = nullptr;
    uint32_t bestIndex = 0xFFFFFFFFu;
    const ComponentTypeId req[] = { PhysicsEnvironmentComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int ei = arch.FindTypeIndex(PhysicsEnvironmentComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestIndex || !IsEntityActive(world, e)) {
                continue;
            }
            bestIndex = e.index;
            best = static_cast<const PhysicsEnvironmentComponent*>(arch.GetPtr(ei, row));
        }
    });
    return best;
}

const WaterWaveComponent* ResolveActiveWaterWave(World& world, EntityID* outEntity)
{
    const WaterWaveComponent* best = nullptr;
    EntityID bestEntity = kNullEntity;
    uint32_t bestIndex = 0xFFFFFFFFu;
    const ComponentTypeId req[] = { WaterWaveComponent::sTypeId };
    world.ForEachArchetype(req, [&](Archetype& arch) {
        const int wi = arch.FindTypeIndex(WaterWaveComponent::sTypeId);
        for (uint32_t row = 0; row < arch.Count(); ++row) {
            const EntityID e = arch.EntityAt(row);
            if (e.index >= bestIndex || !IsEntityActive(world, e)) {
                continue;
            }
            const auto* wave = static_cast<const WaterWaveComponent*>(arch.GetPtr(wi, row));
            if (wave && wave->enabled) {
                bestIndex = e.index;
                best = wave;
                bestEntity = e;
            }
        }
    });
    if (outEntity) {
        *outEntity = bestEntity;
    }
    return best;
}

float MeanProjectedAreaWorld(const ColliderComponent* col, float sx, float sy, float sz)
{
    // 負スケールは絶対値 (ShapeVolumeWorld / shapes::ApplyScaledExtents と同一規約)
    sx = std::fabs(sx);
    sy = std::fabs(sy);
    sz = std::fabs(sz);
    // コライダー無し / 動的 mesh は慣性導出 (LocalInertiaDiag) と同じ「半径 0.5 の球」既定。
    // ここを 0 にすると「空力が黙って効かない」になるので、既定の代表形状へ落とす
    if (!col || col->shape == collidershape::kMesh) {
        return XM_PI * 0.25f;
    }
    switch (col->shape) {
    case collidershape::kSphere: { // 球 = 最大成分スケール。表面積 4 pi r^2 の 1/4 = pi r^2
        const float r = col->radius * std::max(sx, std::max(sy, sz));
        return XM_PI * r * r;
    }
    case collidershape::kBox: { // box = 成分別スケール。表面積 8(hx hy + hy hz + hz hx) の 1/4
        const float hx = col->halfExtents.x * sx;
        const float hy = col->halfExtents.y * sy;
        const float hz = col->halfExtents.z * sz;
        return 2.0f * (hx * hy + hy * hz + hz * hx);
    }
    case collidershape::kConvex: { // M60f: 凸包は**外接 AABB の箱で代用**する。
        // 厳密な表面積は非一様スケールで積分し直しになるうえ、向きを見る正しい
        // 面積分は M59c/M59d の面サンプリングが担当なので、ここは代表値で足りる
        const ConvexHullData* h = convexcol::Resolve(col->meshAsset);
        if (!h || !h->Valid()) {
            return XM_PI * 0.25f;
        }
        const float hx = (h->aabbMax.x - h->aabbMin.x) * 0.5f * sx;
        const float hy = (h->aabbMax.y - h->aabbMin.y) * 0.5f * sy;
        const float hz = (h->aabbMax.z - h->aabbMin.z) * 0.5f * sz;
        return 2.0f * (hx * hy + hy * hz + hz * hx);
    }
    default: { // capsule = 側面 2 pi r (2 halfSeg) + 両端の球面 4 pi r^2、の 1/4
        const float wr = col->radius * std::max(sx, sz);
        const float wh = col->height * 0.5f * sy;
        const float halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
        return XM_PI * wr * halfSeg + XM_PI * wr * wr;
    }
    }
}

float SubmergedFractionWorld(const ShapePose& pose, float planeY, float& outCentroidY)
{
    outCentroidY = pose.py;
    if (pose.shape == collidershape::kSphere) {
        // 球冠。中心を原点に取り t = planeY - 中心Y。t <= -R は完全に水面上、t >= R は完全没水。
        // V(t) = pi(R^2 t - t^3/3 + 2R^3/3)、M(t) = pi(R^2 t^2/2 - t^4/4 - R^4/4)
        // (どちらも多項式 — 球冠の体積・重心に三角関数は要らない)
        const float R = pose.radius;
        if (R <= 0.0f) {
            return 0.0f;
        }
        const float t = planeY - pose.py;
        if (t <= -R) {
            return 0.0f;
        }
        if (t >= R) {
            return 1.0f; // 完全没水。重心は中心のまま
        }
        const float R2 = R * R;
        const float t2 = t * t;
        const float v = R2 * t - t2 * t / 3.0f + 2.0f * R2 * R / 3.0f; // pi は約分で消える
        if (v <= 0.0f) {
            return 0.0f;
        }
        const float m = R2 * t2 * 0.5f - t2 * t2 * 0.25f - R2 * R2 * 0.25f;
        outCentroidY = pose.py + m / v;
        return v / (4.0f * R2 * R / 3.0f); // V_cap / V_sphere
    }
    // box / capsule: 保守 AABB の高さ比近似。傾いた箱の没水重心が水平にずれるのを
    // 拾えない (= 復原モーメントが出ない) のが v1 の既知の限界
    float minX, minY, minZ, maxX, maxY, maxZ;
    shapes::ComputeAabb(pose, minX, minY, minZ, maxX, maxY, maxZ);
    const float h = maxY - minY;
    if (h <= 0.0f || planeY <= minY) {
        return 0.0f;
    }
    const float top = (planeY < maxY) ? planeY : maxY;
    outCentroidY = (minY + top) * 0.5f;
    return (top - minY) / h;
}

float ShapeVolumeWorld(const ColliderComponent& col, float sx, float sy, float sz)
{
    // MakePose 経由と同じく負スケールは絶対値 (shapes::ApplyScaledExtents 参照)
    sx = std::fabs(sx);
    sy = std::fabs(sy);
    sz = std::fabs(sz);
    switch (col.shape) {
    case collidershape::kSphere: { // 球 = 最大成分スケール
        const float r = col.radius * std::max(sx, std::max(sy, sz));
        return (4.0f / 3.0f) * XM_PI * r * r * r;
    }
    case collidershape::kBox: // box = 成分別スケール
        return 8.0f * (col.halfExtents.x * sx) * (col.halfExtents.y * sy)
             * (col.halfExtents.z * sz);
    case collidershape::kCapsule: { // capsule = 円柱 + 両端半球 (halfSeg 規約は ApplyScaledExtents と同一)
        const float wr = col.radius * std::max(sx, sz);
        const float wh = col.height * 0.5f * sy;
        const float halfSeg = (wh > wr) ? (wh - wr) : 0.0f;
        return XM_PI * wr * wr * (2.0f * halfSeg) + (4.0f / 3.0f) * XM_PI * wr * wr * wr;
    }
    case collidershape::kConvex: { // M60f: 凸包。生成時に積分した体積 (密度 1) に線形写像の行列式を掛ける
        const ConvexHullData* h = convexcol::Resolve(col.meshAsset);
        return h ? h->volume * sx * sy * sz : 0.0f;
    }
    default: // mesh (shape=3) は体積を定義しない → 呼び出し側が mass へフォールバック
        return 0.0f;
    }
}

float ResolveBodyMass(const RigidbodyComponent& rb, const ColliderComponent* col,
                      const PhysMat* mat, float sx, float sy, float sz)
{
    const float base = (rb.mass > 0.0f) ? rb.mass : 1.0f; // 従来の既定 (M20)
    if (!rb.useDensity || !mat || !col || col->shape == collidershape::kMesh) {
        return base; // 未割当シーンは常にここ = 従来と同一式
    }
    const float m = mat->density * ShapeVolumeWorld(*col, sx, sy, sz);
    return (m > 0.0f) ? m : base; // 体積 0 (半径 0 等) をゼロ除算にしない
}

float EffectiveMassWorld(World& world, EntityID e, const RigidbodyComponent& rb)
{
    const auto* col = world.GetComponent<ColliderComponent>(e);
    const PhysMat* mat = col ? physmat::Resolve(col->physMaterial) : nullptr;
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
    if (const auto* lt = world.GetComponent<LocalTransform>(e)) {
        sx = lt->scale.x;
        sy = lt->scale.y;
        sz = lt->scale.z;
    }
    return ResolveBodyMass(rb, col, mat, sx, sy, sz);
}

} // namespace mye
