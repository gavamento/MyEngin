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
        // M32a: ライフサイクル + 多点グラデーション + テクスチャ/フリップブック + ソフトパーティクル。
        // 末尾 append なので既存シーンは既定値ロードで挙動不変 (ハッシュは変わる → golden 再記録)。
        MYE_FIELD(ParticleEmitterComponent, playing, Int32),
        MYE_FIELD(ParticleEmitterComponent, durationTicks, Int32),
        MYE_FIELD(ParticleEmitterComponent, looping, Int32),
        MYE_FIELD(ParticleEmitterComponent, burstCount, Int32),
        MYE_FIELD(ParticleEmitterComponent, colorMid1, Color),
        MYE_FIELD(ParticleEmitterComponent, colorMidT1, Float),
        MYE_FIELD(ParticleEmitterComponent, colorMid2, Color),
        MYE_FIELD(ParticleEmitterComponent, colorMidT2, Float),
        MYE_FIELD(ParticleEmitterComponent, sizeMidScale, Float),
        MYE_FIELD(ParticleEmitterComponent, sizeMidT, Float),
        MYE_FIELD(ParticleEmitterComponent, texture, AssetRef),
        MYE_FIELD(ParticleEmitterComponent, flipTilesX, Int32),
        MYE_FIELD(ParticleEmitterComponent, flipTilesY, Int32),
        MYE_FIELD(ParticleEmitterComponent, flipCycles, Float),
        MYE_FIELD(ParticleEmitterComponent, softFadeDistance, Float),
    });

    // M28a: height / friction、M36a: layer / mask / meshAsset を末尾 append
    // (フィールド順変更なし = シーン互換維持。既存シーンは欠損フィールドをデフォルト値でロード。
    //  hash 対象フィールドの追加なので golden.rep は M36a で再記録済み)
    RegisterComponent<ColliderComponent>("Collider", {
        MYE_FIELD(ColliderComponent, shape, Int32),
        MYE_FIELD(ColliderComponent, radius, Float),
        MYE_FIELD(ColliderComponent, halfExtents, Float3),
        MYE_FIELD(ColliderComponent, isTrigger, Int32),
        MYE_FIELD(ColliderComponent, height, Float),
        MYE_FIELD(ColliderComponent, friction, Float),
        MYE_FIELD_TIP(ColliderComponent, layer, Int32, "collision layer 0..31"),
        MYE_FIELD_TIP(ColliderComponent, mask, UInt32, "layers this collider hits (bitmask)"),
        MYE_FIELD(ColliderComponent, meshAsset, AssetRef), // M41 予約 (現状未使用)
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
    // M28b: angularVelocity / angularDamping / freezeRotation を末尾 append。
    // angularVelocity は積分される sim 状態なので hash 対象 (velocity と同格)
    RegisterComponent<RigidbodyComponent>("Rigidbody", {
        MYE_FIELD(RigidbodyComponent, velocity, Float3),
        MYE_FIELD(RigidbodyComponent, mass, Float),
        MYE_FIELD(RigidbodyComponent, linearDamping, Float),
        MYE_FIELD(RigidbodyComponent, restitution, Float),
        MYE_FIELD(RigidbodyComponent, gravityScale, Float),
        MYE_FIELD(RigidbodyComponent, isKinematic, Int32),
        MYE_FIELD(RigidbodyComponent, angularVelocity, Float3),
        MYE_FIELD(RigidbodyComponent, angularDamping, Float),
        MYE_FIELD(RigidbodyComponent, freezeRotation, Int32),
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
        MYE_FIELD(UIElementComponent, text, String256),
        // M35 拡張 (末尾 append)
        MYE_FIELD_RANGE(UIElementComponent, fillAmount, Float, 0.0f, 1.0f),
        MYE_FIELD_TIP(UIElementComponent, fillMode, Int32, "0=off 1=horizontal 2=vertical"),
        MYE_FIELD_TIP(UIElementComponent, sliceBorder, Float4, "9-slice border px (l,t,r,b)"),
        MYE_FIELD(UIElementComponent, sliced, Int32),
        MYE_FIELD(UIElementComponent, focusable, Int32),
        MYE_FIELD(UIElementComponent, focused, Int32),
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

    // M29a: 定常力。Rigidbody の velocity (hash 対象) を決定論的に駆動するので **hash 対象**。
    // opt-in (無ければ物理非関与) で TypeId append (=18) のみ → 既存シーン不変 = bump 不要
    RegisterComponent<ConstantForceComponent>("ConstantForce", {
        MYE_FIELD(ConstantForceComponent, force, Float3),
        MYE_FIELD(ConstantForceComponent, torque, Float3),
        MYE_FIELD(ConstantForceComponent, relative, Int32),
    });

    // M29a: 距離バネジョイント。速度を駆動するので **hash 対象**。opt-in で TypeId append (=19)。
    // connectedEntity は EntityRef → シーン保存は fileId 変換、プレハブは既存 remap が面倒を見る
    RegisterComponent<SpringJointComponent>("SpringJoint", {
        MYE_FIELD(SpringJointComponent, connectedEntity, EntityRef),
        MYE_FIELD_RANGE(SpringJointComponent, restLength, Float, 0.0f, 1000.0f),
        MYE_FIELD_TIP(SpringJointComponent, stiffness, Float,
                      "安定条件: stiffness*dt^2/mass < 4 (dt=1/60 → mass=1 で k < 14400)"),
        MYE_FIELD_RANGE(SpringJointComponent, damping, Float, 0.0f, 100000.0f),
    });

    // M29b: キャラクターコントローラ。LocalTransform を駆動する sim 状態なので **hash 対象**。
    // opt-in で TypeId append (=20) のみ → 既存シーン不変 = bump 不要
    RegisterComponent<CharacterControllerComponent>("CharacterController", {
        MYE_FIELD_RANGE(CharacterControllerComponent, radius, Float, 0.01f, 10.0f),
        MYE_FIELD_RANGE(CharacterControllerComponent, height, Float, 0.1f, 20.0f),
        MYE_FIELD_RANGE(CharacterControllerComponent, slopeLimitDeg, Float, 0.0f, 89.0f),
        MYE_FIELD_RANGE(CharacterControllerComponent, skinWidth, Float, 0.0f, 0.5f),
        MYE_FIELD(CharacterControllerComponent, gravityScale, Float),
        MYE_FIELD(CharacterControllerComponent, moveInput, Float3),
        MYE_FIELD_FLAGS(CharacterControllerComponent, velocity, Float3, kFieldReadOnly),
        MYE_FIELD_FLAGS(CharacterControllerComponent, jumpSpeed, Float, kFieldHidden),
        MYE_FIELD_FLAGS(CharacterControllerComponent, isGrounded, Int32, kFieldReadOnly),
    });

    // M29c: スプライト/トレイル/3D テキスト。描画専用なので **kComponentNoHash**
    // (既存シーンのハッシュ不変)。serialize はされる。opt-in で TypeId append (=21/22/23) のみ
    RegisterComponent<SpriteRendererComponent>("SpriteRenderer", {
        MYE_FIELD(SpriteRendererComponent, texture, AssetRef),
        MYE_FIELD(SpriteRendererComponent, color, Color),
        MYE_FIELD(SpriteRendererComponent, size, Float2),
        MYE_FIELD(SpriteRendererComponent, billboardMode, Int32),
    }, kComponentNoHash);

    RegisterComponent<TrailRendererComponent>("TrailRenderer", {
        MYE_FIELD_RANGE(TrailRendererComponent, duration, Float, 0.02f, 30.0f),
        MYE_FIELD_RANGE(TrailRendererComponent, width, Float, 0.001f, 10.0f),
        MYE_FIELD(TrailRendererComponent, colorBegin, Color),
        MYE_FIELD(TrailRendererComponent, colorEnd, Color),
        MYE_FIELD_RANGE(TrailRendererComponent, minVertexDistance, Float, 0.001f, 10.0f),
        MYE_FIELD(TrailRendererComponent, emitting, Int32),
    }, kComponentNoHash);

    RegisterComponent<TextMeshComponent>("TextMesh", {
        MYE_FIELD(TextMeshComponent, text, String256),
        MYE_FIELD_RANGE(TextMeshComponent, fontScale, Float, 0.05f, 50.0f),
        MYE_FIELD(TextMeshComponent, color, Color),
        MYE_FIELD(TextMeshComponent, billboardMode, Int32),
    }, kComponentNoHash);

    // M29d: スカイボックス/フォグ。描画専用なので **kComponentNoHash** (既存シーン不変)。
    // opt-in で TypeId append (=24/25) のみ → bump 不要
    RegisterComponent<SkyboxComponent>("Skybox", {
        MYE_FIELD(SkyboxComponent, mode, Int32),
        MYE_FIELD(SkyboxComponent, topColor, Color),
        MYE_FIELD(SkyboxComponent, horizonColor, Color),
        MYE_FIELD(SkyboxComponent, bottomColor, Color),
        MYE_FIELD(SkyboxComponent, cubemapTexture, AssetRef),
    }, kComponentNoHash);

    RegisterComponent<FogComponent>("Fog", {
        MYE_FIELD(FogComponent, mode, Int32),
        MYE_FIELD(FogComponent, color, Color),
        MYE_FIELD_RANGE(FogComponent, density, Float, 0.0f, 1.0f),
        MYE_FIELD(FogComponent, start, Float),
        MYE_FIELD(FogComponent, end, Float),
    }, kComponentNoHash);

    // M29e: カメラ別ポストプロセス。描画専用なので **kComponentNoHash**。
    // opt-in で TypeId append (=26) のみ → bump 不要
    RegisterComponent<CameraPostFxComponent>("CameraPostFx", {
        MYE_FIELD_RANGE(CameraPostFxComponent, exposure, Float, 0.0f, 16.0f),
        MYE_FIELD(CameraPostFxComponent, tonemapMode, Int32),
        MYE_FIELD(CameraPostFxComponent, bloomOn, Int32),
        MYE_FIELD(CameraPostFxComponent, bloomThreshold, Float),
        MYE_FIELD(CameraPostFxComponent, bloomIntensity, Float),
        MYE_FIELD(CameraPostFxComponent, fxaaOn, Int32),
        // M32d: 色収差 / ビネット / カラーグレーディング (末尾 append、NoHash なので bump 不要)
        MYE_FIELD_RANGE(CameraPostFxComponent, chromAberration, Float, 0.0f, 0.05f),
        MYE_FIELD_RANGE(CameraPostFxComponent, vignetteIntensity, Float, 0.0f, 1.0f),
        MYE_FIELD_RANGE(CameraPostFxComponent, vignetteRadius, Float, 0.0f, 1.0f),
        MYE_FIELD_RANGE(CameraPostFxComponent, saturation, Float, 0.0f, 4.0f),
        MYE_FIELD_RANGE(CameraPostFxComponent, contrast, Float, 0.0f, 4.0f),
        MYE_FIELD(CameraPostFxComponent, colorFilter, Color),
    }, kComponentNoHash);

    // M32e: 合成エフェクトのライフサイクル。DestroyEntity + 子エミッタ playing を駆動 = hash 対象。
    // opt-in (TypeId append =27) なので既存シーンは不変 = bump 不要
    RegisterComponent<EffectComponent>("Effect", {
        MYE_FIELD(EffectComponent, durationTicks, Int32),
        MYE_FIELD(EffectComponent, lingerTicks, Int32),
        MYE_FIELD_FLAGS(EffectComponent, elapsedTicks, Int32, kFieldReadOnly),
        MYE_FIELD(EffectComponent, playing, Int32),
        MYE_FIELD(EffectComponent, looping, Int32),
        MYE_FIELD(EffectComponent, autoDestroy, Int32),
    });
}

} // namespace mye
