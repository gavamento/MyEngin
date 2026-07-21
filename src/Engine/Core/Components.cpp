#include "Engine/Core/Components.h"

#include <cstddef> // offsetof

#include "Engine/Core/World.h"

namespace mye {

bool IsEntityActive(World& world, EntityID e)
{
    const auto* a = world.GetComponent<ActiveComponent>(e);
    return !a || a->enabled != 0;
}

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
        MYE_FIELD(LightComponent, type, Int32),
        MYE_FIELD(LightComponent, range, Float),
        MYE_FIELD(LightComponent, spotInnerDeg, Float),
        MYE_FIELD(LightComponent, spotOuterDeg, Float),
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

    // M28a: height / friction を末尾 append (フィールド順変更なし = シーン互換維持。
    // 既存シーンは欠損フィールドをデフォルト値でロードする)
    RegisterComponent<ColliderComponent>("Collider", {
        MYE_FIELD(ColliderComponent, shape, Int32),
        MYE_FIELD(ColliderComponent, radius, Float),
        MYE_FIELD(ColliderComponent, halfExtents, Float3),
        MYE_FIELD(ColliderComponent, isTrigger, Int32),
        MYE_FIELD(ColliderComponent, height, Float),
        MYE_FIELD(ColliderComponent, friction, Float),
    });

    // M10: 末尾追加 (TypeId 順を壊さない)。無ければ有効なので既存シーンは不変
    RegisterComponent<ActiveComponent>("Active", {
        MYE_FIELD(ActiveComponent, enabled, Int32),
    });

    // M13: プレハブタグ。純データ (どのシステムにも参加しない = sim 非影響)。
    // kComponentHidden で Inspector の Add/一覧から隠すが、シリアライズ+ハッシュはされる。
    // 無ければ通常エンティティなので既存シーンのハッシュは不変 (ReplayFile bump 不要)
    RegisterComponent<PrefabInstanceComponent>("PrefabInstance", {
        MYE_FIELD_FLAGS(PrefabInstanceComponent, prefabHash, UInt64, kFieldReadOnly),
    }, kComponentHidden);

    RegisterComponent<PrefabLinkComponent>("PrefabLink", {
        MYE_FIELD_FLAGS(PrefabLinkComponent, localId, UInt64, kFieldReadOnly),
    }, kComponentHidden);

    // M14: アニメータ。無ければ何もしない (opt-in) ので既存シーンは不変
    RegisterComponent<AnimatorComponent>("Animator", {
        MYE_FIELD(AnimatorComponent, clip, AssetRef),
        MYE_FIELD_FLAGS(AnimatorComponent, timeTicks, Int32, kFieldReadOnly),
        MYE_FIELD(AnimatorComponent, speed, Int32),
        MYE_FIELD(AnimatorComponent, loop, Int32),
        MYE_FIELD(AnimatorComponent, playing, Int32),
    });

    // M18: スケルタルスキニング。ポーズは描画専用なので **kComponentNoHash** (既存シーン不変)。
    // opt-in (無ければ通常メッシュ描画) なので TypeId append (=14) だけで bump 不要
    RegisterComponent<SkinnedMeshComponent>("SkinnedMesh", {
        MYE_FIELD(SkinnedMeshComponent, model, AssetRef),
        MYE_FIELD(SkinnedMeshComponent, clip, Int32),
        MYE_FIELD_FLAGS(SkinnedMeshComponent, timeTicks, Int32, kFieldReadOnly),
        MYE_FIELD(SkinnedMeshComponent, playing, Int32),
    }, kComponentNoHash);

    // M20: 剛体。velocity は積分される sim 状態なので **hash 対象** (kComponentNoHash を付けない)。
    // opt-in (無ければ物理非関与) なので TypeId append (=15) だけで既存シーンは不変 → bump 不要
    RegisterComponent<RigidbodyComponent>("Rigidbody", {
        MYE_FIELD(RigidbodyComponent, velocity, Float3),
        MYE_FIELD(RigidbodyComponent, mass, Float),
        MYE_FIELD(RigidbodyComponent, linearDamping, Float),
        MYE_FIELD(RigidbodyComponent, restitution, Float),
        MYE_FIELD(RigidbodyComponent, gravityScale, Float),
        MYE_FIELD(RigidbodyComponent, isKinematic, Int32),
    });

    // M21: ゲーム内 UI。描画専用なので **kComponentNoHash** (既存シーンのハッシュ不変)。
    // serialize はされる (UI をシーン保存/Inspector 編集可能)。opt-in で TypeId append (=16) のみ
    RegisterComponent<UIElementComponent>("UIElement", {
        MYE_FIELD(UIElementComponent, kind, Int32),
        MYE_FIELD(UIElementComponent, anchor, Int32),
        MYE_FIELD(UIElementComponent, x, Float),
        MYE_FIELD(UIElementComponent, y, Float),
        MYE_FIELD(UIElementComponent, w, Float),
        MYE_FIELD(UIElementComponent, h, Float),
        MYE_FIELD(UIElementComponent, color, Color),
        MYE_FIELD(UIElementComponent, texture, AssetRef),
        MYE_FIELD(UIElementComponent, fontScale, Float),
        MYE_FIELD(UIElementComponent, order, Int32),
        MYE_FIELD(UIElementComponent, text, String64),
    }, kComponentNoHash);

    // M22: Animator Controller。LocalTransform を駆動するので **hash 対象** (kComponentNoHash 無し)。
    // opt-in (無ければ no-op) で TypeId append (=17) のみ → 既存シーン不変 = bump 不要。
    // params[4] は配列なので手動 FieldDesc で各要素を Int32 登録する (hash + serialize + Inspector)
    RegisterComponent<AnimatorControllerComponent>("AnimatorController", {
        MYE_FIELD(AnimatorControllerComponent, controller, AssetRef),
        MYE_FIELD_FLAGS(AnimatorControllerComponent, currentState, Int32, kFieldReadOnly),
        MYE_FIELD_FLAGS(AnimatorControllerComponent, stateTimeTicks, Int32, kFieldReadOnly),
        MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionTo, Int32, kFieldReadOnly),
        MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionTick, Int32, kFieldReadOnly),
        MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionDuration, Int32, kFieldReadOnly),
        MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionToTime, Int32, kFieldReadOnly),
        FieldDesc{ "param0", FieldType::Int32,
                   static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 0 * sizeof(int32_t)),
                   kFieldNone },
        FieldDesc{ "param1", FieldType::Int32,
                   static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 1 * sizeof(int32_t)),
                   kFieldNone },
        FieldDesc{ "param2", FieldType::Int32,
                   static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 2 * sizeof(int32_t)),
                   kFieldNone },
        FieldDesc{ "param3", FieldType::Int32,
                   static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 3 * sizeof(int32_t)),
                   kFieldNone },
    });
}

} // namespace mye
