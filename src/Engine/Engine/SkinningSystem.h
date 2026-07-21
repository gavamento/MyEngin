#pragma once

namespace mye {

class World;
struct RenderResources;

// スケルタルアニメの時刻を進めるシステム (M18)。tick フェーズで呼ぶ。
// SkinnedMeshComponent.timeTicks を 1 tick 進め、クリップ長でループさせる。
// ポーズ (ボーン行列) 自体は RenderSystem がフレーム毎に評価する (描画専用)。
// SkinnedMeshComponent は kComponentNoHash なので、この更新はワールドハッシュに影響しない。
class SkinningSystem {
public:
    void Update(World& world, const RenderResources& resources);
};

} // namespace mye
