#include "Engine/Core/Components.h"

#include <cstddef> // offsetof

#include "Engine/Core/World.h"

namespace mye {

// 自分と**祖先すべて**を見る (M64b)。1 つでも無効なら無効。
//
// ★自エンティティしか見ないと、sim (スクリプト/衝突/物理/パーティクル) は親を無効にすれば
//   止まるのに**子の MeshRenderer だけは描かれ続ける** = 「親を消したのに見た目が残る」。
// ★親子の循環は `World::ApplySetParent` が拒否するので、この走査は必ず終わる。
// ★コストは「自分の Active 引き + 親の Hierarchy 引き」× 階層の深さ。全エンティティが
//   HierarchyComponent を必ず持つ (World の基本アーキタイプ) ので親引きは常にヒットし、
//   ルート 1 段なら Active 引き 1 回 + Hierarchy 引き 1 回で済む。
bool IsEntityActive(World& world, EntityID e)
{
    for (EntityID cur = e; !cur.IsNull(); cur = world.GetParent(cur)) {
        const auto* a = world.GetComponent<ActiveComponent>(cur);
        if (a != nullptr && !a->enabled) {
            return false;
        }
    }
    return true;
}

void RegisterBuiltinComponents()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    // 登録順 = TypeId。順序を変えるとシーン互換とリプレイ互換が壊れるため、
    // 追加は必ず末尾に行うこと。
    // ★末尾に足した opt-in の型 (無ければ何もしない) は、持つエンティティが既存シーンに居ないので
    //   既存シーンのワールドハッシュを 1 バイトも変えない = ReplayFile の bump は要らない。
    //   既存の型の末尾へフィールドを足した場合は別で、hash 対象ならハッシュ値は変わる (.rep は毎回
    //   録り直しの使い捨てなので bump はしない)、生バイトが伸びるので kSimSnapshotVersion は上げる。
    //   以下の各登録には、型ごとに違う理由 (hash 対象か NoHash か、など) だけを書く
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
        MYE_JP("メインカメラ", MYE_FIELD(CameraComponent, isPrimary, Bool)),
    });

    RegisterComponent<LightComponent>("Light", {
        MYE_JP("色", MYE_FIELD(LightComponent, color, Float3)),
        MYE_JP("強度", MYE_FIELD(LightComponent, intensity, Float)),
        MYE_JP("環境光", MYE_FIELD(LightComponent, ambient, Float3)),
        MYE_JP("種類", MYE_FIELD(LightComponent, type, Int32)),
        MYE_JP("範囲", MYE_FIELD(LightComponent, range, Float)),
        MYE_JP("スポット内角 (度)", MYE_FIELD(LightComponent, spotInnerDeg, Float)),
        MYE_JP("スポット外角 (度)", MYE_FIELD(LightComponent, spotOuterDeg, Float)),
        MYE_JP("影を落とす", MYE_FIELD(LightComponent, castShadow, Bool)), // M54b (点/スポット用)
        MYE_JP("安全半径", MYE_FIELD(LightComponent, safeRadius, Float)),
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
        MYE_JP("再生中", MYE_FIELD(ParticleEmitterComponent, playing, Bool)),
        MYE_JP("長さ (tick)", MYE_FIELD(ParticleEmitterComponent, durationTicks, Int32)),
        MYE_JP("ループ", MYE_FIELD(ParticleEmitterComponent, looping, Bool)),
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
        // M42e: GPU 深度衝突 (末尾 append)
        MYE_JP("深度バッファ衝突", MYE_FIELD(ParticleEmitterComponent, depthCollision, Bool)),
        MYE_JP("衝突時の反発", MYE_FIELD_RANGE(ParticleEmitterComponent, collisionBounce, Float, 0.0f, 1.0f)),
        // M61a: A群拡張 (末尾 append)。既定値では拡張を使わない経路とビット同一
        MYE_JP("速度の継承", MYE_FIELD(ParticleEmitterComponent, velocityInheritance, Float)),
        MYE_JP("シミュレーション空間", MYE_FIELD(ParticleEmitterComponent, simulationSpace, Int32)),
        MYE_JP("プリウォーム (秒)", MYE_FIELD(ParticleEmitterComponent, prewarmTime, Float)),
        MYE_JP("サブフレーム放出", MYE_FIELD(ParticleEmitterComponent, subframeEmission, Bool)),
        MYE_JP("乱流モード", MYE_FIELD(ParticleEmitterComponent, turbulenceMode, Int32)),
        MYE_JP("ノイズ周波数", MYE_FIELD(ParticleEmitterComponent, noiseFrequency, Float)),
        MYE_JP("ノイズ速度", MYE_FIELD(ParticleEmitterComponent, noiseSpeed, Float)),
        MYE_JP("放出元", MYE_FIELD(ParticleEmitterComponent, emitFrom, Int32)),
        // M63a: B群 = 描画表現力 (末尾 append)。既定値では B 群を使わない経路とビット同一
        MYE_JP("回転 (最小)", MYE_FIELD(ParticleEmitterComponent, rotationMin, Float)),
        MYE_JP("回転 (最大)", MYE_FIELD(ParticleEmitterComponent, rotationMax, Float)),
        MYE_JP("角速度 (最小)", MYE_FIELD(ParticleEmitterComponent, rotationSpeedMin, Float)),
        MYE_JP("角速度 (最大)", MYE_FIELD(ParticleEmitterComponent, rotationSpeedMax, Float)),
        MYE_JP("速度ストレッチ", MYE_FIELD(ParticleEmitterComponent, stretchScale, Float)),
        MYE_JP("ストレッチ上限", MYE_FIELD(ParticleEmitterComponent, stretchMax, Float)),
        MYE_JP("フリップ FPS", MYE_FIELD(ParticleEmitterComponent, flipFps, Float)),
        MYE_JP("フリップ補間", MYE_FIELD(ParticleEmitterComponent, flipBlend, Bool)),
        MYE_JP("フリップ開始をランダム化", MYE_FIELD(ParticleEmitterComponent, flipRandomStart, Bool)),
        MYE_JP("ライティング", MYE_FIELD(ParticleEmitterComponent, lightingMode, Int32)),
        MYE_JP("ラップ拡散", MYE_FIELD_RANGE(ParticleEmitterComponent, lightWrap, Float, 0.0f, 1.0f)),
        MYE_JP("受光の強さ", MYE_FIELD(ParticleEmitterComponent, lightIntensity, Float)),
        MYE_JP("影を受ける", MYE_FIELD(ParticleEmitterComponent, lightReceiveShadow, Bool)),
        MYE_JP("衝突の厚み", MYE_FIELD(ParticleEmitterComponent, collisionThickness, Float)),
        MYE_JP("衝突時の摩擦", MYE_FIELD_RANGE(ParticleEmitterComponent, collisionFriction, Float, 0.0f, 1.0f)),
        MYE_JP("衝突時の寿命損失", MYE_FIELD_RANGE(ParticleEmitterComponent, collisionLifeLoss, Float, 0.0f, 1.0f)),
        MYE_JP("解析床と衝突", MYE_FIELD(ParticleEmitterComponent, collisionFloor, Bool)),
        MYE_JP("解析床の高さ", MYE_FIELD(ParticleEmitterComponent, collisionFloorY, Float)),
    });

    // M28a: height / friction、M36a: layer / mask / meshAsset を末尾 append
    // (フィールド順変更なし = シーン互換維持。既存シーンは欠損フィールドをデフォルト値でロード。
    //  hash 対象)
    RegisterComponent<ColliderComponent>("Collider", {
        MYE_JP("形状", MYE_FIELD(ColliderComponent, shape, Int32)),
        MYE_JP("半径", MYE_FIELD(ColliderComponent, radius, Float)),
        MYE_JP("ハーフサイズ", MYE_FIELD(ColliderComponent, halfExtents, Float3)),
        MYE_JP("トリガー", MYE_FIELD(ColliderComponent, isTrigger, Bool)),
        MYE_JP("高さ", MYE_FIELD(ColliderComponent, height, Float)),
        MYE_JP("摩擦", MYE_FIELD(ColliderComponent, friction, Float)),
        MYE_JP("レイヤー", MYE_FIELD_TIP(ColliderComponent, layer, Int32, "collision layer 0..31")),
        MYE_JP("衝突マスク", MYE_FIELD_TIP(ColliderComponent, mask, UInt32, "layers this collider hits (bitmask)")),
        MYE_JP("メッシュアセット", MYE_FIELD(ColliderComponent, meshAsset, AssetRef)), // shape=3 静的メッシュ / shape=4 .terrain.json
        // M59a2: 物理マテリアル (末尾 append)。未割当の挙動のビット同一は
        // PhysicsSelfTest の [phys] body ビットパターン照合が見る (ワールドハッシュ値は変わるため)
        MYE_JP("物理マテリアル", MYE_FIELD(ColliderComponent, physMaterial, AssetRef)),
        MYE_JP("材料の上書き", MYE_FIELD(ColliderComponent, materialOverrideBits, UInt32)),
    });

    // M10: 無ければ有効扱い
    RegisterComponent<ActiveComponent>("Active", {
        MYE_JP("有効", MYE_FIELD(ActiveComponent, enabled, Bool)),
    });

    // M13: プレハブタグ。純データ (どのシステムにも参加しない = sim 非影響)。
    // kComponentHidden で Inspector の Add/一覧から隠すが、シリアライズ+ハッシュはされる。
    RegisterComponent<PrefabInstanceComponent>("PrefabInstance", {
        MYE_JP("プレハブハッシュ", MYE_FIELD_FLAGS(PrefabInstanceComponent, prefabHash, UInt64, kFieldReadOnly)),
        // M48c: 末尾追加
        MYE_JP("外側ローカル ID", MYE_FIELD_FLAGS(PrefabInstanceComponent, outerLocalId, UInt64, kFieldReadOnly)),
    }, kComponentHidden);

    RegisterComponent<PrefabLinkComponent>("PrefabLink", {
        MYE_JP("ローカル ID", MYE_FIELD_FLAGS(PrefabLinkComponent, localId, UInt64, kFieldReadOnly)),
    }, kComponentHidden);

    // M14: アニメータ。無ければ何もしない
    RegisterComponent<AnimatorComponent>("Animator", {
        MYE_JP("クリップ", MYE_FIELD(AnimatorComponent, clip, AssetRef)),
        MYE_JP("再生位置 (tick)", MYE_FIELD_FLAGS(AnimatorComponent, timeTicks, Int32, kFieldReadOnly)),
        MYE_JP("速度", MYE_FIELD(AnimatorComponent, speed, Int32)),
        MYE_JP("ループ", MYE_FIELD(AnimatorComponent, loop, Bool)),
        MYE_JP("再生中", MYE_FIELD(AnimatorComponent, playing, Bool)),
    });

    // M18: スケルタルスキニング。ポーズは描画専用なので **kComponentNoHash**。
    // M18 追補: loop / fadeTicks (設定) と、クロスフェードの再生状態 5 本 (末尾)。
    // 再生状態は SkinningSystem が毎 tick 書く値なので **シーンに保存しない** (kFieldNoSerialize)。
    // 保存すると「編集中に clip を変えて保存」したシーンが、再生開始の 1 tick 目に古い clip から
    // フェードしてしまう。生バイトは snapshot に載る
    RegisterComponent<SkinnedMeshComponent>("SkinnedMesh", {
        MYE_JP("モデル", MYE_FIELD(SkinnedMeshComponent, model, AssetRef)),
        MYE_JP("クリップ", MYE_FIELD(SkinnedMeshComponent, clip, Int32)),
        MYE_JP("再生位置 (tick)", MYE_FIELD_FLAGS(SkinnedMeshComponent, timeTicks, Int32, kFieldReadOnly)),
        MYE_JP("再生中", MYE_FIELD(SkinnedMeshComponent, playing, Bool)),
        MYE_JP("ループ", MYE_FIELD(SkinnedMeshComponent, loop, Bool)),
        MYE_JP("クロスフェード (tick)", MYE_FIELD(SkinnedMeshComponent, fadeTicks, Int32)),
        MYE_JP("観測したクリップ", MYE_FIELD_FLAGS(SkinnedMeshComponent, observedClip, Int32, kFieldReadOnly | kFieldNoSerialize)),
        MYE_JP("フェード元クリップ", MYE_FIELD_FLAGS(SkinnedMeshComponent, fromClip, Int32, kFieldReadOnly | kFieldNoSerialize)),
        MYE_JP("フェード元の再生位置 (tick)", MYE_FIELD_FLAGS(SkinnedMeshComponent, fromTimeTicks, Int32, kFieldReadOnly | kFieldNoSerialize)),
        MYE_JP("フェード経過 (tick)", MYE_FIELD_FLAGS(SkinnedMeshComponent, fadeElapsed, Int32, kFieldReadOnly | kFieldNoSerialize)),
        MYE_JP("フェード長 (tick)", MYE_FIELD_FLAGS(SkinnedMeshComponent, fadeTotal, Int32, kFieldReadOnly | kFieldNoSerialize)),
    }, kComponentNoHash);

    // M20: 剛体。velocity は積分される sim 状態なので **hash 対象** (kComponentNoHash を付けない)。
    // M28b: angularVelocity / angularDamping / freezeRotation を末尾 append。
    // angularVelocity は積分される sim 状態なので hash 対象 (velocity と同格)
    RegisterComponent<RigidbodyComponent>("Rigidbody", {
        MYE_JP("速度", MYE_FIELD(RigidbodyComponent, velocity, Float3)),
        MYE_JP("質量", MYE_FIELD(RigidbodyComponent, mass, Float)),
        MYE_JP("移動の減衰", MYE_FIELD(RigidbodyComponent, linearDamping, Float)),
        MYE_JP("反発", MYE_FIELD(RigidbodyComponent, restitution, Float)),
        MYE_JP("重力スケール", MYE_FIELD(RigidbodyComponent, gravityScale, Float)),
        MYE_JP("キネマティック", MYE_FIELD(RigidbodyComponent, isKinematic, Bool)),
        MYE_JP("角速度", MYE_FIELD(RigidbodyComponent, angularVelocity, Float3)),
        MYE_JP("回転の減衰", MYE_FIELD(RigidbodyComponent, angularDamping, Float)),
        MYE_JP("回転を固定", MYE_FIELD(RigidbodyComponent, freezeRotation, Bool)),
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

    // M21: ゲーム内 UI。描画専用なので **kComponentNoHash**。
    // serialize はされる (UI をシーン保存/Inspector 編集可能)。
    // ★配置 (anchor / x / y / w / h / space) は RectTransform が持つ (M75a)。旧シーンの
    //   同名キーは SceneSerializer::ReadEntityComponents が拾って RectTransform に変換する
    //   (ここにフィールドを置くと二重管理になる — 変換の正本は uilayout::FromLegacyRect の 1 本)
    RegisterComponent<UIElementComponent>("UIElement", {
        MYE_JP("種類", MYE_FIELD(UIElementComponent, kind, Int32)),
        MYE_JP("色", MYE_FIELD(UIElementComponent, color, Color)),
        MYE_JP("テクスチャ", MYE_FIELD(UIElementComponent, texture, AssetRef)),
        MYE_JP("文字サイズ", MYE_FIELD(UIElementComponent, fontScale, Float)),
        MYE_JP("描画順", MYE_FIELD(UIElementComponent, order, Int32)),
        MYE_JP("テキスト", MYE_FIELD(UIElementComponent, text, String256)),
        // M35 拡張 (末尾 append)
        MYE_JP("フィル量", MYE_FIELD_RANGE(UIElementComponent, fillAmount, Float, 0.0f, 1.0f)),
        MYE_JP("フィル方向", MYE_FIELD_TIP(UIElementComponent, fillMode, Int32, "0=off 1=horizontal 2=vertical")),
        MYE_JP("スライス境界", MYE_FIELD_TIP(UIElementComponent, sliceBorder, Float4, "9-slice border px (l,t,r,b)")),
        MYE_JP("9 スライス", MYE_FIELD(UIElementComponent, sliced, Bool)),
        MYE_JP("フォーカス可", MYE_FIELD(UIElementComponent, focusable, Bool)),
        MYE_JP("フォーカス中", MYE_FIELD(UIElementComponent, focused, Bool)),
        // M51e 拡張 (末尾 append)
        MYE_JP("子をクリップ", MYE_FIELD(UIElementComponent, clipChildren, Bool)),
        MYE_JP("文字整列", MYE_FIELD_TIP(UIElementComponent, align, Int32, "9-grid 0..8 (text only)")),
        MYE_JP("折返し", MYE_FIELD_TIP(UIElementComponent, wrap, Int32, "0=off 1=char wrap at width")),
        // ワールド追従 UI (末尾 append、NoHash)。追従は自動判定 — これらは追従要素の見た目調整
        MYE_JP("距離で縮む", MYE_FIELD_TIP(UIElementComponent, distanceScale, Bool,
                                           "scale by camera distance (world-attached UI only)")),
        MYE_JP("等倍距離 (m)", MYE_FIELD_TIP(UIElementComponent, distanceRef, Float,
                                             "distance at which scale = 1.0")),
        MYE_JP("画面内にクランプ", MYE_FIELD_TIP(UIElementComponent, clampToScreen, Bool,
                                                 "keep the rect on screen (world-attached UI only)")),
    }, kComponentNoHash | kComponentUiAux);

    // M22: Animator Controller。LocalTransform を駆動するので **hash 対象** (kComponentNoHash 無し)。
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
    RegisterComponent<ConstantForceComponent>("ConstantForce", {
        MYE_JP("力", MYE_FIELD(ConstantForceComponent, force, Float3)),
        MYE_JP("トルク", MYE_FIELD(ConstantForceComponent, torque, Float3)),
        MYE_JP("ローカル座標系", MYE_FIELD(ConstantForceComponent, relative, Int32)),
    });

    // M29a: 距離バネジョイント。速度を駆動するので **hash 対象**。
    // connectedEntity は EntityRef → シーン保存は fileId 変換、プレハブは既存 remap が面倒を見る
    RegisterComponent<SpringJointComponent>("SpringJoint", {
        MYE_JP("接続先", MYE_FIELD(SpringJointComponent, connectedEntity, EntityRef)),
        MYE_JP("自然長", MYE_FIELD_RANGE(SpringJointComponent, restLength, Float, 0.0f, 1000.0f)),
        MYE_JP("ばね定数", MYE_FIELD_TIP(SpringJointComponent, stiffness, Float,
                      "安定条件: stiffness*dt^2/mass < 4 (dt=1/60 → mass=1 で k < 14400)")),
        MYE_JP("減衰", MYE_FIELD_RANGE(SpringJointComponent, damping, Float, 0.0f, 100000.0f)),
    });

    // M29b: キャラクターコントローラ。LocalTransform を駆動する sim 状態なので **hash 対象**。
    RegisterComponent<CharacterControllerComponent>("CharacterController", {
        MYE_JP("半径", MYE_FIELD_RANGE(CharacterControllerComponent, radius, Float, 0.01f, 10.0f)),
        MYE_JP("高さ", MYE_FIELD_RANGE(CharacterControllerComponent, height, Float, 0.1f, 20.0f)),
        MYE_JP("登れる傾斜 (度)", MYE_FIELD_RANGE(CharacterControllerComponent, slopeLimitDeg, Float, 0.0f, 89.0f)),
        MYE_JP("スキン幅", MYE_FIELD_RANGE(CharacterControllerComponent, skinWidth, Float, 0.0f, 0.5f)),
        MYE_JP("重力スケール", MYE_FIELD(CharacterControllerComponent, gravityScale, Float)),
        MYE_JP("移動入力", MYE_FIELD(CharacterControllerComponent, moveInput, Float3)),
        MYE_JP("速度", MYE_FIELD_FLAGS(CharacterControllerComponent, velocity, Float3, kFieldReadOnly)),
        MYE_JP("ジャンプ速度", MYE_FIELD_FLAGS(CharacterControllerComponent, jumpSpeed, Float, kFieldHidden)),
        MYE_JP("接地している", MYE_FIELD_FLAGS(CharacterControllerComponent, isGrounded, Bool, kFieldReadOnly)),
    });

    // M29c: スプライト/トレイル/3D テキスト。描画専用なので **kComponentNoHash**。
    // serialize はされる
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
        MYE_JP("放出中", MYE_FIELD(TrailRendererComponent, emitting, Bool)),
    }, kComponentNoHash);

    RegisterComponent<TextMeshComponent>("TextMesh", {
        MYE_JP("テキスト", MYE_FIELD(TextMeshComponent, text, String256)),
        MYE_JP("文字サイズ", MYE_FIELD_RANGE(TextMeshComponent, fontScale, Float, 0.05f, 50.0f)),
        MYE_JP("色", MYE_FIELD(TextMeshComponent, color, Color)),
        MYE_JP("ビルボード", MYE_FIELD(TextMeshComponent, billboardMode, Int32)),
    }, kComponentNoHash);

    // M29d: スカイボックス/フォグ。描画専用なので **kComponentNoHash** (末尾にフィールドを
    // 足してもハッシュは変わらない)
    RegisterComponent<SkyboxComponent>("Skybox", {
        MYE_JP("モード", MYE_FIELD(SkyboxComponent, mode, Int32)),
        MYE_JP("上の色", MYE_FIELD(SkyboxComponent, topColor, Color)),
        MYE_JP("地平線の色", MYE_FIELD(SkyboxComponent, horizonColor, Color)),
        MYE_JP("下の色", MYE_FIELD(SkyboxComponent, bottomColor, Color)),
        MYE_JP("テクスチャ", MYE_FIELD(SkyboxComponent, cubemapTexture, AssetRef)),
        // 2026-09-14: 星空と環境光の切り離し (末尾 append。欠けた古いシーンは既定値 = 星なし + 空の色で環境光)
        MYE_JP("星の密度", MYE_FIELD_RANGE(SkyboxComponent, starDensity, Float, 0.0f, 1.0f)),
        MYE_JP("星の明るさ", MYE_FIELD_RANGE(SkyboxComponent, starBrightness, Float, 0.0f, 20.0f)),
        MYE_JP("星の瞬き", MYE_FIELD_RANGE(SkyboxComponent, starTwinkle, Float, 0.0f, 1.0f)),
        MYE_JP("星の細かさ", MYE_FIELD_RANGE(SkyboxComponent, starCells, Int32, 1.0f, 1024.0f)),
        MYE_JP("環境光に使う", MYE_FIELD(SkyboxComponent, lighting, Bool)),
    }, kComponentNoHash);

    RegisterComponent<FogComponent>("Fog", {
        MYE_JP("モード", MYE_FIELD(FogComponent, mode, Int32)),
        MYE_JP("色", MYE_FIELD(FogComponent, color, Color)),
        MYE_JP("濃度", MYE_FIELD_RANGE(FogComponent, density, Float, 0.0f, 1.0f)),
        MYE_JP("開始", MYE_FIELD(FogComponent, start, Float)),
        MYE_JP("終了", MYE_FIELD(FogComponent, end, Float)),
        // M43a: ハイトフォグ + 太陽インスキャッタ (末尾 append)
        MYE_JP("高度減衰", MYE_FIELD_RANGE(FogComponent, heightFalloff, Float, 0.0f, 4.0f)),
        MYE_JP("基準高度", MYE_FIELD(FogComponent, baseHeight, Float)),
        MYE_JP("インスキャッタ強度", MYE_FIELD_RANGE(FogComponent, inscatterIntensity, Float, 0.0f, 1.0f)),
        MYE_JP("インスキャッタ指数", MYE_FIELD_RANGE(FogComponent, inscatterPower, Float, 1.0f, 64.0f)),
    }, kComponentNoHash);

    // M29e: カメラ別ポストプロセス。描画専用なので **kComponentNoHash** (末尾にフィールドを
    // 足してもハッシュは変わらない)
    RegisterComponent<CameraPostFxComponent>("CameraPostFx", {
        MYE_JP("露出", MYE_FIELD_RANGE(CameraPostFxComponent, exposure, Float, 0.0f, 16.0f)),
        MYE_JP("トーンマップ", MYE_FIELD(CameraPostFxComponent, tonemapMode, Int32)),
        MYE_JP("ブルーム", MYE_FIELD(CameraPostFxComponent, bloomOn, Bool)),
        MYE_JP("ブルームしきい値", MYE_FIELD(CameraPostFxComponent, bloomThreshold, Float)),
        MYE_JP("ブルーム強度", MYE_FIELD(CameraPostFxComponent, bloomIntensity, Float)),
        MYE_JP("FXAA", MYE_FIELD(CameraPostFxComponent, fxaaOn, Bool)),
        // M32d: 色収差 / ビネット / カラーグレーディング (末尾 append)
        MYE_JP("色収差", MYE_FIELD_RANGE(CameraPostFxComponent, chromAberration, Float, 0.0f, 0.05f)),
        MYE_JP("ビネット強度", MYE_FIELD_RANGE(CameraPostFxComponent, vignetteIntensity, Float, 0.0f, 1.0f)),
        MYE_JP("ビネット半径", MYE_FIELD_RANGE(CameraPostFxComponent, vignetteRadius, Float, 0.0f, 1.0f)),
        MYE_JP("彩度", MYE_FIELD_RANGE(CameraPostFxComponent, saturation, Float, 0.0f, 4.0f)),
        MYE_JP("コントラスト", MYE_FIELD_RANGE(CameraPostFxComponent, contrast, Float, 0.0f, 4.0f)),
        MYE_JP("カラーフィルタ", MYE_FIELD(CameraPostFxComponent, colorFilter, Color)),
        // M40d: SSAO パラメータ (末尾 append)
        MYE_JP("SSAO 半径", MYE_FIELD_RANGE(CameraPostFxComponent, ssaoRadius, Float, 0.05f, 4.0f)),
        MYE_JP("SSAO 強度", MYE_FIELD_RANGE(CameraPostFxComponent, ssaoIntensity, Float, 0.0f, 4.0f)),
        // M43b: ゴッドレイ (末尾 append)
        MYE_JP("ゴッドレイ強度", MYE_FIELD_RANGE(CameraPostFxComponent, godrayIntensity, Float, 0.0f, 4.0f)),
        MYE_JP("ゴッドレイ減衰", MYE_FIELD_RANGE(CameraPostFxComponent, godrayDecay, Float, 0.5f, 0.999f)),
        // M44a: カラーグレーディング LUT (末尾 append)
        MYE_JP("LUT テクスチャ", MYE_FIELD(CameraPostFxComponent, lutTexture, AssetRef)),
        MYE_JP("LUT 強度", MYE_FIELD_RANGE(CameraPostFxComponent, lutIntensity, Float, 0.0f, 1.0f)),
        // M44b: 自動露出 (末尾 append)
        MYE_JP("自動露出", MYE_FIELD(CameraPostFxComponent, autoExposure, Bool)),
        MYE_JP("自動露出の追従速度", MYE_FIELD_RANGE(CameraPostFxComponent, aeSpeed, Float, 0.1f, 20.0f)),
        MYE_JP("自動露出の下限", MYE_FIELD_RANGE(CameraPostFxComponent, aeMin, Float, 0.01f, 1.0f)),
        MYE_JP("自動露出の上限", MYE_FIELD_RANGE(CameraPostFxComponent, aeMax, Float, 1.0f, 16.0f)),
        // M44c: 被写界深度 (末尾 append)
        MYE_JP("被写界深度: 合焦距離", MYE_FIELD_RANGE(CameraPostFxComponent, dofFocusDistance, Float, 0.1f, 500.0f)),
        MYE_JP("被写界深度: 合焦幅", MYE_FIELD_RANGE(CameraPostFxComponent, dofFocusRange, Float, 0.1f, 100.0f)),
        MYE_JP("被写界深度: 最大ボケ半径", MYE_FIELD_RANGE(CameraPostFxComponent, dofMaxRadius, Float, 0.0f, 32.0f)),
        // M44d: カメラモーションブラー (末尾 append)
        MYE_JP("モーションブラー強度", MYE_FIELD_RANGE(CameraPostFxComponent, motionBlurIntensity, Float, 0.0f, 1.0f)),
        MYE_JP("モーションブラー最大画素", MYE_FIELD_RANGE(CameraPostFxComponent, mbMaxPixels, Float, 1.0f, 64.0f)),
        // M55d: TAA (末尾 append)。Deferred のみ効く
        MYE_JP("TAA", MYE_FIELD(CameraPostFxComponent, taaOn, Bool)),
        MYE_JP("TAA 履歴の残し率", MYE_FIELD_RANGE(CameraPostFxComponent, taaFeedback, Float, 0.0f, 0.95f)),
        // M56d: SSR (末尾 append)。Deferred のみ効く
        MYE_JP("SSR", MYE_FIELD(CameraPostFxComponent, ssrOn, Bool)),
        MYE_JP("SSR 最大粗さ", MYE_FIELD_RANGE(CameraPostFxComponent, ssrMaxRoughness, Float, 0.0f, 1.0f)),
        MYE_JP("SSR 強度", MYE_FIELD_RANGE(CameraPostFxComponent, ssrIntensity, Float, 0.0f, 2.0f)),
        // M57c: フロクセル・ボリュメトリック (末尾 append)
        MYE_JP("ボリュメトリック霧", MYE_FIELD(CameraPostFxComponent, froxelOn, Bool)),
        MYE_JP("霧の密度", MYE_FIELD_RANGE(CameraPostFxComponent, froxelDensity, Float, 0.0f, 0.5f)),
        MYE_JP("霧の異方性", MYE_FIELD_RANGE(CameraPostFxComponent, froxelAnisotropy, Float, -0.9f, 0.9f)),
        // M78c: プロジェクトポスト／コンピュートスタック (末尾 append)
        MYE_JP("エフェクトスタック", MYE_FIELD(CameraPostFxComponent, fxStack, AssetRef)),
    }, kComponentNoHash);

    // M32e: 合成エフェクトのライフサイクル。DestroyEntity + 子エミッタ playing を駆動 = hash 対象。
    RegisterComponent<EffectComponent>("Effect", {
        MYE_JP("長さ (tick)", MYE_FIELD(EffectComponent, durationTicks, Int32)),
        MYE_JP("余韻 (tick)", MYE_FIELD(EffectComponent, lingerTicks, Int32)),
        MYE_JP("経過 (tick)", MYE_FIELD_FLAGS(EffectComponent, elapsedTicks, Int32, kFieldReadOnly)),
        MYE_JP("再生中", MYE_FIELD(EffectComponent, playing, Bool)),
        MYE_JP("ループ", MYE_FIELD(EffectComponent, looping, Bool)),
        MYE_JP("再生後に破棄", MYE_FIELD(EffectComponent, autoDestroy, Bool)),
    });

    // M45e: 3D オーディオ。**出力 sink であり決定論レーン外なので kComponentNoHash**
    // (WorldHasher.cpp が NoHash を丸ごとスキップする)。
    RegisterComponent<AudioListenerComponent>("AudioListener", {
        MYE_JP("有効", MYE_FIELD(AudioListenerComponent, enabled, Bool)),
    }, kComponentNoHash);

    RegisterComponent<AudioSourceComponent>("AudioSource", {
        MYE_JP("サウンド", MYE_FIELD(AudioSourceComponent, sound, AssetRef)),
        MYE_JP("起動時に再生", MYE_FIELD(AudioSourceComponent, playOnAwake, Bool)),
        MYE_JP("ループ", MYE_FIELD(AudioSourceComponent, loop, Int32)),
        MYE_JP("音量", MYE_FIELD_RANGE(AudioSourceComponent, volume, Float, 0.0f, 1.0f)),
        MYE_JP("ピッチ", MYE_FIELD_RANGE(AudioSourceComponent, pitch, Float, 0.25f, 4.0f)),
        MYE_JP("ミュート", MYE_FIELD(AudioSourceComponent, mute, Bool)),
        MYE_JP("優先度", MYE_FIELD(AudioSourceComponent, priority, Int32)),
        MYE_JP("バス", MYE_FIELD(AudioSourceComponent, bus, String64)),
        MYE_JP("減衰を上書き", MYE_FIELD(AudioSourceComponent, overrideAttenuation, Bool)),
        MYE_JP("spatial blend", MYE_FIELD_RANGE(AudioSourceComponent, spatialBlend, Float, 0.0f, 1.0f)),
        MYE_JP("最小距離", MYE_FIELD_RANGE(AudioSourceComponent, minDistance, Float, 0.01f, 1000.0f)),
        MYE_JP("最大距離", MYE_FIELD_RANGE(AudioSourceComponent, maxDistance, Float, 0.02f, 10000.0f)),
        MYE_JP("減衰カーブ", MYE_FIELD(AudioSourceComponent, rolloff, Int32)),
        MYE_JP("ドップラー", MYE_FIELD_RANGE(AudioSourceComponent, dopplerScale, Float, 0.0f, 5.0f)),
        MYE_JP("リバーブ送り", MYE_FIELD_RANGE(AudioSourceComponent, reverbSend, Float, 0.0f, 1.0f)),
    }, kComponentNoHash);

    // M48f: 部位 (ソケット)。**hash 対象** — M48g の PartFollowSystem が LocalTransform を
    // 駆動する = sim 状態の入力になるため
    RegisterComponent<PartComponent>("Part", {
        MYE_JP("タグ", MYE_FIELD(PartComponent, tag, UInt64)),
        MYE_JP("ジョイント", MYE_FIELD(PartComponent, joint, String64)),
        MYE_JP("骨の供給元", MYE_FIELD(PartComponent, source, EntityRef)),
    });

    // M49: 部位の範囲 (箱/球ボリューム)。**hash 対象** — Parts::RaycastParts の結果を
    // スクリプトが読んで挙動を変える = sim 状態の入力になるため (Part と同じ判断)
    RegisterComponent<PartBoundsComponent>("PartBounds", {
        MYE_JP("形状", MYE_FIELD(PartBoundsComponent, shape, Int32)),
        MYE_JP("中心", MYE_FIELD(PartBoundsComponent, center, Float3)),
        MYE_JP("ハーフサイズ", MYE_FIELD_TIP(PartBoundsComponent, halfExtents, Float3,
                                             "sphere uses x as radius")),
    });

    // M52g: 入力レーンの結び付け。**hash 対象** — レーンごとのアクション評価結果を
    // ワールドハッシュに載せること自体が目的 (Components.h の理由 1)。
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

    // M58b: 地形。**kComponentNoHash** — 地形は描画専用レーン。ハイトフィールドを
    // sim に入れる地形コリジョンは Collider (shape=4) 側が持つ
    RegisterComponent<TerrainComponent>("Terrain", {
        MYE_JP("地形アセット", MYE_FIELD_TIP(TerrainComponent, source, String64,
                                             "assets-relative .terrain.json path")),
        // 範囲は TerrainSystem::ClampChunkTiles と同じ 2..256。Inspector 側にも入れておくと
        // 「打った数字が黙って丸められて表示と食い違う」事故が起きない
        MYE_JP("チャンクのタイル数",
               MYE_FIELD_RANGE(TerrainComponent, chunkTiles, Int32, 2.0f, 256.0f)),
        // M58e: LOD (フィールド表の末尾 append)。既定 0 = 無効
        MYE_JP("LOD 切替距離",
               MYE_FIELD_TIP(TerrainComponent, lodDistance, Float,
                             "camera-space depth for LOD 1 (0 = LOD off)")),
        MYE_JP("スカート深さ",
               MYE_FIELD_TIP(TerrainComponent, skirtDepth, Float,
                             "0 = auto (measured LOD edge gap), <0 = no skirt")),
    }, kComponentNoHash);

    // M56a/M56b: デカール (投影ボックス)。**kComponentNoHash** — GBuffer の albedo と
    // 法線 / roughness を上描きするだけの描画レーンで、sim には 1 バイトも触らない。
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
        // ---- M56b (末尾 append)。**強度 0 = 恒等** = GBuffer は 1 ビットも動かない ----
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
    // 駆動する sim 入力そのもの。
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
    // **装着 = 新数式への opt-in** (係数 0 でのビット中立は
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
    // 水面と水の密度は PhysicsEnvironment 側 (env 不在なら
    // 既定の水)。**v1 に復原モーメントは無い** (Components.h の制限コメント参照)
    RegisterComponent<BuoyancyComponent>("Buoyancy", {
        MYE_JP("排除体積倍率", MYE_FIELD_TIP(BuoyancyComponent, volumeScale, Float,
                                             "<= 0 disables buoyancy entirely")),
        MYE_JP("水中の抵抗", MYE_FIELD_RANGE(BuoyancyComponent, linearDrag, Float, 0.0f, 100.0f)),
        MYE_JP("水中の回転抵抗",
               MYE_FIELD_RANGE(BuoyancyComponent, angularDrag, Float, 0.0f, 100.0f)),
    });

    // M59d: 翼面。**hash 対象** — 親剛体の velocity / angularVelocity を駆動する。
    // 子エンティティに置いて質量中心からずらすのが本来の
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
    // LocalTransform を駆動する。type は「どの拘束行を立てるか」のプリセット
    // (Components.h の jointtype)
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
    // SkinnedMesh 側に付く札で、骨の実体は Part を持つ直子エンティティ
    RegisterComponent<RagdollComponent>("Ragdoll", {
        MYE_JP("物理駆動", MYE_FIELD_TIP(RagdollComponent, active, Bool,
                                         "on hands the bones to the rigid bodies; off lets the "
                                         "animation drive them and holds the bodies kinematic")),
    });

    // M60h1: 車輪 (レイキャストサスペンション)。**hash 対象** — 親剛体の velocity /
    // angularVelocity を駆動し、出力 2 本もソルバが毎 tick 書く sim 状態。
    // 車体の**子**に置いて質量中心からずらすのが本来の
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
               MYE_FIELD_FLAGS(WheelComponent, isGrounded, Bool, kFieldReadOnly)),
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
    // タイヤ力が velocity / angularVelocity を駆動する。
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
    // 住み、ここはオーサリングのみ。フィールドは **hash 対象**。池の組み直し条件は「粒子数 (segmentCount+1) の不一致」だけ —
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
                             "attaches the far end to this rigid body both ways (the rope "
                             "carries the body, the body loads the rope). anchored where the "
                             "rope end sits when first resolved; clearing it drops the body")),
    });

    // ---- M65a: 音響伝播 (TypeId 45〜49、末尾 append) ----
    // ★M60' の Cloth / SoftBody は未登録 (番号の見込みは RectTransform の登録コメント)。
    //   登録順 = TypeId なので飛ばし登録はできない — M60' 再開時はその時点の末尾へ append する。

    // 音のボクセル場を張る箱。**この 1 個の有無が音響システム全体の存在ゲート**。
    // hash 対象 — グリッドの形は波の到達セルを決める sim 入力そのもの
    RegisterComponent<AcousticVolumeComponent>("AcousticVolume", {
        MYE_JP("セル数 X", MYE_FIELD_RANGE(AcousticVolumeComponent, dimX, Int32, 1.0f, 256.0f)),
        MYE_JP("セル数 Y", MYE_FIELD_RANGE(AcousticVolumeComponent, dimY, Int32, 1.0f, 256.0f)),
        MYE_JP("セル数 Z", MYE_FIELD_RANGE(AcousticVolumeComponent, dimZ, Int32, 1.0f, 256.0f)),
        MYE_JP("セルサイズ",
               MYE_FIELD_TIP(AcousticVolumeComponent, cellSize, Float,
                             "metres per cell; the box extent is dim * cellSize, derived - "
                             "never authored directly")),
        MYE_JP("航法グリッド比",
               MYE_FIELD_RANGE(AcousticVolumeComponent, navCellRatio, Int32, 1.0f, 8.0f)),
        MYE_JP("遮蔽レイヤー",
               MYE_FIELD_TIP(AcousticVolumeComponent, blockLayerMask, UInt32,
                             "only colliders on these layers block sound")),
        MYE_JP("有効", MYE_FIELD(AcousticVolumeComponent, enabled, Bool)),
        MYE_JP("残光の減衰率",
               MYE_FIELD_TIP(AcousticVolumeComponent, glowKeepPerTick, Float,
                             "per-tick keep factor for the afterglow; 0 or out of (0,1) = "
                             "engine default 0.995. closer to 1 = longer afterglow, but the "
                             "uint8 storage caps the tail at ~4.25s regardless")),
        MYE_JP("残光の明るさ",
               MYE_FIELD_RANGE(AcousticVolumeComponent, glowIntensity, Float, 0.0f, 4.0f)),
        MYE_JP("残光に面の色",
               MYE_FIELD_RANGE(AcousticVolumeComponent, glowAlbedoMix, Float, 0.0f, 1.0f)),
        MYE_JP("残光を減らす間隔",
               MYE_FIELD_TIP(AcousticVolumeComponent, glowDecayEveryTicks, Int32,
                             "decay the afterglow only once every N ticks; 0 or 1 = every tick. "
                             "N stretches the afterglow (and its ~4.25s uint8 cap) by N. "
                             "clamped to 16")),
    });

    // 音を出す口。pending* を書くと次の音響フェーズで波が 1 本生まれる。
    // travelAccum / cooldown は **sim 状態** (エンジンが書く) なので hash 対象のまま置く
    RegisterComponent<AcousticEmitterComponent>("AcousticEmitter", {
        MYE_JP("発音の大きさ",
               MYE_FIELD_TIP(AcousticEmitterComponent, pendingLoudness, Float,
                             "write a positive value to fire one wave; the engine clears it "
                             "on the tick it is consumed")),
        MYE_JP("発音の到達距離", MYE_FIELD(AcousticEmitterComponent, pendingRadiusM, Float)),
        MYE_JP("音色", MYE_FIELD_RANGE(AcousticEmitterComponent, pendingTone, Int32, 0.0f, 3.0f)),
        MYE_JP("リング分周",
               MYE_FIELD_TIP(AcousticEmitterComponent, ticksPerRing, Int32,
                             "ticks per wavefront ring; speed = cellSize * 60 / this")),
        MYE_JP("足音を自動生成", MYE_FIELD(AcousticEmitterComponent, autoFootstep, Bool)),
        MYE_JP("歩幅", MYE_FIELD(AcousticEmitterComponent, stepDistanceM, Float)),
        MYE_JP("歩幅の累積", MYE_FIELD_FLAGS(AcousticEmitterComponent, travelAccum, Float,
                                             kFieldReadOnly)),
        MYE_JP("発音間隔", MYE_FIELD(AcousticEmitterComponent, cooldownTicks, Int32)),
        MYE_JP("発音待ち", MYE_FIELD_FLAGS(AcousticEmitterComponent, cooldown, Int32,
                                           kFieldReadOnly)),
        MYE_JP("足音の振幅係数",
               MYE_FIELD_TIP(AcousticEmitterComponent, footstepGain, Float,
                             "scales the auto-footstep amplitude and reach; 0 or less = 1.0. "
                             "lets run steps be louder and carry farther than crouch steps, "
                             "independent of the floor material")),
    });

    // 音を聞く耳。★ミラーに kFieldNoSerialize を付けないこと — 付けるとハッシュ対象から
    // 外れて「波がいつ・どこから届いたか」の被覆が丸ごと消える (PlayerInput と同じ罠)
    RegisterComponent<AcousticListenerComponent>("AcousticListener", {
        MYE_JP("聴取しきい値", MYE_FIELD(AcousticListenerComponent, threshold, Float)),
        MYE_JP("最終聴取 tick", MYE_FIELD_FLAGS(AcousticListenerComponent, lastHeardTick, UInt64,
                                                kFieldReadOnly)),
        MYE_JP("最終聴取位置", MYE_FIELD_FLAGS(AcousticListenerComponent, lastHeardPos, Float3,
                                               kFieldReadOnly)),
        MYE_JP("最終聴取の大きさ", MYE_FIELD_FLAGS(AcousticListenerComponent, lastLoudness, Float,
                                                   kFieldReadOnly)),
        MYE_JP("最終音源", MYE_FIELD_FLAGS(AcousticListenerComponent, lastSourceEntity, EntityRef,
                                           kFieldReadOnly)),
        MYE_JP("最終音色", MYE_FIELD_FLAGS(AcousticListenerComponent, lastTone, Int32,
                                           kFieldReadOnly)),
        MYE_JP("敵の音を聞く",
               MYE_FIELD_TIP(AcousticListenerComponent, hearAgents, Bool,
                             "off = waves emitted by an entity with AgentBrain are never delivered "
                             "(enemies stop reacting to each other's voices)")),
        MYE_JP("無視する音源",
               MYE_FIELD_TIP(AcousticListenerComponent, ignoreSource, EntityRef,
                             "waves emitted by this entity are never delivered "
                             "(a sound source this listener has got used to)")),
    });

    // 光に寄る目。光源は既存の LightComponent をそのまま読む (新しい光の概念を作らない)
    RegisterComponent<LightSeekerComponent>("LightSeeker", {
        MYE_JP("感知半径", MYE_FIELD(LightSeekerComponent, attractRadius, Float)),
        MYE_JP("最小強度",
               MYE_FIELD_TIP(LightSeekerComponent, minIntensity, Float,
                             "lights dimmer than this are invisible - this is what lets a "
                             "light being placed be seen before it finishes growing")),
        MYE_JP("最寄りの光", MYE_FIELD_FLAGS(LightSeekerComponent, nearestLight, EntityRef,
                                             kFieldReadOnly)),
        MYE_JP("最寄りの光の位置", MYE_FIELD_FLAGS(LightSeekerComponent, nearestPos, Float3,
                                                   kFieldReadOnly)),
        MYE_JP("最寄りの光の強さ", MYE_FIELD_FLAGS(LightSeekerComponent, nearestStrength, Float,
                                                   kFieldReadOnly)),
    });

    // 敵の共通思考。**性格づけはこのフィールド値だけ** — 遷移表は 5 状態固定でコードに持つ
    RegisterComponent<AgentBrainComponent>("AgentBrain", {
        MYE_JP("状態", MYE_FIELD_TIP(AgentBrainComponent, state, Int32,
                                     "0=patrol 1=alert 2=search 3=chase 4=return")),
        MYE_JP("状態経過 tick", MYE_FIELD_FLAGS(AgentBrainComponent, stateTicks, Int32,
                                                kFieldReadOnly)),
        MYE_JP("帰還先", MYE_FIELD(AgentBrainComponent, home, Float3)),
        MYE_JP("目標", MYE_FIELD_FLAGS(AgentBrainComponent, target, Float3, kFieldReadOnly)),
        MYE_JP("警戒時間", MYE_FIELD_TIP(AgentBrainComponent, alertTicks, Int32,
                                         "ticks spent frozen and silent after hearing "
                                         "something - this is the tell the player reads")),
        MYE_JP("探索時間", MYE_FIELD(AgentBrainComponent, searchTicks, Int32)),
        MYE_JP("追跡を諦める時間", MYE_FIELD(AgentBrainComponent, loseTicks, Int32)),
        MYE_JP("記憶時間", MYE_FIELD(AgentBrainComponent, memoryTicks, Int32)),
        MYE_JP("歩行速度", MYE_FIELD(AgentBrainComponent, walkSpeed, Float)),
        MYE_JP("走行速度", MYE_FIELD(AgentBrainComponent, runSpeed, Float)),
        MYE_JP("自発音の間隔", MYE_FIELD(AgentBrainComponent, emitEveryTicks, Int32)),
        MYE_JP("自発音の大きさ", MYE_FIELD(AgentBrainComponent, emitLoudness, Float)),
        MYE_JP("自発音の位相", MYE_FIELD_FLAGS(AgentBrainComponent, emitPhase, Int32,
                                               kFieldReadOnly)),
    });

    // ---- M68a: 音響 × オーディオの調整卓 (TypeId 50、末尾 append) ----
    // ★**kComponentNoHash** — 出力レーンしか触らないので、WorldHasher が丸ごとスキップする
    //   (AudioListener / AudioSource と同じ根拠)。
    // ★実行中に Inspector で全部触れることが設計の一部。調整値は耳でしか決まらないので、
    //   「触っても replay が割れない」ことがそのまま作業速度になる。
    RegisterComponent<AcousticAudioComponent>("AcousticAudio", {
        MYE_JP("有効", MYE_FIELD(AcousticAudioComponent, enabled, Bool)),
        MYE_JP("リスナー場の半径",
               MYE_FIELD_RANGE(AcousticAudioComponent, probeMaxRing, Int32, 1.0f, 256.0f)),
        MYE_JP("回折が飽和する長さ",
               MYE_FIELD_RANGE(AcousticAudioComponent, bendFullM, Float, 0.5f, 64.0f)),
        MYE_JP("回折 LPF の下限",
               MYE_FIELD_RANGE(AcousticAudioComponent, lpfFloor, Float, 0.0f, 1.0f)),
        MYE_JP("密閉時の音量",
               MYE_FIELD_RANGE(AcousticAudioComponent, occludedGain, Float, 0.0f, 1.0f)),
        MYE_JP("密閉時の LPF",
               MYE_FIELD_RANGE(AcousticAudioComponent, occludedLpf, Float, 0.0f, 1.0f)),
        MYE_JP("整形の半減期",
               MYE_FIELD_RANGE(AcousticAudioComponent, smoothTicks, Int32, 0.0f, 120.0f)),
        MYE_JP("開放度の半径",
               MYE_FIELD_RANGE(AcousticAudioComponent, roomProbeM, Float, 1.0f, 32.0f)),
        MYE_JP("開放度の下端",
               MYE_FIELD_RANGE(AcousticAudioComponent, openSmall, Float, 0.0f, 1.0f)),
        MYE_JP("開放度の上端",
               MYE_FIELD_RANGE(AcousticAudioComponent, openLarge, Float, 0.0f, 1.0f)),
        MYE_JP("残響の半減期",
               MYE_FIELD_RANGE(AcousticAudioComponent, roomSmoothTicks, Int32, 0.0f, 300.0f)),
        MYE_JP("狭い側のプリセット",
               MYE_FIELD_RANGE(AcousticAudioComponent, reverbSmall, Int32, 0.0f, 10.0f)),
        MYE_JP("広い側のプリセット",
               MYE_FIELD_RANGE(AcousticAudioComponent, reverbLarge, Int32, 0.0f, 10.0f)),
        MYE_JP("回り込みの残響送り",
               MYE_FIELD_RANGE(AcousticAudioComponent, detourWet, Float, 0.0f, 1.0f)),
        MYE_JP("波の音量係数",
               MYE_FIELD_RANGE(AcousticAudioComponent, waveVolume, Float, 0.0f, 4.0f)),
        MYE_JP("波の最小音量",
               MYE_FIELD_RANGE(AcousticAudioComponent, minWaveVolume, Float, 0.0f, 1.0f)),
        MYE_JP("波の音量カーブ",
               MYE_FIELD_RANGE(AcousticAudioComponent, waveVolumeExp, Float, 0.25f, 4.0f)),
        MYE_JP("波の残響送り",
               MYE_FIELD_RANGE(AcousticAudioComponent, waveReverbSend, Float, 0.0f, 1.0f)),
        MYE_JP("波の減衰カーブ",
               MYE_FIELD_RANGE(AcousticAudioComponent, waveRolloff, Int32, 0.0f, 2.0f)),
        MYE_JP("音色 0 のサウンド", MYE_FIELD(AcousticAudioComponent, toneSound0, String64)),
        MYE_JP("音色 1 のサウンド", MYE_FIELD(AcousticAudioComponent, toneSound1, String64)),
        MYE_JP("音色 2 のサウンド", MYE_FIELD(AcousticAudioComponent, toneSound2, String64)),
        MYE_JP("音色 3 のサウンド", MYE_FIELD(AcousticAudioComponent, toneSound3, String64)),
    }, kComponentNoHash);

    // ImpactSynth: 発音元ごとの波の音 (=51)。音レーン専用なので NoHash。
    // 付けなければ床材 → tone マップに従う = 既存シーンの音は 1 音も変わらない
    RegisterComponent<WaveSoundComponent>("WaveSound", {
        MYE_JP("波の音",
               MYE_FIELD_TIP(WaveSoundComponent, sound, String64,
                             "sound key (.sound.json / .impact.json name) played when this entity's "
                             "wave is born; empty = silent. Without this component the floor "
                             "material's acousticSound, then AcousticAudio's tone map, decide")),
    }, kComponentNoHash);

    // M75a: RectTransform (=52)。UI 要素の配置 (UIElement から分離)。描画専用の NoHash +
    // UI 専用判定に載せる UiAux。旧シーンの UIElement.anchor/x/y/w/h/space はロード時に
    // ここへ変換される (SceneSerializer)。**M60′ の Cloth/SoftBody は 63/64 の見込み** (62 = Tag)
    // (M75 の UI コンポーネント群 52〜61 が先に埋める)
    RegisterComponent<RectTransformComponent>("RectTransform", {
        MYE_JP("アンカー (min)", MYE_FIELD_TIP(RectTransformComponent, anchorMin, Float2,
                                              "parent-relative 0..1, (0,0) = top-left")),
        MYE_JP("アンカー (max)", MYE_FIELD_TIP(RectTransformComponent, anchorMax, Float2,
                                              "equal to anchorMin = fixed size, apart = stretch")),
        MYE_JP("ピボット", MYE_FIELD_TIP(RectTransformComponent, pivot, Float2,
                                         "own-rect 0..1, centre of rotation/scale")),
        MYE_JP("位置", MYE_FIELD_TIP(RectTransformComponent, anchoredPosition, Float2,
                                     "offset from the anchor point to the pivot")),
        MYE_JP("サイズ", MYE_FIELD_TIP(RectTransformComponent, sizeDelta, Float2,
                                       "size delta against the anchor rect (= size when anchors match)")),
        MYE_JP("回転 (度)", MYE_FIELD_TIP(RectTransformComponent, rotation, Float,
                                          "Z rotation in degrees, positive = clockwise")),
        MYE_JP("スケール", MYE_FIELD(RectTransformComponent, scale, Float2)),
        MYE_JP("基準", MYE_FIELD_TIP(RectTransformComponent, basis, Int32,
                                     "0 = nearest UI ancestor (canvas if none) 1 = canvas")),
    }, kComponentNoHash | kComponentUiAux);

    // M75c: UICanvas (RectTransform の直後)。Canvas + Canvas Scaler。描画専用の NoHash +
    // UI 専用判定に載せる UiAux (忘れると Canvas を持つ要素がワールド追従に落ちて消える)
    RegisterComponent<UICanvasComponent>("UICanvas", {
        MYE_JP("基準幅", MYE_FIELD_TIP(UICanvasComponent, referenceW, Int32,
                                       "reference width; <= 0 = project_settings ui.referenceW")),
        MYE_JP("基準高さ", MYE_FIELD_TIP(UICanvasComponent, referenceH, Int32,
                                         "reference height; <= 0 = project_settings ui.referenceH")),
        MYE_JP("スケールモード", MYE_FIELD_TIP(UICanvasComponent, scaleMode, Int32,
                                               "0 = Expand 1 = Shrink 2 = Match width or height")),
        MYE_JP("幅/高さの比重", MYE_FIELD_RANGE(UICanvasComponent, match, Float, 0.0f, 1.0f)),
        MYE_JP("描画順", MYE_FIELD_TIP(UICanvasComponent, sortOrder, Int32,
                                       "first draw/hit key; larger = in front (default canvas = 0)")),
    }, kComponentNoHash | kComponentUiAux);

    // M75e: 自動レイアウト 3 種 (UICanvas の直後、この順)。どれも描画専用の NoHash + UiAux。
    // 子の RectTransform へは書き込まず、uilayout::Resolve が解くときに読む (UILayoutGroup.h)
    RegisterComponent<UILayoutGroupComponent>("UILayoutGroup", {
        MYE_JP("種類", MYE_FIELD_TIP(UILayoutGroupComponent, kind, Int32,
                                     "0 = horizontal 1 = vertical 2 = grid")),
        MYE_JP("余白", MYE_FIELD_TIP(UILayoutGroupComponent, padding, Float4,
                                     "inner padding (left, top, right, bottom)")),
        MYE_JP("間隔", MYE_FIELD_TIP(UILayoutGroupComponent, spacing, Float2,
                                     "gap between children; horizontal uses x, vertical y, grid both")),
        MYE_JP("子の整列", MYE_FIELD_TIP(UILayoutGroupComponent, childAlignment, Int32,
                                         "9-grid 0..8 (0 = upper left)")),
        MYE_JP("子の幅を制御", MYE_FIELD_TIP(UILayoutGroupComponent, controlChildWidth, Bool,
                                             "horizontal/vertical: the group sets child widths")),
        MYE_JP("子の高さを制御", MYE_FIELD_TIP(UILayoutGroupComponent, controlChildHeight, Bool,
                                               "horizontal/vertical: the group sets child heights")),
        MYE_JP("幅を広げる", MYE_FIELD_TIP(UILayoutGroupComponent, forceExpandWidth, Bool,
                                           "horizontal/vertical: hand spare width to every child")),
        MYE_JP("高さを広げる", MYE_FIELD_TIP(UILayoutGroupComponent, forceExpandHeight, Bool,
                                             "horizontal/vertical: hand spare height to every child")),
        MYE_JP("逆順に並べる", MYE_FIELD_TIP(UILayoutGroupComponent, reverseArrangement, Bool,
                                             "horizontal/vertical: last sibling first")),
        MYE_JP("セルの大きさ", MYE_FIELD_TIP(UILayoutGroupComponent, cellSize, Float2,
                                             "grid: every child's size")),
        MYE_JP("開始の角", MYE_FIELD_TIP(UILayoutGroupComponent, startCorner, Int32,
                                         "grid: 0 = upper left 1 = upper right 2 = lower left 3 = lower right")),
        MYE_JP("埋める向き", MYE_FIELD_TIP(UILayoutGroupComponent, startAxis, Int32,
                                           "grid: 0 = fill rows first 1 = fill columns first")),
        MYE_JP("制約", MYE_FIELD_TIP(UILayoutGroupComponent, constraint, Int32,
                                     "grid: 0 = flexible 1 = fixed column count 2 = fixed row count")),
        MYE_JP("列数/行数", MYE_FIELD_TIP(UILayoutGroupComponent, constraintCount, Int32,
                                          "grid: the fixed column/row count (minimum 1)")),
    }, kComponentNoHash | kComponentUiAux);

    RegisterComponent<UILayoutElementComponent>("UILayoutElement", {
        MYE_JP("レイアウトを無視", MYE_FIELD_TIP(UILayoutElementComponent, ignoreLayout, Bool,
                                                 "not arranged by the parent layout group")),
        MYE_JP("最小の幅", MYE_FIELD_TIP(UILayoutElementComponent, minWidth, Float, "negative = unset")),
        MYE_JP("最小の高さ", MYE_FIELD_TIP(UILayoutElementComponent, minHeight, Float, "negative = unset")),
        MYE_JP("推奨の幅", MYE_FIELD_TIP(UILayoutElementComponent, preferredWidth, Float, "negative = unset")),
        MYE_JP("推奨の高さ", MYE_FIELD_TIP(UILayoutElementComponent, preferredHeight, Float, "negative = unset")),
        MYE_JP("伸縮の幅", MYE_FIELD_TIP(UILayoutElementComponent, flexibleWidth, Float,
                                         "share of the spare width; negative = unset")),
        MYE_JP("伸縮の高さ", MYE_FIELD_TIP(UILayoutElementComponent, flexibleHeight, Float,
                                           "share of the spare height; negative = unset")),
        MYE_JP("優先度", MYE_FIELD_TIP(UILayoutElementComponent, layoutPriority, Int32,
                                       "higher wins over text/group sizes (priority 0)")),
    }, kComponentNoHash | kComponentUiAux);

    RegisterComponent<UIContentSizeFitterComponent>("UIContentSizeFitter", {
        MYE_JP("横の合わせ方", MYE_FIELD_TIP(UIContentSizeFitterComponent, horizontalFit, Int32,
                                             "0 = unconstrained 1 = min size 2 = preferred size")),
        MYE_JP("縦の合わせ方", MYE_FIELD_TIP(UIContentSizeFitterComponent, verticalFit, Int32,
                                             "0 = unconstrained 1 = min size 2 = preferred size")),
    }, kComponentNoHash | kComponentUiAux);

    // M75f: ウィジェット 4 種 (UIContentSizeFitter の直後、この順)。
    // Selectable と ToggleGroup は描画 / ヒット / ナビの入力 = NoHash + UiAux。
    // Toggle と Slider は **sim が値を書く状態なのでハッシュ対象** (UiAux だけ) — ここを NoHash にすると
    // ウィジェットの配線が壊れても replay_verify が緑のままになる
    RegisterComponent<UISelectableComponent>("UISelectable", {
        MYE_JP("操作可能", MYE_FIELD_TIP(UISelectableComponent, interactable, Bool,
                                         "0 = swallows the pointer but never presses, clicks or takes focus")),
        MYE_JP("遷移", MYE_FIELD_TIP(UISelectableComponent, transition, Int32,
                                     "0 = none 1 = color tint 2 = sprite swap")),
        MYE_JP("通常の色", MYE_FIELD(UISelectableComponent, normalColor, Color)),
        MYE_JP("ハイライトの色", MYE_FIELD(UISelectableComponent, highlightedColor, Color)),
        MYE_JP("押下の色", MYE_FIELD(UISelectableComponent, pressedColor, Color)),
        MYE_JP("選択中の色", MYE_FIELD(UISelectableComponent, selectedColor, Color)),
        MYE_JP("無効の色", MYE_FIELD(UISelectableComponent, disabledColor, Color)),
        MYE_JP("色の倍率", MYE_FIELD_RANGE(UISelectableComponent, colorMultiplier, Float, 1.0f, 5.0f)),
        MYE_JP("ハイライトの画像", MYE_FIELD(UISelectableComponent, highlightedSprite, AssetRef)),
        MYE_JP("押下の画像", MYE_FIELD(UISelectableComponent, pressedSprite, AssetRef)),
        MYE_JP("選択中の画像", MYE_FIELD(UISelectableComponent, selectedSprite, AssetRef)),
        MYE_JP("無効の画像", MYE_FIELD(UISelectableComponent, disabledSprite, AssetRef)),
        MYE_JP("ナビゲーション", MYE_FIELD_TIP(UISelectableComponent, navigationMode, Int32,
                                               "0 = none 1 = horizontal 2 = vertical 3 = automatic 4 = explicit")),
        MYE_JP("上の選択先", MYE_FIELD_TIP(UISelectableComponent, selectOnUp, EntityRef, "explicit navigation only")),
        MYE_JP("下の選択先", MYE_FIELD_TIP(UISelectableComponent, selectOnDown, EntityRef, "explicit navigation only")),
        MYE_JP("左の選択先", MYE_FIELD_TIP(UISelectableComponent, selectOnLeft, EntityRef, "explicit navigation only")),
        MYE_JP("右の選択先", MYE_FIELD_TIP(UISelectableComponent, selectOnRight, EntityRef, "explicit navigation only")),
        MYE_JP("対象のグラフィック", MYE_FIELD_TIP(UISelectableComponent, targetGraphic, EntityRef,
                                                   "UIElement to tint / swap; empty = this entity's own")),
    }, kComponentNoHash | kComponentUiAux);

    RegisterComponent<UIToggleComponent>("UIToggle", {
        MYE_JP("オン", MYE_FIELD(UIToggleComponent, isOn, Bool)),
        MYE_JP("グラフィック", MYE_FIELD_TIP(UIToggleComponent, graphic, EntityRef,
                                             "drawn only while on (the check mark)")),
        MYE_JP("グループ", MYE_FIELD_TIP(UIToggleComponent, group, EntityRef,
                                         "entity with a UIToggleGroup; empty = standalone")),
    }, kComponentUiAux);

    RegisterComponent<UISliderComponent>("UISlider", {
        MYE_JP("塗りの矩形", MYE_FIELD_TIP(UISliderComponent, fillRect, EntityRef,
                                           "anchors are driven from the value inside its parent")),
        MYE_JP("つまみの矩形", MYE_FIELD_TIP(UISliderComponent, handleRect, EntityRef,
                                             "anchors are driven from the value inside its parent")),
        MYE_JP("向き", MYE_FIELD_TIP(UISliderComponent, direction, Int32,
                                     "0 = left to right 1 = right to left 2 = bottom to top 3 = top to bottom")),
        MYE_JP("最小値", MYE_FIELD(UISliderComponent, minValue, Float)),
        MYE_JP("最大値", MYE_FIELD(UISliderComponent, maxValue, Float)),
        MYE_JP("整数のみ", MYE_FIELD(UISliderComponent, wholeNumbers, Bool)),
        MYE_JP("値", MYE_FIELD(UISliderComponent, value, Float)),
        MYE_JP("掴んだ位置", MYE_FIELD_FLAGS(UISliderComponent, dragOffset, Float2, kFieldHidden)),
    }, kComponentUiAux);

    RegisterComponent<UIToggleGroupComponent>("UIToggleGroup", {
        MYE_JP("すべてオフを許可", MYE_FIELD_TIP(UIToggleGroupComponent, allowSwitchOff, Bool,
                                                 "clicking the only toggle that is on may turn it off")),
    }, kComponentNoHash | kComponentUiAux);

    // M76f: Deep-Modal 衝突音 (TypeId=61、**末尾 append**)。ModalSound を持つ物だけ接触音が
    // モーダル合成に差し替わる (無ければ従来の WaveSound / 床材 / tone のまま)。
    // kComponentNoHash — sim 状態はゼロ (WaveSoundComponent と同じ音レーン)
    RegisterComponent<ModalSoundComponent>("ModalSound", {
        MYE_JP("メッシュ", MYE_FIELD_TIP(ModalSoundComponent, mesh, AssetRef,
                                         "empty = this entity's MeshRenderer.mesh")),
        MYE_JP("音量", MYE_FIELD(ModalSoundComponent, gain, Float)),
        MYE_JP("マスク閾値", MYE_FIELD_TIP(ModalSoundComponent, maskThreshold, Float,
                                           "<= 0 uses the .dmnet header default")),
        MYE_JP("クールダウン (tick)", MYE_FIELD(ModalSoundComponent, cooldownTicks, Int32)),
        MYE_JP("サイズ倍率", MYE_FIELD(ModalSoundComponent, sizeScale, Float)),
        MYE_JP("最大距離", MYE_FIELD(ModalSoundComponent, maxDistance, Float)),
        MYE_JP("波の耳出しを消す", MYE_FIELD_TIP(ModalSoundComponent, muteWave, Bool,
                                                 "non-zero: mute the WaveSound playback once baked")),
    }, kComponentNoHash);

    // 汎用タグ (TypeId=62、**末尾 append**)。M60′ の Cloth/SoftBody の見込みはこれで 63/64 へ下がる。
    // hash 対象 — スクリプトが HasTag / FindEntitiesWithTag で分岐できる sim 入力だから。
    // opt-in の型なので既存シーンのハッシュは変わらない (この関数の頭の規約)。
    // Inspector はビット集合をタグ名のチェックリストで出す (InspectorWindow の "Tag"/"mask" 特例)
    RegisterComponent<TagComponent>("Tag", {
        MYE_JP("タグ", MYE_FIELD_TIP(TagComponent, mask, UInt64,
                                     "set of tag numbers (names live in Project Settings > Tags)")),
    });

    // 水面波 (TypeId=63、末尾 append)。
    // 三角関数 (Gerstner 波) によるリアルな水面シミュレーションと浮力連動。
    RegisterComponent<WaterWaveComponent>("WaterWave", {
        MYE_JP("有効", MYE_FIELD(WaterWaveComponent, enabled, Bool)),
        MYE_JP("基準高さ", MYE_FIELD(WaterWaveComponent, baseHeight, Float)),
        MYE_JP("波高倍率", MYE_FIELD_RANGE(WaterWaveComponent, overallScale, Float, 0.0f, 10.0f)),
        MYE_JP("時間倍率", MYE_FIELD_RANGE(WaterWaveComponent, timeScale, Float, 0.0f, 10.0f)),
        MYE_JP("有効波本数", MYE_FIELD_RANGE(WaterWaveComponent, waveCount, Int32, 1.0f, 4.0f)),
        MYE_JP("浮力連動", MYE_FIELD(WaterWaveComponent, affectBuoyancy, Bool)),

        MYE_JP("波0 振幅", MYE_FIELD_RANGE(WaterWaveComponent, wave0Amplitude, Float, 0.0f, 10.0f)),
        MYE_JP("波0 波長", MYE_FIELD_RANGE(WaterWaveComponent, wave0Wavelength, Float, 0.1f, 200.0f)),
        MYE_JP("波0 速度", MYE_FIELD(WaterWaveComponent, wave0Speed, Float)),
        MYE_JP("波0 方向角 (度)", MYE_FIELD_RANGE(WaterWaveComponent, wave0DirAngle, Float, -180.0f, 180.0f)),
        MYE_JP("波0 急峻度", MYE_FIELD_RANGE(WaterWaveComponent, wave0Steepness, Float, 0.0f, 1.0f)),

        MYE_JP("波1 振幅", MYE_FIELD_RANGE(WaterWaveComponent, wave1Amplitude, Float, 0.0f, 10.0f)),
        MYE_JP("波1 波長", MYE_FIELD_RANGE(WaterWaveComponent, wave1Wavelength, Float, 0.1f, 200.0f)),
        MYE_JP("波1 速度", MYE_FIELD(WaterWaveComponent, wave1Speed, Float)),
        MYE_JP("波1 方向角 (度)", MYE_FIELD_RANGE(WaterWaveComponent, wave1DirAngle, Float, -180.0f, 180.0f)),
        MYE_JP("波1 急峻度", MYE_FIELD_RANGE(WaterWaveComponent, wave1Steepness, Float, 0.0f, 1.0f)),

        MYE_JP("波2 振幅", MYE_FIELD_RANGE(WaterWaveComponent, wave2Amplitude, Float, 0.0f, 10.0f)),
        MYE_JP("波2 波長", MYE_FIELD_RANGE(WaterWaveComponent, wave2Wavelength, Float, 0.1f, 200.0f)),
        MYE_JP("波2 速度", MYE_FIELD(WaterWaveComponent, wave2Speed, Float)),
        MYE_JP("波2 方向角 (度)", MYE_FIELD_RANGE(WaterWaveComponent, wave2DirAngle, Float, -180.0f, 180.0f)),
        MYE_JP("波2 急峻度", MYE_FIELD_RANGE(WaterWaveComponent, wave2Steepness, Float, 0.0f, 1.0f)),

        MYE_JP("波3 振幅", MYE_FIELD_RANGE(WaterWaveComponent, wave3Amplitude, Float, 0.0f, 10.0f)),
        MYE_JP("波3 波長", MYE_FIELD_RANGE(WaterWaveComponent, wave3Wavelength, Float, 0.1f, 200.0f)),
        MYE_JP("波3 速度", MYE_FIELD(WaterWaveComponent, wave3Speed, Float)),
        MYE_JP("波3 方向角 (度)", MYE_FIELD_RANGE(WaterWaveComponent, wave3DirAngle, Float, -180.0f, 180.0f)),
        MYE_JP("波3 急峻度", MYE_FIELD_RANGE(WaterWaveComponent, wave3Steepness, Float, 0.0f, 1.0f)),

        MYE_JP("深水色", MYE_FIELD(WaterWaveComponent, deepColor, Color)),
        MYE_JP("浅水色", MYE_FIELD(WaterWaveComponent, shallowColor, Color)),
        MYE_JP("白波強度", MYE_FIELD_RANGE(WaterWaveComponent, foamStrength, Float, 0.0f, 2.0f)),
        MYE_JP("フレネル指数", MYE_FIELD_RANGE(WaterWaveComponent, fresnelPower, Float, 1.0f, 10.0f)),
        MYE_JP("滑らかさ", MYE_FIELD_RANGE(WaterWaveComponent, smoothness, Float, 0.0f, 1.0f)),
        // M79 sub-05: 描画専用の差し替え口。kFieldNoHash = WorldHash / リプレイに入らない
        // (シーンへの保存/復元は従来どおり行う)。設定すると水面をサーフェスシェーダで描く
        MYE_JP("描画マテリアル (サーフェス)",
               MYE_FIELD_FLAGS(WaterWaveComponent, surfaceMaterial, AssetRef, kFieldNoHash)),
        // 波の時計 (シミュレーション tick 数)。浮力も水面の描画もこれを秒へ直して使う。
        // WorldHash / スナップショットに入るので、2 回目の Play・巻き戻し・分岐実行で位相が揃う
        MYE_JP("波の時刻 (tick)", MYE_FIELD_FLAGS(WaterWaveComponent, timeTicks, Int32, kFieldReadOnly)),
    });
}

} // namespace mye
