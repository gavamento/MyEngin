//====================================================================================
//                          SpriteShapeCollider.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          SpriteShape2D の輪郭を静的な辺の箱として物理へ供給する
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"
#include "Engine/Engine/Physics/Rigid/StaticShapeSource.h"

namespace mye {

// collider が立った SpriteShape2D の輪郭 (EvaluateSpline の折れ線) を、線分ごとの薄い箱として
// PhysicsSystem の静的収集へ渡す (StaticShapeSource)。箱の姿勢はエンティティのワールド姿勢とスケールで毎 tick 作る。
// 評価した折れ線は点列の内容ハッシュでキャッシュする (点が変われば次の Collect で作り直す)。
// 点が変わっても眠っている剛体は起こさない (通常の静的 Collider を動かしたときと同じ)。
// 持ち主は EngineLoop / HeadlessSim。sim 状態を持たない (キャッシュは導出値) ので snapshot / hash の配線は要らない
class SpriteShapeColliders final : public StaticShapeSource {
public:
    SpriteShapeColliders() = default;
    SpriteShapeColliders(const SpriteShapeColliders&) = delete;
    SpriteShapeColliders& operator=(const SpriteShapeColliders&) = delete;

    // 持ち主 index 昇順 → 輪郭の線分の並び。順序は入力だけで決まる
    void Collect(World& world, std::vector<StaticShape>& out) override;
    // 動かす物体の範囲 (regions) に届かない輪郭は返さない。結果は絞らない Collect と同じ接触になる
    void CollectWithin(World& world, MotionRegions& regions, std::vector<StaticShape>& out) override;

    // 持ち主 1 体ぶんの全箱 (絞り込まない) を out の末尾へ足す。collider が無い / 点が 2 未満なら何も足さない
    // (Scene View の緑のギズモ用。Collect と同じキャッシュを使う)
    void AppendEntityShapes(World& world, EntityID e, std::vector<StaticShape>& out);

    // 直近の Collect / CollectWithin が返した箱の数 (計測用)
    size_t LastCollectBoxCount() const { return lastBoxCount_; }

private:
    struct EntityKey {
        uint32_t index = 0;
        uint32_t generation = 0;
        auto operator<=>(const EntityKey&) const = default;
    };
    // 持ち主ごとのキャッシュ。ColliderComponent は PhysicsSystem が tick の間指すので、map の要素として動かさない
    struct EntityCache {
        uint64_t contentHash = 0;
        bool valid = false;
        bool closed = false; // 折れ線の末尾から先頭へ閉じるか (IsClosedOutline の結果)
        std::vector<DirectX::XMFLOAT2> polyline;
        ColliderComponent col;
    };

    void CollectImpl(World& world, MotionRegions* regions, std::vector<StaticShape>& out);
    // 1 体ぶんの箱を作って out へ。regions が非 null で、どの範囲にも届かなければ何も足さない
    void CollectEntity(World& world, EntityID e, const SpriteShape2DComponent& shape, MotionRegions* regions,
                       std::vector<StaticShape>& out);

    std::map<EntityKey, EntityCache> cache_;
    size_t lastBoxCount_ = 0;
};

} // namespace mye
