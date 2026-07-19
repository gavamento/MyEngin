#include "Engine/Core/Components.h"

namespace mye {

void RegisterBuiltinComponents()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    // 登録順 = TypeId。順序を変えるとシーン互換とリプレイ互換が壊れるため、
    // 追加は必ず末尾に行うこと
    RegisterComponent<NameComponent>("Name", {
        MYE_FIELD(NameComponent, value, String64),
    });

    RegisterComponent<LocalTransform>("LocalTransform", {
        MYE_FIELD(LocalTransform, position, Float3),
        MYE_FIELD(LocalTransform, rotation, Quat),
        MYE_FIELD(LocalTransform, scale, Float3),
    });

    RegisterComponent<WorldMatrixComponent>("WorldMatrix", {
        MYE_FIELD_FLAGS(WorldMatrixComponent, value, Float4x4, kFieldReadOnly | kFieldNoSerialize),
    }, kComponentNoSerialize | kComponentHidden);

    RegisterComponent<HierarchyComponent>("Hierarchy", {
        MYE_FIELD_FLAGS(HierarchyComponent, parent, EntityRef, kFieldReadOnly | kFieldNoSerialize),
        MYE_FIELD_FLAGS(HierarchyComponent, firstChild, EntityRef, kFieldHidden | kFieldNoSerialize),
        MYE_FIELD_FLAGS(HierarchyComponent, nextSibling, EntityRef, kFieldHidden | kFieldNoSerialize),
        MYE_FIELD_FLAGS(HierarchyComponent, depth, UInt32, kFieldReadOnly | kFieldNoSerialize),
    }, kComponentNoSerialize | kComponentHidden); // 親子関係はシーンシリアライザが fileId で特別扱い

    RegisterComponent<MeshRendererComponent>("MeshRenderer", {
        MYE_FIELD(MeshRendererComponent, mesh, AssetRef),
        MYE_FIELD(MeshRendererComponent, material, AssetRef),
    });

    RegisterComponent<CameraComponent>("Camera", {
        MYE_FIELD(CameraComponent, fovYDeg, Float),
        MYE_FIELD(CameraComponent, nearZ, Float),
        MYE_FIELD(CameraComponent, farZ, Float),
        MYE_FIELD(CameraComponent, isPrimary, Int32),
    });

    RegisterComponent<LightComponent>("Light", {
        MYE_FIELD(LightComponent, color, Float3),
        MYE_FIELD(LightComponent, intensity, Float),
        MYE_FIELD(LightComponent, ambient, Float3),
    });

    RegisterComponent<FileIdComponent>("FileId", {
        MYE_FIELD_FLAGS(FileIdComponent, value, UInt64, kFieldReadOnly | kFieldNoSerialize),
    }, kComponentNoSerialize | kComponentHidden); // シリアライザが "fileId" として特別扱い

    RegisterComponent<ParticleEmitterComponent>("ParticleEmitter", {
        MYE_FIELD(ParticleEmitterComponent, rate, Float),
        MYE_FIELD(ParticleEmitterComponent, shape, Int32),
        MYE_FIELD(ParticleEmitterComponent, shapeRadius, Float),
        MYE_FIELD(ParticleEmitterComponent, coneAngleDeg, Float),
        MYE_FIELD(ParticleEmitterComponent, boxExtents, Float3),
        MYE_FIELD(ParticleEmitterComponent, lifetimeMin, Float),
        MYE_FIELD(ParticleEmitterComponent, lifetimeMax, Float),
        MYE_FIELD(ParticleEmitterComponent, speedMin, Float),
        MYE_FIELD(ParticleEmitterComponent, speedMax, Float),
        MYE_FIELD(ParticleEmitterComponent, sizeMin, Float),
        MYE_FIELD(ParticleEmitterComponent, sizeMax, Float),
        MYE_FIELD(ParticleEmitterComponent, colorBegin, Color),
        MYE_FIELD(ParticleEmitterComponent, colorEnd, Color),
        MYE_FIELD(ParticleEmitterComponent, sizeEndScale, Float),
        MYE_FIELD(ParticleEmitterComponent, gravity, Float3),
        MYE_FIELD(ParticleEmitterComponent, wind, Float3),
        MYE_FIELD(ParticleEmitterComponent, turbulence, Float),
        MYE_FIELD(ParticleEmitterComponent, blendMode, Int32),
        MYE_FIELD(ParticleEmitterComponent, seed, UInt32),
        MYE_FIELD(ParticleEmitterComponent, maxParticles, Int32),
    });

    RegisterComponent<ColliderComponent>("Collider", {
        MYE_FIELD(ColliderComponent, shape, Int32),
        MYE_FIELD(ColliderComponent, radius, Float),
        MYE_FIELD(ColliderComponent, halfExtents, Float3),
        MYE_FIELD(ColliderComponent, isTrigger, Int32),
    });
}

} // namespace mye
