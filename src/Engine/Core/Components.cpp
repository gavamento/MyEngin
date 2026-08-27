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
        MYE_JP("値", MYE_FIELD(NameComponent, value, String64)),
    });

    RegisterComponent<LocalTransform>("LocalTransform", {
        MYE_JP("位置", MYE_FIELD(LocalTransform, position, Float3)),
        MYE_JP("回転", MYE_FIELD(LocalTransform, rotation, Quat)),
        MYE_JP("サイズ", MYE_FIELD(LocalTransform, scale, Float3)),
    });

    RegisterComponent<WorldMatrixComponent>("WorldMatrix", {
        MYE_JP("値", MYE_FIELD_FLAGS(WorldMatrixComponent, value, Float4x4, kFieldReadOnly | kFieldNoSerialize)),
    }, kComponentNoSerialize | kComponentHidden);

    RegisterComponent<HierarchyComponent>("Hierarchy", {
        MYE_JP("親", MYE_FIELD_FLAGS(HierarchyComponent, parent, EntityRef, kFieldReadOnly | kFieldNoSerialize)),
        MYE_JP("最初の子", MYE_FIELD_FLAGS(HierarchyComponent, firstChild, EntityRef, kFieldHidden | kFieldNoSerialize)),
        MYE_JP("次の兄弟", MYE_FIELD_FLAGS(HierarchyComponent, nextSibling, EntityRef, kFieldHidden | kFieldNoSerialize)),
        MYE_JP("階層の深さ", MYE_FIELD_FLAGS(HierarchyComponent, depth, UInt32, kFieldReadOnly | kFieldNoSerialize)),
    }, kComponentNoSerialize | kComponentHidden); // 親子関係はシーンシリアライザが fileId で特別扱い

    RegisterComponent<MeshRendererComponent>("MeshRenderer", {
        MYE_JP("メッシュ", MYE_FIELD(MeshRendererComponent, mesh, AssetRef)),
        MYE_JP("マテリアル", MYE_FIELD(MeshRendererComponent, material, AssetRef)),
    });

    RegisterComponent<CameraComponent>("Camera", {
        MYE_JP("視野角 (度)", MYE_FIELD(CameraComponent, fovYDeg, Float)),
        MYE_JP("ニアクリップ", MYE_FIELD(CameraComponent, nearZ, Float)),
        MYE_JP("ファークリップ", MYE_FIELD(CameraComponent, farZ, Float)),
        MYE_JP("メインカメラ", MYE_FIELD(CameraComponent, isPrimary, Int32)),
    });

    RegisterComponent<LightComponent>("Light", {
        MYE_JP("色", MYE_FIELD(LightComponent, color, Float3)),
        MYE_JP("強度", MYE_FIELD(LightComponent, intensity, Float)),
        MYE_JP("環境光", MYE_FIELD(LightComponent, ambient, Float3)),
        MYE_JP("種類", MYE_FIELD(LightComponent, type, Int32)),
        MYE_JP("範囲", MYE_FIELD(LightComponent, range, Float)),
        MYE_JP("スポット内角 (度)", MYE_FIELD(LightComponent, spotInnerDeg, Float)),
        MYE_JP("スポット外角 (度)", MYE_FIELD(LightComponent, spotOuterDeg, Float)),
        MYE_JP("影を落とす", MYE_FIELD(LightComponent, castShadow, Int32)), // M54b (点/スポット用)
    });

    RegisterComponent<FileIdComponent>("FileId", {
        MYE_JP("値", MYE_FIELD_FLAGS(FileIdComponent, value, UInt64, kFieldReadOnly | kFieldNoSerialize)),
    }, kComponentNoSerialize | kComponentHidden); // シリアライザが "fileId" として特別扱い

    RegisterComponent<ParticleEmitterComponent>("ParticleEmitter", {
        MYE_JP("放出レート", MYE_FIELD(ParticleEmitterComponent, rate, Float)),
        MYE_JP("形状", MYE_FIELD(ParticleEmitterComponent, shape, Int32)),
        MYE_JP("形状の半径", MYE_FIELD(ParticleEmitterComponent, shapeRadius, Float)),
        MYE_JP("コーン角 (度)", MYE_FIELD(ParticleEmitterComponent, coneAngleDeg, Float)),
        MYE_JP("ボックスの広がり", MYE_FIELD(ParticleEmitterComponent, boxExtents, Float3)),
        MYE_JP("寿命 (最小)", MYE_FIELD(ParticleEmitterComponent, lifetimeMin, Float)),
        MYE_JP("寿命 (最大)", MYE_FIELD(ParticleEmitterComponent, lifetimeMax, Float)),
        MYE_JP("初速 (最小)", MYE_FIELD(ParticleEmitterComponent, speedMin, Float)),
        MYE_JP("初速 (最大)", MYE_FIELD(ParticleEmitterComponent, speedMax, Float)),
        MYE_JP("サイズ (最小)", MYE_FIELD(ParticleEmitterComponent, sizeMin, Float)),
        MYE_JP("サイズ (最大)", MYE_FIELD(ParticleEmitterComponent, sizeMax, Float)),
        MYE_JP("開始色", MYE_FIELD(ParticleEmitterComponent, colorBegin, Color)),
        MYE_JP("終了色", MYE_FIELD(ParticleEmitterComponent, colorEnd, Color)),
        MYE_JP("サイズ終端の倍率", MYE_FIELD(ParticleEmitterComponent, sizeEndScale, Float)),
        MYE_JP("重力", MYE_FIELD(ParticleEmitterComponent, gravity, Float3)),
        MYE_JP("風", MYE_FIELD(ParticleEmitterComponent, wind, Float3)),
        MYE_JP("乱流", MYE_FIELD(ParticleEmitterComponent, turbulence, Float)),
        MYE_JP("ブレンド", MYE_FIELD(ParticleEmitterComponent, blendMode, Int32)),
        MYE_JP("シード", MYE_FIELD(ParticleEmitterComponent, seed, UInt32)),
        MYE_JP("最大数", MYE_FIELD(ParticleEmitterComponent, maxParticles, Int32)),
        // M32a: ライフサイクル + 多点グラデーション + テクスチャ/フリップブック + ソフトパーティクル。
        // 末尾 append なので既存シーンは既定値ロードで挙動不変 (ハッシュは変わる → golden 再記録)。
        MYE_JP("再生中", MYE_FIELD(ParticleEmitterComponent, playing, Int32)),
        MYE_JP("長さ (tick)", MYE_FIELD(ParticleEmitterComponent, durationTicks, Int32)),
        MYE_JP("ループ", MYE_FIELD(ParticleEmitterComponent, looping, Int32)),
        MYE_JP("バースト数", MYE_FIELD(ParticleEmitterComponent, burstCount, Int32)),
        MYE_JP("中間色 1", MYE_FIELD(ParticleEmitterComponent, colorMid1, Color)),
        MYE_JP("中間色 1 の t", MYE_FIELD(ParticleEmitterComponent, colorMidT1, Float)),
        MYE_JP("中間色 2", MYE_FIELD(ParticleEmitterComponent, colorMid2, Color)),
        MYE_JP("中間色 2 の t", MYE_FIELD(ParticleEmitterComponent, colorMidT2, Float)),
        MYE_JP("サイズ中間の倍率", MYE_FIELD(ParticleEmitterComponent, sizeMidScale, Float)),
        MYE_JP("サイズ中間 t", MYE_FIELD(ParticleEmitterComponent, sizeMidT, Float)),
        MYE_JP("テクスチャ", MYE_FIELD(ParticleEmitterComponent, texture, AssetRef)),
        MYE_JP("フリップ列数", MYE_FIELD(ParticleEmitterComponent, flipTilesX, Int32)),
        MYE_JP("フリップ行数", MYE_FIELD(ParticleEmitterComponent, flipTilesY, Int32)),
        MYE_JP("フリップ周回数", MYE_FIELD(ParticleEmitterComponent, flipCycles, Float)),
        MYE_JP("ソフトフェード距離", MYE_FIELD(ParticleEmitterComponent, softFadeDistance, Float)),
        // M42e: GPU 深度衝突 (末尾 append。hash 対象フィールド追加 → golden 再記録 = M42e で実施)
        MYE_JP("深度バッファ衝突", MYE_FIELD(ParticleEmitterComponent, depthCollision, Int32)),
        MYE_JP("衝突時の反発", MYE_FIELD_RANGE(ParticleEmitterComponent, collisionBounce, Float, 0.0f, 1.0f)),
        // M61a: A群拡張 (末尾 append)。既定値 = 従来挙動とビット同一。hash 対象の追加なので
        // 既存 .rep のハッシュ値は変わる (.rep は毎回録り直しの使い捨て、M59a2 と同じ扱い)
        MYE_JP("速度の継承", MYE_FIELD(ParticleEmitterComponent, velocityInheritance, Float)),
        MYE_JP("シミュレーション空間", MYE_FIELD(ParticleEmitterComponent, simulationSpace, Int32)),
        MYE_JP("プリウォーム (秒)", MYE_FIELD(ParticleEmitterComponent, prewarmTime, Float)),
        MYE_JP("サブフレーム放出", MYE_FIELD(ParticleEmitterComponent, subframeEmission, Int32)),
        MYE_JP("乱流モード", MYE_FIELD(ParticleEmitterComponent, turbulenceMode, Int32)),
        MYE_JP("ノイズ周波数", MYE_FIELD(ParticleEmitterComponent, noiseFrequency, Float)),
        MYE_JP("ノイズ速度", MYE_FIELD(ParticleEmitterComponent, noiseSpeed, Float)),
        MYE_JP("放出元", MYE_FIELD(ParticleEmitterComponent, emitFrom, Int32)),
    });

    // M28a: height / friction、M36a: layer / mask / meshAsset を末尾 append
    // (フィールド順変更なし = シーン互換維持。既存シーンは欠損フィールドをデフォルト値でロード。
    //  hash 対象フィールドの追加なので golden.rep は M36a で再記録済み)
    RegisterComponent<ColliderComponent>("Collider", {
        MYE_JP("形状", MYE_FIELD(ColliderComponent, shape, Int32)),
        MYE_JP("半径", MYE_FIELD(ColliderComponent, radius, Float)),
        MYE_JP("ハーフサイズ", MYE_FIELD(ColliderComponent, halfExtents, Float3)),
        MYE_JP("トリガー", MYE_FIELD(ColliderComponent, isTrigger, Bool)),
        MYE_JP("高さ", MYE_FIELD(ColliderComponent, height, Float)),
        MYE_JP("摩擦", MYE_FIELD(ColliderComponent, friction, Float)),
        MYE_JP("レイヤー", MYE_FIELD_TIP(ColliderComponent, layer, Int32, "collision layer 0..31")),
        MYE_JP("衝突マスク", MYE_FIELD_TIP(ColliderComponent, mask, UInt32, "layers this collider hits (bitmask)")),
        MYE_JP("メッシュアセット", MYE_FIELD(ColliderComponent, meshAsset, AssetRef)), // M41 予約 (現状未使用)
        // M59a2: 物理マテリアル (末尾 append)。hash 対象フィールドの追加だが .rep は毎回
        // 録り直しの使い捨てで、挙動のビット同一は PhysicsSelfTest の [phys] body
        // ビットパターン照合で証明する (ワールドハッシュ値自体はフィールド追加で変わる)
        MYE_JP("物理マテリアル", MYE_FIELD(ColliderComponent, physMaterial, AssetRef)),
        MYE_JP("材料の上書き", MYE_FIELD(ColliderComponent, materialOverrideBits, UInt32)),
    });

    // M10: 末尾追加 (TypeId 順を壊さない)。無ければ有効なので既存シーンは不変
    RegisterComponent<ActiveComponent>("Active", {
        MYE_JP("有効", MYE_FIELD(ActiveComponent, enabled, Int32)),
    });

    // M13: プレハブタグ。純データ (どのシステムにも参加しない = sim 非影響)。
    // kComponentHidden で Inspector の Add/一覧から隠すが、シリアライズ+ハッシュはされる。
    // 無ければ通常エンティティなので既存シーンのハッシュは不変 (ReplayFile bump 不要)
    RegisterComponent<PrefabInstanceComponent>("PrefabInstance", {
        MYE_JP("プレハブハッシュ", MYE_FIELD_FLAGS(PrefabInstanceComponent, prefabHash, UInt64, kFieldReadOnly)),
        // M48c: 末尾追加。追跡シーンに PrefabInstance は 1 件も無いのでハッシュ影響なし
        MYE_JP("外側ローカル ID", MYE_FIELD_FLAGS(PrefabInstanceComponent, outerLocalId, UInt64, kFieldReadOnly)),
    }, kComponentHidden);

    RegisterComponent<PrefabLinkComponent>("PrefabLink", {
        MYE_JP("ローカル ID", MYE_FIELD_FLAGS(PrefabLinkComponent, localId, UInt64, kFieldReadOnly)),
    }, kComponentHidden);

    // M14: アニメータ。無ければ何もしない (opt-in) ので既存シーンは不変
    RegisterComponent<AnimatorComponent>("Animator", {
        MYE_JP("クリップ", MYE_FIELD(AnimatorComponent, clip, AssetRef)),
        MYE_JP("再生位置 (tick)", MYE_FIELD_FLAGS(AnimatorComponent, timeTicks, Int32, kFieldReadOnly)),
        MYE_JP("速度", MYE_FIELD(AnimatorComponent, speed, Int32)),
        MYE_JP("ループ", MYE_FIELD(AnimatorComponent, loop, Int32)),
        MYE_JP("再生中", MYE_FIELD(AnimatorComponent, playing, Int32)),
    });

    // M18: スケルタルスキニング。ポーズは描画専用なので **kComponentNoHash** (既存シーン不変)。
    // opt-in (無ければ通常メッシュ描画) なので TypeId append (=14) だけで bump 不要
    RegisterComponent<SkinnedMeshComponent>("SkinnedMesh", {
        MYE_JP("モデル", MYE_FIELD(SkinnedMeshComponent, model, AssetRef)),
        MYE_JP("クリップ", MYE_FIELD(SkinnedMeshComponent, clip, Int32)),
        MYE_JP("再生位置 (tick)", MYE_FIELD_FLAGS(SkinnedMeshComponent, timeTicks, Int32, kFieldReadOnly)),
        MYE_JP("再生中", MYE_FIELD(SkinnedMeshComponent, playing, Int32)),
    }, kComponentNoHash);

    // M20: 剛体。velocity は積分される sim 状態なので **hash 対象** (kComponentNoHash を付けない)。
    // opt-in (無ければ物理非関与) なので TypeId append (=15) だけで既存シーンは不変 → bump 不要
    // M28b: angularVelocity / angularDamping / freezeRotation を末尾 append。
    // angularVelocity は積分される sim 状態なので hash 対象 (velocity と同格)
    RegisterComponent<RigidbodyComponent>("Rigidbody", {
        MYE_JP("速度", MYE_FIELD(RigidbodyComponent, velocity, Float3)),
        MYE_JP("質量", MYE_FIELD(RigidbodyComponent, mass, Float)),
        MYE_JP("移動の減衰", MYE_FIELD(RigidbodyComponent, linearDamping, Float)),
        MYE_JP("反発", MYE_FIELD(RigidbodyComponent, restitution, Float)),
        MYE_JP("重力スケール", MYE_FIELD(RigidbodyComponent, gravityScale, Float)),
        MYE_JP("キネマティック", MYE_FIELD(RigidbodyComponent, isKinematic, Int32)),
        MYE_JP("角速度", MYE_FIELD(RigidbodyComponent, angularVelocity, Float3)),
        MYE_JP("回転の減衰", MYE_FIELD(RigidbodyComponent, angularDamping, Float)),
        MYE_JP("回転を固定", MYE_FIELD(RigidbodyComponent, freezeRotation, Bool)), // M59a2 後続で bool 化
        // M59a2: 密度→質量導出 (opt-in、末尾 append)
        MYE_JP("密度から質量", MYE_FIELD_TIP(RigidbodyComponent, useDensity, Bool,
                                             "mass = material density x scaled shape volume "
                                             "(needs a collider with a phys material)")),
        // M59f1: ジャイロ項 + 質量中心オフセット (どちらも opt-in、末尾 append)
        MYE_JP("ジャイロ効果",
               MYE_FIELD_TIP(RigidbodyComponent, gyroscopic, Bool,
                             "Integrate the gyroscopic term (omega x I omega). Off by default; "
                             "a sphere is unaffected by construction")),
        MYE_JP("質量中心",
               MYE_FIELD_TIP(RigidbodyComponent, centerOfMass, Float3,
                             "Local offset of the centre of mass from the shape origin. The "
                             "inertia tensor is taken to be about this point")),
        // M59h: スリープ状態 (ソルバが書く sim 状態。Inspector からは観測用)
        MYE_JP("スリープ計数",
               MYE_FIELD_TIP(RigidbodyComponent, sleepTicks, Int32,
                             "Consecutive quiet ticks. Driven by the solver")),
        MYE_JP("スリープ中",
               MYE_FIELD_TIP(RigidbodyComponent, isSleeping, Bool,
                             "Asleep: the solver skips it and its velocities are exactly zero")),
        // M59j: 連続衝突判定 (opt-in、末尾 append)
        MYE_JP("連続衝突判定 (CCD)",
               MYE_FIELD_TIP(RigidbodyComponent, ccd, Bool,
                             "Sweep this body when it moves further than its own size in one "
                             "substep, so it stops at the first touch instead of tunnelling")),
        // M60e: 複合コライダー (opt-in、末尾 append)
        MYE_JP("複合コライダー",
               MYE_FIELD_TIP(RigidbodyComponent, compoundColliders, Bool,
                             "Absorb the colliders of descendants that have no rigidbody of "
                             "their own, and derive the centre of mass and inertia from them")),
    });

    // M21: ゲーム内 UI。描画専用なので **kComponentNoHash** (既存シーンのハッシュ不変)。
    // serialize はされる (UI をシーン保存/Inspector 編集可能)。opt-in で TypeId append (=16) のみ
    RegisterComponent<UIElementComponent>("UIElement", {
        MYE_JP("種類", MYE_FIELD(UIElementComponent, kind, Int32)),
        MYE_JP("アンカー", MYE_FIELD(UIElementComponent, anchor, Int32)),
        MYE_JP("X", MYE_FIELD(UIElementComponent, x, Float)),
        MYE_JP("Y", MYE_FIELD(UIElementComponent, y, Float)),
        MYE_JP("幅", MYE_FIELD(UIElementComponent, w, Float)),
        MYE_JP("高さ", MYE_FIELD(UIElementComponent, h, Float)),
        MYE_JP("色", MYE_FIELD(UIElementComponent, color, Color)),
        MYE_JP("テクスチャ", MYE_FIELD(UIElementComponent, texture, AssetRef)),
        MYE_JP("文字サイズ", MYE_FIELD(UIElementComponent, fontScale, Float)),
        MYE_JP("描画順", MYE_FIELD(UIElementComponent, order, Int32)),
        MYE_JP("テキスト", MYE_FIELD(UIElementComponent, text, String256)),
        // M35 拡張 (末尾 append)
        MYE_JP("フィル量", MYE_FIELD_RANGE(UIElementComponent, fillAmount, Float, 0.0f, 1.0f)),
        MYE_JP("フィル方向", MYE_FIELD_TIP(UIElementComponent, fillMode, Int32, "0=off 1=horizontal 2=vertical")),
        MYE_JP("スライス境界", MYE_FIELD_TIP(UIElementComponent, sliceBorder, Float4, "9-slice border px (l,t,r,b)")),
        MYE_JP("9 スライス", MYE_FIELD(UIElementComponent, sliced, Int32)),
        MYE_JP("フォーカス可", MYE_FIELD(UIElementComponent, focusable, Int32)),
        MYE_JP("フォーカス中", MYE_FIELD(UIElementComponent, focused, Int32)),
        // M51e 拡張 (末尾 append)
        MYE_JP("配置空間", MYE_FIELD_TIP(UIElementComponent, space, Int32, "0=screen 1=parent rect")),
        MYE_JP("子をクリップ", MYE_FIELD(UIElementComponent, clipChildren, Int32)),
        MYE_JP("文字整列", MYE_FIELD_TIP(UIElementComponent, align, Int32, "9-grid 0..8 (text only)")),
        MYE_JP("折返し", MYE_FIELD_TIP(UIElementComponent, wrap, Int32, "0=off 1=char wrap at width")),
        // ワールド追従 UI (末尾 append、NoHash)。追従は自動判定 — これらは追従要素の見た目調整
        MYE_JP("距離で縮む", MYE_FIELD_TIP(UIElementComponent, distanceScale, Bool,
                                           "scale by camera distance (world-attached UI only)")),
        MYE_JP("等倍距離 (m)", MYE_FIELD_TIP(UIElementComponent, distanceRef, Float,
                                             "distance at which scale = 1.0")),
        MYE_JP("画面内にクランプ", MYE_FIELD_TIP(UIElementComponent, clampToScreen, Bool,
                                                 "keep the rect on screen (world-attached UI only)")),
    }, kComponentNoHash);

    // M22: Animator Controller。LocalTransform を駆動するので **hash 対象** (kComponentNoHash 無し)。
    // opt-in (無ければ no-op) で TypeId append (=17) のみ → 既存シーン不変 = bump 不要。
    // params[4] は配列なので手動 FieldDesc で各要素を Int32 登録する (hash + serialize + Inspector)
    RegisterComponent<AnimatorControllerComponent>("AnimatorController", {
        MYE_JP("コントローラ", MYE_FIELD(AnimatorControllerComponent, controller, AssetRef)),
        MYE_JP("現在のステート", MYE_FIELD_FLAGS(AnimatorControllerComponent, currentState, Int32, kFieldReadOnly)),
        MYE_JP("ステート経過 (tick)", MYE_FIELD_FLAGS(AnimatorControllerComponent, stateTimeTicks, Int32, kFieldReadOnly)),
        MYE_JP("遷移先", MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionTo, Int32, kFieldReadOnly)),
        MYE_JP("遷移経過 (tick)", MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionTick, Int32, kFieldReadOnly)),
        MYE_JP("遷移時間 (tick)", MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionDuration, Int32, kFieldReadOnly)),
        MYE_JP("遷移先の再生位置", MYE_FIELD_FLAGS(AnimatorControllerComponent, transitionToTime, Int32, kFieldReadOnly)),
        MYE_JP("パラメータ 0",
               FieldDesc{ "param0", FieldType::Int32,
                          static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 0 * sizeof(int32_t)),
                          kFieldNone }),
        MYE_JP("パラメータ 1",
               FieldDesc{ "param1", FieldType::Int32,
                          static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 1 * sizeof(int32_t)),
                          kFieldNone }),
        MYE_JP("パラメータ 2",
               FieldDesc{ "param2", FieldType::Int32,
                          static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 2 * sizeof(int32_t)),
                          kFieldNone }),
        MYE_JP("パラメータ 3",
               FieldDesc{ "param3", FieldType::Int32,
                          static_cast<uint32_t>(offsetof(AnimatorControllerComponent, params) + 3 * sizeof(int32_t)),
                          kFieldNone }),
    });

    // M29a: 定常力。Rigidbody の velocity (hash 対象) を決定論的に駆動するので **hash 対象**。
    // opt-in (無ければ物理非関与) で TypeId append (=18) のみ → 既存シーン不変 = bump 不要
    RegisterComponent<ConstantForceComponent>("ConstantForce", {
        MYE_JP("力", MYE_FIELD(ConstantForceComponent, force, Float3)),
        MYE_JP("トルク", MYE_FIELD(ConstantForceComponent, torque, Float3)),
        MYE_JP("ローカル座標系", MYE_FIELD(ConstantForceComponent, relative, Int32)),
    });

    // M29a: 距離バネジョイント。速度を駆動するので **hash 対象**。opt-in で TypeId append (=19)。
    // connectedEntity は EntityRef → シーン保存は fileId 変換、プレハブは既存 remap が面倒を見る
    RegisterComponent<SpringJointComponent>("SpringJoint", {
        MYE_JP("接続先", MYE_FIELD(SpringJointComponent, connectedEntity, EntityRef)),
        MYE_JP("自然長", MYE_FIELD_RANGE(SpringJointComponent, restLength, Float, 0.0f, 1000.0f)),
        MYE_JP("ばね定数", MYE_FIELD_TIP(SpringJointComponent, stiffness, Float,
                      "安定条件: stiffness*dt^2/mass < 4 (dt=1/60 → mass=1 で k < 14400)")),
        MYE_JP("減衰", MYE_FIELD_RANGE(SpringJointComponent, damping, Float, 0.0f, 100000.0f)),
    });

    // M29b: キャラクターコントローラ。LocalTransform を駆動する sim 状態なので **hash 対象**。
    // opt-in で TypeId append (=20) のみ → 既存シーン不変 = bump 不要
    RegisterComponent<CharacterControllerComponent>("CharacterController", {
        MYE_JP("半径", MYE_FIELD_RANGE(CharacterControllerComponent, radius, Float, 0.01f, 10.0f)),
        MYE_JP("高さ", MYE_FIELD_RANGE(CharacterControllerComponent, height, Float, 0.1f, 20.0f)),
        MYE_JP("登れる傾斜 (度)", MYE_FIELD_RANGE(CharacterControllerComponent, slopeLimitDeg, Float, 0.0f, 89.0f)),
        MYE_JP("スキン幅", MYE_FIELD_RANGE(CharacterControllerComponent, skinWidth, Float, 0.0f, 0.5f)),
        MYE_JP("重力スケール", MYE_FIELD(CharacterControllerComponent, gravityScale, Float)),
        MYE_JP("移動入力", MYE_FIELD(CharacterControllerComponent, moveInput, Float3)),
        MYE_JP("速度", MYE_FIELD_FLAGS(CharacterControllerComponent, velocity, Float3, kFieldReadOnly)),
        MYE_JP("ジャンプ速度", MYE_FIELD_FLAGS(CharacterControllerComponent, jumpSpeed, Float, kFieldHidden)),
        MYE_JP("接地している", MYE_FIELD_FLAGS(CharacterControllerComponent, isGrounded, Int32, kFieldReadOnly)),
    });

    // M29c: スプライト/トレイル/3D テキスト。描画専用なので **kComponentNoHash**
    // (既存シーンのハッシュ不変)。serialize はされる。opt-in で TypeId append (=21/22/23) のみ
    RegisterComponent<SpriteRendererComponent>("SpriteRenderer", {
        MYE_JP("テクスチャ", MYE_FIELD(SpriteRendererComponent, texture, AssetRef)),
        MYE_JP("色", MYE_FIELD(SpriteRendererComponent, color, Color)),
        MYE_JP("サイズ", MYE_FIELD(SpriteRendererComponent, size, Float2)),
        MYE_JP("ビルボード", MYE_FIELD(SpriteRendererComponent, billboardMode, Int32)),
    }, kComponentNoHash);

    RegisterComponent<TrailRendererComponent>("TrailRenderer", {
        MYE_JP("継続時間", MYE_FIELD_RANGE(TrailRendererComponent, duration, Float, 0.02f, 30.0f)),
        MYE_JP("幅", MYE_FIELD_RANGE(TrailRendererComponent, width, Float, 0.001f, 10.0f)),
        MYE_JP("開始色", MYE_FIELD(TrailRendererComponent, colorBegin, Color)),
        MYE_JP("終了色", MYE_FIELD(TrailRendererComponent, colorEnd, Color)),
        MYE_JP("頂点の最小間隔", MYE_FIELD_RANGE(TrailRendererComponent, minVertexDistance, Float, 0.001f, 10.0f)),
        MYE_JP("放出中", MYE_FIELD(TrailRendererComponent, emitting, Int32)),
    }, kComponentNoHash);

    RegisterComponent<TextMeshComponent>("TextMesh", {
        MYE_JP("テキスト", MYE_FIELD(TextMeshComponent, text, String256)),
        MYE_JP("文字サイズ", MYE_FIELD_RANGE(TextMeshComponent, fontScale, Float, 0.05f, 50.0f)),
        MYE_JP("色", MYE_FIELD(TextMeshComponent, color, Color)),
        MYE_JP("ビルボード", MYE_FIELD(TextMeshComponent, billboardMode, Int32)),
    }, kComponentNoHash);

    // M29d: スカイボックス/フォグ。描画専用なので **kComponentNoHash** (既存シーン不変)。
    // opt-in で TypeId append (=24/25) のみ → bump 不要
    RegisterComponent<SkyboxComponent>("Skybox", {
        MYE_JP("モード", MYE_FIELD(SkyboxComponent, mode, Int32)),
        MYE_JP("上の色", MYE_FIELD(SkyboxComponent, topColor, Color)),
        MYE_JP("地平線の色", MYE_FIELD(SkyboxComponent, horizonColor, Color)),
        MYE_JP("下の色", MYE_FIELD(SkyboxComponent, bottomColor, Color)),
        MYE_JP("キューブマップ", MYE_FIELD(SkyboxComponent, cubemapTexture, AssetRef)),
    }, kComponentNoHash);

    RegisterComponent<FogComponent>("Fog", {
        MYE_JP("モード", MYE_FIELD(FogComponent, mode, Int32)),
        MYE_JP("色", MYE_FIELD(FogComponent, color, Color)),
        MYE_JP("濃度", MYE_FIELD_RANGE(FogComponent, density, Float, 0.0f, 1.0f)),
        MYE_JP("開始", MYE_FIELD(FogComponent, start, Float)),
        MYE_JP("終了", MYE_FIELD(FogComponent, end, Float)),
        // M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append、NoHash なので bump 不要)
        MYE_JP("高度減衰", MYE_FIELD_RANGE(FogComponent, heightFalloff, Float, 0.0f, 4.0f)),
        MYE_JP("基準高度", MYE_FIELD(FogComponent, baseHeight, Float)),
        MYE_JP("インスキャッタ強度", MYE_FIELD_RANGE(FogComponent, inscatterIntensity, Float, 0.0f, 1.0f)),
        MYE_JP("インスキャッタ指数", MYE_FIELD_RANGE(FogComponent, inscatterPower, Float, 1.0f, 64.0f)),
    }, kComponentNoHash);

    // M29e: カメラ別ポストプロセス。描画専用なので **kComponentNoHash**。
    // opt-in で TypeId append (=26) のみ → bump 不要
    RegisterComponent<CameraPostFxComponent>("CameraPostFx", {
        MYE_JP("露出", MYE_FIELD_RANGE(CameraPostFxComponent, exposure, Float, 0.0f, 16.0f)),
        MYE_JP("トーンマップ", MYE_FIELD(CameraPostFxComponent, tonemapMode, Int32)),
        MYE_JP("ブルーム", MYE_FIELD(CameraPostFxComponent, bloomOn, Int32)),
        MYE_JP("ブルームしきい値", MYE_FIELD(CameraPostFxComponent, bloomThreshold, Float)),
        MYE_JP("ブルーム強度", MYE_FIELD(CameraPostFxComponent, bloomIntensity, Float)),
        MYE_JP("FXAA", MYE_FIELD(CameraPostFxComponent, fxaaOn, Int32)),
        // M32d: 色収差 / ビネット / カラーグレーディング (末尾 append、NoHash なので bump 不要)
        MYE_JP("色収差", MYE_FIELD_RANGE(CameraPostFxComponent, chromAberration, Float, 0.0f, 0.05f)),
        MYE_JP("ビネット強度", MYE_FIELD_RANGE(CameraPostFxComponent, vignetteIntensity, Float, 0.0f, 1.0f)),
        MYE_JP("ビネット半径", MYE_FIELD_RANGE(CameraPostFxComponent, vignetteRadius, Float, 0.0f, 1.0f)),
        MYE_JP("彩度", MYE_FIELD_RANGE(CameraPostFxComponent, saturation, Float, 0.0f, 4.0f)),
        MYE_JP("コントラスト", MYE_FIELD_RANGE(CameraPostFxComponent, contrast, Float, 0.0f, 4.0f)),
        MYE_JP("カラーフィルタ", MYE_FIELD(CameraPostFxComponent, colorFilter, Color)),
        // M40d: SSAO パラメータ (末尾 append、NoHash なので bump 不要)
        MYE_JP("SSAO 半径", MYE_FIELD_RANGE(CameraPostFxComponent, ssaoRadius, Float, 0.05f, 4.0f)),
        MYE_JP("SSAO 強度", MYE_FIELD_RANGE(CameraPostFxComponent, ssaoIntensity, Float, 0.0f, 4.0f)),
        // M43b: ゴッドレイ (末尾 append、NoHash なので bump 不要)
        MYE_JP("ゴッドレイ強度", MYE_FIELD_RANGE(CameraPostFxComponent, godrayIntensity, Float, 0.0f, 4.0f)),
        MYE_JP("ゴッドレイ減衰", MYE_FIELD_RANGE(CameraPostFxComponent, godrayDecay, Float, 0.5f, 0.999f)),
        // M44a: カラーグレーディング LUT (末尾 append、NoHash なので bump 不要)
        MYE_JP("LUT テクスチャ", MYE_FIELD(CameraPostFxComponent, lutTexture, AssetRef)),
        MYE_JP("LUT 強度", MYE_FIELD_RANGE(CameraPostFxComponent, lutIntensity, Float, 0.0f, 1.0f)),
        // M44b: 自動露出 (末尾 append、NoHash なので bump 不要)
        MYE_JP("自動露出", MYE_FIELD(CameraPostFxComponent, autoExposure, Int32)),
        MYE_JP("自動露出の追従速度", MYE_FIELD_RANGE(CameraPostFxComponent, aeSpeed, Float, 0.1f, 20.0f)),
        MYE_JP("自動露出の下限", MYE_FIELD_RANGE(CameraPostFxComponent, aeMin, Float, 0.01f, 1.0f)),
        MYE_JP("自動露出の上限", MYE_FIELD_RANGE(CameraPostFxComponent, aeMax, Float, 1.0f, 16.0f)),
        // M44c: 被写界深度 (末尾 append、NoHash なので bump 不要)
        MYE_JP("被写界深度: 合焦距離", MYE_FIELD_RANGE(CameraPostFxComponent, dofFocusDistance, Float, 0.1f, 500.0f)),
        MYE_JP("被写界深度: 合焦幅", MYE_FIELD_RANGE(CameraPostFxComponent, dofFocusRange, Float, 0.1f, 100.0f)),
        MYE_JP("被写界深度: 最大ボケ半径", MYE_FIELD_RANGE(CameraPostFxComponent, dofMaxRadius, Float, 0.0f, 32.0f)),
        // M44d: カメラモーションブラー (末尾 append、NoHash なので bump 不要)
        MYE_JP("モーションブラー強度", MYE_FIELD_RANGE(CameraPostFxComponent, motionBlurIntensity, Float, 0.0f, 1.0f)),
        MYE_JP("モーションブラー最大画素", MYE_FIELD_RANGE(CameraPostFxComponent, mbMaxPixels, Float, 1.0f, 64.0f)),
        // M55d: TAA (末尾 append、NoHash なので bump 不要)。Deferred のみ効く
        MYE_JP("TAA", MYE_FIELD(CameraPostFxComponent, taaOn, Int32)),
        MYE_JP("TAA 履歴の残し率", MYE_FIELD_RANGE(CameraPostFxComponent, taaFeedback, Float, 0.0f, 0.95f)),
        // M56d: SSR (末尾 append、NoHash なので bump 不要)。Deferred のみ効く
        MYE_JP("SSR", MYE_FIELD(CameraPostFxComponent, ssrOn, Int32)),
        MYE_JP("SSR 最大粗さ", MYE_FIELD_RANGE(CameraPostFxComponent, ssrMaxRoughness, Float, 0.0f, 1.0f)),
        MYE_JP("SSR 強度", MYE_FIELD_RANGE(CameraPostFxComponent, ssrIntensity, Float, 0.0f, 2.0f)),
        // M57c: フロクセル・ボリュメトリック (末尾 append、NoHash なので bump 不要)
        MYE_JP("ボリュメトリック霧", MYE_FIELD(CameraPostFxComponent, froxelOn, Int32)),
        MYE_JP("霧の密度", MYE_FIELD_RANGE(CameraPostFxComponent, froxelDensity, Float, 0.0f, 0.5f)),
        MYE_JP("霧の異方性", MYE_FIELD_RANGE(CameraPostFxComponent, froxelAnisotropy, Float, -0.9f, 0.9f)),
    }, kComponentNoHash);

    // M32e: 合成エフェクトのライフサイクル。DestroyEntity + 子エミッタ playing を駆動 = hash 対象。
    // opt-in (TypeId append =27) なので既存シーンは不変 = bump 不要
    RegisterComponent<EffectComponent>("Effect", {
        MYE_JP("長さ (tick)", MYE_FIELD(EffectComponent, durationTicks, Int32)),
        MYE_JP("余韻 (tick)", MYE_FIELD(EffectComponent, lingerTicks, Int32)),
        MYE_JP("経過 (tick)", MYE_FIELD_FLAGS(EffectComponent, elapsedTicks, Int32, kFieldReadOnly)),
        MYE_JP("再生中", MYE_FIELD(EffectComponent, playing, Int32)),
        MYE_JP("ループ", MYE_FIELD(EffectComponent, looping, Int32)),
        MYE_JP("再生後に破棄", MYE_FIELD(EffectComponent, autoDestroy, Int32)),
    });

    // M45e: 3D オーディオ。**出力 sink であり決定論レーン外なので kComponentNoHash** —
    // WorldHasher.cpp が NoHash を丸ごとスキップするので、TypeId 末尾 append (=28/29) の
    // これらを足しても既存シーンのハッシュは 1 バイトも変わらない (= golden 再記録不要)。
    RegisterComponent<AudioListenerComponent>("AudioListener", {
        MYE_JP("有効", MYE_FIELD(AudioListenerComponent, enabled, Int32)),
    }, kComponentNoHash);

    RegisterComponent<AudioSourceComponent>("AudioSource", {
        MYE_JP("サウンド", MYE_FIELD(AudioSourceComponent, sound, AssetRef)),
        MYE_JP("起動時に再生", MYE_FIELD(AudioSourceComponent, playOnAwake, Int32)),
        MYE_JP("ループ", MYE_FIELD(AudioSourceComponent, loop, Int32)),
        MYE_JP("音量", MYE_FIELD_RANGE(AudioSourceComponent, volume, Float, 0.0f, 1.0f)),
        MYE_JP("ピッチ", MYE_FIELD_RANGE(AudioSourceComponent, pitch, Float, 0.25f, 4.0f)),
        MYE_JP("ミュート", MYE_FIELD(AudioSourceComponent, mute, Int32)),
        MYE_JP("優先度", MYE_FIELD(AudioSourceComponent, priority, Int32)),
        MYE_JP("バス", MYE_FIELD(AudioSourceComponent, bus, String64)),
        MYE_JP("減衰を上書き", MYE_FIELD(AudioSourceComponent, overrideAttenuation, Int32)),
        MYE_JP("spatial blend", MYE_FIELD_RANGE(AudioSourceComponent, spatialBlend, Float, 0.0f, 1.0f)),
        MYE_JP("最小距離", MYE_FIELD_RANGE(AudioSourceComponent, minDistance, Float, 0.01f, 1000.0f)),
        MYE_JP("最大距離", MYE_FIELD_RANGE(AudioSourceComponent, maxDistance, Float, 0.02f, 10000.0f)),
        MYE_JP("減衰カーブ", MYE_FIELD(AudioSourceComponent, rolloff, Int32)),
        MYE_JP("ドップラー", MYE_FIELD_RANGE(AudioSourceComponent, dopplerScale, Float, 0.0f, 5.0f)),
        MYE_JP("リバーブ送り", MYE_FIELD_RANGE(AudioSourceComponent, reverbSend, Float, 0.0f, 1.0f)),
    }, kComponentNoHash);

    // M48f: 部位 (ソケット)。**hash 対象** — M48g の PartFollowSystem が LocalTransform を
    // 駆動する = sim 状態の入力になるため。opt-in (TypeId 末尾 append =30) なので
    // 既存シーンのハッシュは 1 バイトも変わらない = ReplayFile bump 不要
    RegisterComponent<PartComponent>("Part", {
        MYE_JP("タグ", MYE_FIELD(PartComponent, tag, UInt64)),
        MYE_JP("ジョイント", MYE_FIELD(PartComponent, joint, String64)),
        MYE_JP("骨の供給元", MYE_FIELD(PartComponent, source, EntityRef)),
    });

    // M49: 部位の範囲 (箱/球ボリューム)。**hash 対象** — Parts::RaycastParts の結果を
    // スクリプトが読んで挙動を変える = sim 状態の入力になるため (Part と同じ判断)。
    // opt-in (TypeId 末尾 append =31) なので既存シーンのハッシュは不変 = ReplayFile bump 不要
    RegisterComponent<PartBoundsComponent>("PartBounds", {
        MYE_JP("形状", MYE_FIELD(PartBoundsComponent, shape, Int32)),
        MYE_JP("中心", MYE_FIELD(PartBoundsComponent, center, Float3)),
        MYE_JP("ハーフサイズ", MYE_FIELD_TIP(PartBoundsComponent, halfExtents, Float3,
                                             "sphere uses x as radius")),
    });

    // M52g: 入力レーンの結び付け。**hash 対象** — レーンごとのアクション評価結果を
    // ワールドハッシュに載せること自体が目的 (Components.h の理由 1)。
    // opt-in (TypeId 末尾 append =32) なので既存シーンのハッシュは不変 = ReplayFile bump 不要。
    //
    // ★ミラー 5 本に kFieldNoSerialize を**付けてはいけない**: WorldHasher は
    //   NoSerialize フィールドをハッシュから除外する (WorldHasher.cpp) ので、
    //   付けた瞬間に「毎 tick 書いているのにハッシュに 1 ビットも出ない」= 被覆ゼロになる。
    //   代償としてシーン JSON に tick 限りの入力値が載るが、次の tick で上書きされる
    //   派生値なので実害は無い (ReadOnly で編集は塞いである)
    RegisterComponent<PlayerInputComponent>("PlayerInput", {
        // 範囲外の値は PlayerInputSystem 側で「未接続レーン」に落ちる (= 全ゼロ) ので、
        // Inspector に範囲を持たせない (Int32 のスライダ経路を新規に踏まない)
        MYE_JP("プレイヤー番号", MYE_FIELD(PlayerInputComponent, playerIndex, Int32)),
        MYE_JP("接続", MYE_FIELD_FLAGS(PlayerInputComponent, connected, Bool, kFieldReadOnly)),
        MYE_JP("軸 0-3", MYE_FIELD_FLAGS(PlayerInputComponent, axes, Float4, kFieldReadOnly)),
        MYE_JP("押下中ビット", MYE_FIELD_FLAGS(PlayerInputComponent, heldBits, UInt32, kFieldReadOnly)),
        MYE_JP("押した瞬間ビット", MYE_FIELD_FLAGS(PlayerInputComponent, pressedBits, UInt32, kFieldReadOnly)),
        MYE_JP("離した瞬間ビット", MYE_FIELD_FLAGS(PlayerInputComponent, releasedBits, UInt32, kFieldReadOnly)),
    });

    // M58b: 地形。**kComponentNoHash** — 地形は描画専用レーンで、ハイトフィールドが
    // sim に入る (= ワールドハッシュに載る) のは M59 の地形コリジョンから。
    // opt-in (TypeId 末尾 append) なので既存シーンのハッシュは不変 = ReplayFile bump 不要
    RegisterComponent<TerrainComponent>("Terrain", {
        MYE_JP("地形アセット", MYE_FIELD_TIP(TerrainComponent, source, String64,
                                             "assets-relative .terrain.json path")),
        // 範囲は TerrainSystem::ClampChunkTiles と同じ 2..256。Inspector 側にも入れておくと
        // 「打った数字が黙って丸められて表示と食い違う」事故が起きない
        MYE_JP("チャンクのタイル数",
               MYE_FIELD_RANGE(TerrainComponent, chunkTiles, Int32, 2.0f, 256.0f)),
        // M58e: LOD (フィールド表の末尾 append)。既定 0 = 無効なので、既存シーンを
        // 開き直しても絵は 1 画素も変わらない
        MYE_JP("LOD 切替距離",
               MYE_FIELD_TIP(TerrainComponent, lodDistance, Float,
                             "camera-space depth for LOD 1 (0 = LOD off)")),
        MYE_JP("スカート深さ",
               MYE_FIELD_TIP(TerrainComponent, skirtDepth, Float,
                             "0 = auto (measured LOD edge gap), <0 = no skirt")),
    }, kComponentNoHash);

    // M56a/M56b: デカール (投影ボックス)。**kComponentNoHash** — GBuffer の albedo と
    // 法線 / roughness を上描きするだけの描画レーンで、sim には 1 バイトも触らない。
    // opt-in (TypeId 末尾 append) なので既存シーンのハッシュは不変 = ReplayFile bump 不要
    RegisterComponent<DecalComponent>("Decal", {
        // フィールド名に "tex" が入っていることが Inspector のピッカーが
        // TextureLibrary を引く条件 (InspectorWindow::DrawAssetRef の名前推定)
        MYE_JP("テクスチャ", MYE_FIELD(DecalComponent, texture, AssetRef)),
        MYE_JP("色と不透明度", MYE_FIELD_TIP(DecalComponent, color, Color,
                                             "alpha = decal opacity")),
        MYE_JP("UV スケール", MYE_FIELD_TIP(DecalComponent, uvScale, Float2,
                                            "atlas sub-rect (v1 sampler is LINEAR/CLAMP)")),
        MYE_JP("UV オフセット", MYE_FIELD(DecalComponent, uvOffset, Float2)),
        // 180 を超える値は cos が単調でなくなるので Inspector 側でも止める
        // (範囲外を渡されても DecalAngleFadeCos が丸めるが、表示と食い違わせない)
        MYE_JP("角度フェード", MYE_FIELD_RANGE(DecalComponent, angleFadeDeg, Float, 0.0f, 180.0f)),
        MYE_JP("描画順", MYE_FIELD(DecalComponent, sortOrder, Int32)),
        // ---- M56b (末尾 append)。**強度 0 = 恒等** = 既存シーンを読み直しても
        //      GBuffer は 1 ビットも動かない (フィールドが増えるだけ) ----
        // 注: 名前推定は小文字 "tex" を探すので "normalTex" は総当たり一覧の側に落ちる
        MYE_JP("法線マップ", MYE_FIELD_TIP(DecalComponent, normalTex, AssetRef,
                                           "tangent-space normal map (null = flat)")),
        MYE_JP("法線の強さ", MYE_FIELD_RANGE(DecalComponent, normalStrength, Float, 0.0f, 1.0f)),
        MYE_JP("粗さ", MYE_FIELD_RANGE(DecalComponent, roughness, Float, 0.0f, 1.0f)),
        MYE_JP("粗さの強さ",
               MYE_FIELD_RANGE(DecalComponent, roughnessStrength, Float, 0.0f, 1.0f)),
    }, kComponentNoHash);

    // M56f: ローカル反射プローブ。**kComponentNoHash** — 焼いた cubemap をスペキュラ
    // 環境項へ差し込むだけの描画レーンで、sim には 1 バイトも触らない。
    // 置いただけでは何も起きない (ベイクは常に明示指示) ので、既存シーンへ足しても
    // 絵は 1 ビットも変わらない
    RegisterComponent<ReflectionProbeComponent>("ReflectionProbe", {
        MYE_JP("影響範囲 (半径)",
               MYE_FIELD_TIP(ReflectionProbeComponent, extents, Float3,
                             "half extents of the axis-aligned influence / projection box")),
        MYE_JP("ブレンド距離",
               MYE_FIELD_RANGE(ReflectionProbeComponent, blendDistance, Float, 0.0f, 32.0f)),
        MYE_JP("強度", MYE_FIELD_RANGE(ReflectionProbeComponent, intensity, Float, 0.0f, 4.0f)),
        MYE_JP("ボックス投影",
               MYE_FIELD_TIP(ReflectionProbeComponent, boxProjection, Bool,
                             "off = infinitely distant cube (no parallax correction)")),
        MYE_JP("近クリップ",
               MYE_FIELD_RANGE(ReflectionProbeComponent, nearZ, Float, 0.001f, 10.0f)),
        MYE_JP("遠クリップ",
               MYE_FIELD_RANGE(ReflectionProbeComponent, farZ, Float, 1.0f, 5000.0f)),
    }, kComponentNoHash);

    // M59b: 物理環境。**hash 対象** — 重力ベクトル / 風 / 空気密度は velocity を決定論的に
    // 駆動する sim 入力そのもの。opt-in (TypeId 末尾 append =36) なので既存シーンのハッシュは
    // 1 バイトも変わらない = ReplayFile bump 不要。
    // 消費は「entity.index 最小の active な 1 個」(Skybox/Fog 規約) — 2 個以上置いても
    // 決定論は保たれるが 2 個目以降は黙って無視される
    RegisterComponent<PhysicsEnvironmentComponent>("PhysicsEnvironment", {
        MYE_JP("重力", MYE_FIELD_TIP(PhysicsEnvironmentComponent, gravity, Float3,
                                     "world-space gravity vector (m/s^2)")),
        MYE_JP("空気密度",
               MYE_FIELD_RANGE(PhysicsEnvironmentComponent, airDensity, Float, 0.0f, 100.0f)),
        MYE_JP("風", MYE_FIELD_TIP(PhysicsEnvironmentComponent, windVelocity, Float3,
                                   "uniform steady wind (m/s). Drag acts on v - wind")),
        MYE_JP("水面の高さ", MYE_FIELD(PhysicsEnvironmentComponent, waterPlaneY, Float)),
        MYE_JP("水の密度",
               MYE_FIELD_RANGE(PhysicsEnvironmentComponent, waterDensity, Float, 0.0f, 20000.0f)),
        // レンジとツールチップを両方付けたいので生 FieldDesc で書く (マクロは片方ずつ)。
        // 宣言順 (name/type/offset/…/minVal/maxVal/tooltip) を崩さないこと
        MYE_JP("サブステップ数",
               ::mye::FieldDesc{
                   .name = "substeps", .type = ::mye::FieldType::Int32,
                   .offset = static_cast<uint32_t>(offsetof(PhysicsEnvironmentComponent, substeps)),
                   .minVal = 1.0f, .maxVal = 16.0f,
                   .tooltip = "1 tick is split into this many integrate+solve steps. Higher = "
                              "stiffer springs and cleaner stacks, at a proportional cost" }),
        // M59h: スリープ閾値 (この env が居るシーンだけ眠る)
        MYE_JP("スリープ速度しきい値",
               MYE_FIELD_RANGE(PhysicsEnvironmentComponent, sleepLinearThreshold, Float, 0.0f,
                               10.0f)),
        MYE_JP("スリープ角速度しきい値",
               MYE_FIELD_RANGE(PhysicsEnvironmentComponent, sleepAngularThreshold, Float, 0.0f,
                               10.0f)),
        MYE_JP("スリープ遅延 (tick)",
               MYE_FIELD_TIP(PhysicsEnvironmentComponent, sleepDelayTicks, Int32,
                             "Ticks of continuous quiet before an island falls asleep. "
                             "0 or below disables sleeping entirely")),
    });

    // M59b: 等方空力。**hash 対象** — velocity / angularVelocity を駆動する。
    // opt-in (TypeId 末尾 append =37)。**装着 = 新数式への opt-in** (係数 0 でのビット中立は
    // 約束しない = M59 決定台帳 1 の存在ゲート)
    RegisterComponent<AeroComponent>("Aero", {
        MYE_JP("抗力", MYE_FIELD(AeroComponent, enableDrag, Bool)),
        MYE_JP("角抗力", MYE_FIELD(AeroComponent, enableAngularDrag, Bool)),
        MYE_JP("マグヌス", MYE_FIELD(AeroComponent, enableMagnus, Bool)),
        MYE_JP("抗力係数 Cd", MYE_FIELD_TIP(AeroComponent, dragCoefficient, Float,
                                            "<= 0 uses the physics material's Cd (0.47 if none)")),
        MYE_JP("面積倍率", MYE_FIELD_RANGE(AeroComponent, areaScale, Float, 0.0f, 100.0f)),
        MYE_JP("角抗力係数",
               MYE_FIELD_RANGE(AeroComponent, angularDragCoefficient, Float, 0.0f, 100.0f)),
        MYE_JP("マグヌス係数",
               MYE_FIELD_RANGE(AeroComponent, magnusCoefficient, Float, 0.0f, 100.0f)),
        MYE_JP("面ベース空力", MYE_FIELD_TIP(AeroComponent, surfaceModel, Bool,
                                             "orientation-aware: produces lift and weathercock "
                                             "stability (needs the drag flag)")),
        MYE_JP("表面摩擦", MYE_FIELD_RANGE(AeroComponent, skinFriction, Float, 0.0f, 10.0f)),
    });

    // M59b2: 浮力。**hash 対象** — velocity / angularVelocity を駆動する。
    // opt-in (TypeId 末尾 append =38)。水面と水の密度は PhysicsEnvironment 側 (env 不在なら
    // 既定の水)。**v1 に復原モーメントは無い** (Components.h の制限コメント参照)
    RegisterComponent<BuoyancyComponent>("Buoyancy", {
        MYE_JP("排除体積倍率", MYE_FIELD_TIP(BuoyancyComponent, volumeScale, Float,
                                             "<= 0 disables buoyancy entirely")),
        MYE_JP("水中の抵抗", MYE_FIELD_RANGE(BuoyancyComponent, linearDrag, Float, 0.0f, 100.0f)),
        MYE_JP("水中の回転抵抗",
               MYE_FIELD_RANGE(BuoyancyComponent, angularDrag, Float, 0.0f, 100.0f)),
    });

    // M59d: 翼面。**hash 対象** — 親剛体の velocity / angularVelocity を駆動する。
    // opt-in (TypeId 末尾 append =39)。子エンティティに置いて質量中心からずらすのが本来の
    // 使い方 (Components.h の設計コメント参照)
    RegisterComponent<AeroSurfaceComponent>("AeroSurface", {
        MYE_JP("法線", MYE_FIELD_TIP(AeroSurfaceComponent, normal, Float3,
                                     "local normal; positive angle of attack lifts this way")),
        MYE_JP("翼面積", MYE_FIELD_RANGE(AeroSurfaceComponent, area, Float, 0.0f, 1000.0f)),
        MYE_JP("揚力傾斜", MYE_FIELD_TIP(AeroSurfaceComponent, liftSlope, Float,
                                         "dCL/d(sin alpha); 2*pi is thin-airfoil theory")),
        MYE_JP("失速角 (度)",
               MYE_FIELD_RANGE(AeroSurfaceComponent, stallAngleDeg, Float, 0.0f, 89.0f)),
        MYE_JP("有害抗力 CD0",
               MYE_FIELD_RANGE(AeroSurfaceComponent, dragCoefficient, Float, 0.0f, 10.0f)),
        MYE_JP("誘導抗力", MYE_FIELD_RANGE(AeroSurfaceComponent, inducedDrag, Float, 0.0f, 10.0f)),
        MYE_JP("失速時抗力",
               MYE_FIELD_RANGE(AeroSurfaceComponent, stalledDrag, Float, 0.0f, 10.0f)),
    });

    // M60a: 関節。**hash 対象** — broken が sim 状態で、拘束が velocity / angularVelocity /
    // LocalTransform を駆動する。opt-in (TypeId 末尾 append =40)。type は「どの拘束行を
    // 立てるか」のプリセットで、M60a が実際に立てるのは Ball の 3 本だけ (残りは M60b/c)
    RegisterComponent<JointComponent>("Joint", {
        MYE_JP("接続先", MYE_FIELD_TIP(JointComponent, connectedEntity, EntityRef,
                                       "empty pins this body to a fixed point in the world")),
        MYE_JP("種類", MYE_FIELD_TIP(JointComponent, type, Int32,
                                     "0=Ball 1=Hinge 2=Fixed 3=Slider 4=Cone")),
        MYE_JP("アンカー", MYE_FIELD_TIP(JointComponent, anchor, Float3,
                                         "attach point in this entity's local space")),
        MYE_JP("接続先アンカー",
               MYE_FIELD_TIP(JointComponent, connectedAnchor, Float3,
                             "attach point in the other entity's local space "
                             "(world space when no entity is connected)")),
        MYE_JP("軸", MYE_FIELD_TIP(JointComponent, axis, Float3,
                                   "local axis: hinge spin / slider glide / cone center")),
        MYE_JP("リミットを使う", MYE_FIELD_TIP(JointComponent, useLimit, Bool,
                                               "off leaves every limit row out (a cone becomes "
                                               "a plain ball joint)")),
        MYE_JP("リミット下限", MYE_FIELD_TIP(JointComponent, limitMin, Float,
                                             "hinge/cone twist: degrees, slider: metres. "
                                             "measured on the owner, from the rest rotation")),
        MYE_JP("リミット上限", MYE_FIELD_TIP(JointComponent, limitMax, Float,
                                             "an inverted range (max below min) is left free")),
        MYE_JP("スイング角 (度)",
               MYE_FIELD_RANGE(JointComponent, swingLimitDeg, Float, 0.0f, 179.0f)),
        MYE_JP("モータ目標速度",
               MYE_FIELD_TIP(JointComponent, motorTargetVelocity, Float,
                             "positive drives the owner along +axis. a limit always wins over "
                             "the motor")),
        MYE_JP("モータ最大力", MYE_FIELD_TIP(JointComponent, motorMaxForce, Float,
                                             "0 or below leaves the motor row out entirely")),
        MYE_JP("破断力", MYE_FIELD_TIP(JointComponent, breakForce, Float,
                                       "average reaction force over one tick, in newtons. "
                                       "0 or below never breaks")),
        MYE_JP("破断トルク", MYE_FIELD_TIP(JointComponent, breakTorque, Float,
                                           "reaction torque only - a motor never breaks the "
                                           "joint it drives. 0 or below never breaks")),
        MYE_JP("破断済み", MYE_FIELD_TIP(JointComponent, broken, Bool,
                                         "sim state: a broken joint stops constraining")),
        // M60b: 角度自由度の基準になる相対回転 (末尾 append)
        MYE_JP("基準の相対回転",
               MYE_FIELD_TIP(JointComponent, restRotation, Quat,
                             "relative rotation that counts as 'at rest'; identity means the "
                             "two local frames line up")),
        // M60j: 繋がったペアの接触を外す (末尾 append)
        MYE_JP("接続先と衝突しない",
               MYE_FIELD_TIP(JointComponent, disableCollision, Bool,
                             "drops just this one pair from the broad phase; neighbours two "
                             "links away still collide, and a broken joint collides again")),
    });

    // M60g1: ラグドール。**hash 対象** — active が「アニメが骨を駆動するか / 物理が駆動するか」
    // を切り替え、物理の収集分岐 (部位を kinematic として扱うか) がこれを読む。
    // SkinnedMesh 側に付く札で、骨の実体は Part を持つ直子エンティティ (TypeId 末尾 append =41)
    RegisterComponent<RagdollComponent>("Ragdoll", {
        MYE_JP("物理駆動", MYE_FIELD_TIP(RagdollComponent, active, Bool,
                                         "on hands the bones to the rigid bodies; off lets the "
                                         "animation drive them and holds the bodies kinematic")),
    });

    // M60h1: 車輪 (レイキャストサスペンション)。**hash 対象** — 親剛体の velocity /
    // angularVelocity を駆動し、出力 2 本もソルバが毎 tick 書く sim 状態。
    // opt-in (TypeId 末尾 append =42)。車体の**子**に置いて質量中心からずらすのが本来の
    // 使い方 (AeroSurface と同じ「子に置くとレバー腕が生まれる」設計)
    RegisterComponent<WheelComponent>("Wheel", {
        MYE_JP("サス静止長", MYE_FIELD_TIP(WheelComponent, restLength, Float,
                                           "unloaded distance from the mount point down to the "
                                           "wheel centre; the ray is this plus the radius")),
        MYE_JP("ばね定数", MYE_FIELD_TIP(WheelComponent, stiffness, Float,
                                         "N/m. the resting sag is mg/k - raise the substeps "
                                         "before raising this")),
        MYE_JP("ダンパ", MYE_FIELD_RANGE(WheelComponent, damping, Float, 0.0f, 1000000.0f)),
        MYE_JP("車輪半径", MYE_FIELD_RANGE(WheelComponent, radius, Float, 0.0f, 100.0f)),
        MYE_JP("最大圧縮", MYE_FIELD_TIP(WheelComponent, maxCompression, Float,
                                         "bottoming out, in metres. 0 or below leaves the "
                                         "travel unlimited")),
        MYE_JP("接地している",
               MYE_FIELD_FLAGS(WheelComponent, isGrounded, Int32, kFieldReadOnly)),
        MYE_JP("圧縮量", MYE_FIELD_FLAGS(WheelComponent, compression, Float, kFieldReadOnly)),
        // M60h2: タイヤ (末尾 append)。効くのは車体に Vehicle があるときだけ
        MYE_JP("ステア追従率", MYE_FIELD_TIP(WheelComponent, steerFactor, Float,
                                             "how much of the vehicle steer input this wheel "
                                             "follows: 1 for the front axle, 0 for the rear")),
        MYE_JP("駆動配分", MYE_FIELD_RANGE(WheelComponent, driveFactor, Float, -1.0f, 1.0f)),
        MYE_JP("制動配分", MYE_FIELD_RANGE(WheelComponent, brakeFactor, Float, 0.0f, 1.0f)),
        MYE_JP("コーナリングパワー",
               MYE_FIELD_TIP(WheelComponent, corneringStiffness, Float,
                             "lateral force per radian of slip, in N/rad, saturated at mu*N")),
        MYE_JP("タイヤ摩擦", MYE_FIELD_TIP(WheelComponent, friction, Float,
                                           "combined with the ground material as sqrt(a*b), "
                                           "same rule as a contact")),
        MYE_JP("転がり抵抗", MYE_FIELD_TIP(WheelComponent, rollingResistance, Float,
                                           "combined with the ground material as max(a, b)")),
        MYE_JP("切れ角 (rad)",
               MYE_FIELD_FLAGS(WheelComponent, steerAngle, Float, kFieldReadOnly)),
        MYE_JP("回転角 (rad)",
               MYE_FIELD_FLAGS(WheelComponent, rotationAngle, Float, kFieldReadOnly)),
    });

    // M60h2: 車両。**hash 対象** — steer / throttle / brake が sim 状態の運転入力で、
    // タイヤ力が velocity / angularVelocity を駆動する。opt-in (TypeId 末尾 append =43)。
    // ★入力を ABI ではなくフィールドに置いたので **ABI 追加ゼロ** (スクリプトは既存の
    //   SetComponentField で書く)。同時に「この車体の車輪はタイヤ摩擦を持つ」の宣言も
    //   兼ねていて、Vehicle が無ければ車輪は M60h1 のサスだけ = ビット同一の経路を通る
    RegisterComponent<VehicleComponent>("Vehicle", {
        MYE_JP("ステア入力", MYE_FIELD_TIP(VehicleComponent, steer, Float,
                                           "-1..1, positive steers right. sim state: scripts "
                                           "write it with SetComponentField")),
        MYE_JP("スロットル", MYE_FIELD_TIP(VehicleComponent, throttle, Float,
                                           "-1..1, negative drives in reverse")),
        MYE_JP("ブレーキ", MYE_FIELD_RANGE(VehicleComponent, brake, Float, 0.0f, 1.0f)),
        MYE_JP("最大切れ角 (度)",
               MYE_FIELD_RANGE(VehicleComponent, maxSteerAngleDeg, Float, 0.0f, 89.0f)),
        MYE_JP("駆動力", MYE_FIELD_TIP(VehicleComponent, motorForce, Float,
                                       "newtons per driven wheel at full throttle")),
        MYE_JP("制動力", MYE_FIELD_TIP(VehicleComponent, brakeForce, Float,
                                       "newtons per braked wheel at full brake")),
    });

    // M60'c: ロープ (XPBD 変形体第 1 号)。状態は XpbdBackend の池 (ECS 外 sim 状態) に
    // 住み、ここはオーサリングのみ。フィールドは **hash 対象** (opt-in、TypeId 末尾
    // append =44)。池の組み直し条件は「粒子数 (segmentCount+1) の不一致」だけ —
    // snapshot 復元後の Sync が池を壊さないための規約 (XpbdBackend::Sync 参照)
    RegisterComponent<RopeComponent>("Rope", {
        MYE_JP("分割数", MYE_FIELD_RANGE(RopeComponent, segmentCount, Int32, 1.0f, 256.0f)),
        MYE_JP("全長", MYE_FIELD_TIP(RopeComponent, length, Float,
                                     "metres, split evenly into rest lengths at build time; "
                                     "editing it later takes effect on the next rebuild")),
        MYE_JP("半径", MYE_FIELD_RANGE(RopeComponent, radius, Float, 0.001f, 10.0f)),
        MYE_JP("質量", MYE_FIELD_TIP(RopeComponent, mass, Float,
                                     "total mass in kg, spread evenly over the particles at "
                                     "build time")),
        MYE_JP("コンプライアンス",
               MYE_FIELD_TIP(RopeComponent, compliance, Float,
                             "1/stiffness in m/N, read live every tick; 0 keeps the rope "
                             "inextensible")),
        MYE_JP("減衰", MYE_FIELD_RANGE(RopeComponent, damping, Float, 0.0f, 1.0f)),
        MYE_JP("始端をピン", MYE_FIELD_TIP(RopeComponent, attachStart, Bool,
                                           "pins the first particle to this entity and follows "
                                           "it every tick. baked at build time")),
        MYE_JP("終端をピン", MYE_FIELD_TIP(RopeComponent, attachEnd, Bool,
                                           "pins the last particle at its build-time world "
                                           "position. baked at build time")),
        MYE_JP("終端の接続先",
               MYE_FIELD_TIP(RopeComponent, connectedEntity, EntityRef,
                             "reserved: the far end will attach to this body (not yet wired)")),
    });
}

} // namespace mye
