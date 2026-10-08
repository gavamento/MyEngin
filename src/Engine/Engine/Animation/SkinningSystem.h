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
// ポーズプログラム (M89a): poseClaim が立っている SkinnedMesh は、その tick にコントローラが
// プログラムを書いたので旧経路の時計を進めず、印だけ 0 に戻す。立っていなければ
// poseLayerCount を 0 に戻して旧経路を進める (= 書き手が居なくなった次の tick に旧経路へ戻る)。
// コントローラはこのシステムより前に走ること。
class SkinningSystem {
public:
    void Update(World& world, const RenderResources& resources);
};

// クロスフェード中か。中でなければポーズは clip 単独 (= M18 と同じ評価)
bool IsSkinFading(const SkinnedMeshComponent& sm);

// ポーズを SampleSkinnedLocals で組む必要があるか (M89a)。false なら clip 単独 = M18 と同じ評価で、
// 描画はパレットを直接作る経路 (ComputeBonePalette) を通してよい
bool UsesLocalsPath(const SkinnedMeshComponent& sm);

// a と b が同じモデルなら同じポーズになるか (SampleSkinnedLocals の入力が等しいか) (M89a)。
// 部位追従のポーズキャッシュのキー。★ポーズの入力を足したら、ここと SampleSkinnedLocals を一緒に直す。
// 終わったフェードの残骸 (fromClip 等) や、使っていない層の中身の違いでは割れない
bool SamePoseInputs(const SkinnedMeshComponent& a, const SkinnedMeshComponent& b);

// sm の今のポーズの局所行列 (joints.size() 個)。
// ★描画 / ラグドール / 部位追従の 3 者が必ずこれを通す — どれか 1 つだけフェードを知らないと、
//   骨に付けた部位やラグドールの未駆動の骨だけが、切り替えの瞬間に飛ぶ。
// poseLayerCount > 0 ならポーズプログラムの層を ComputeJointLocalsLayered で畳む (M89a)。
// 旧経路でフェードしていないときは ComputeJointLocals(model, clip, timeTicks / 60) そのもの (ビット一致)
void SampleSkinnedLocals(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                         std::vector<DirectX::XMMATRIX>& outLocals);

} // namespace mye
