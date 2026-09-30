#pragma once

#include <vector>

#include <DirectXMath.h>

namespace mye {

class World;
struct RenderResources;
struct SkinnedModel;
struct SkinnedMeshComponent;

// スケルタルアニメの時刻を進めるシステム (M18)。tick フェーズで呼ぶ。
// SkinnedMeshComponent.timeTicks を 1 tick 進め、クリップ長でループさせる (loop = 0 なら末尾で止める)。
// clip の書き換えを検出して頭から再生し直し、fadeTicks > 0 ならクロスフェードを始める (M18 追補)。
// ポーズ (ボーン行列) 自体は RenderSystem がフレーム毎に評価する (描画専用)。
// SkinnedMeshComponent は kComponentNoHash なので、この更新はワールドハッシュに影響しない。
class SkinningSystem {
public:
    void Update(World& world, const RenderResources& resources);
};

// クロスフェード中か。中でなければポーズは clip 単独 (= M18 と同じ評価)
bool IsSkinFading(const SkinnedMeshComponent& sm);

// sm の今のポーズの局所行列 (joints.size() 個)。
// ★描画 / ラグドール / 部位追従の 3 者が必ずこれを通す — どれか 1 つだけフェードを知らないと、
//   骨に付けた部位やラグドールの未駆動の骨だけが、切り替えの瞬間に飛ぶ。
// フェードしていないときは ComputeJointLocals(model, clip, timeTicks / 60) そのもの (ビット一致)
void SampleSkinnedLocals(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                         std::vector<DirectX::XMMATRIX>& outLocals);

} // namespace mye
