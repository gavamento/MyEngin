#pragma once

#include <vector>

#include <DirectXMath.h>

namespace mye {

class World;
struct RenderResources;
struct SkinnedModel;
struct SkeletalClip;
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

// 骨クリップの長さ (tick、60Hz)。秒を一度だけ四捨五入する。長さ 0 以下は 0 (M89b で切り出し)。
// ★旧経路のループ・コントローラのステート長・BT の waitForEnd が同じ値を使う — 1 tick でもずれると
//   ループの折り返しと終端の判定が食い違う
int32_t SkeletalClipTicks(const SkeletalClip& clip);

// クロスフェード中か。中でなければポーズは clip 単独 (= M18 と同じ評価)
bool IsSkinFading(const SkinnedMeshComponent& sm);

// ポーズを SampleSkinnedLocals で組む必要があるか (M89a。IK の鎖があるときも M89l)。false なら clip 単独 = M18 と同じ評価で、
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
// poseRootJoint >= 0 なら、そのジョイントの水平の移動を抜く (M89j のルートモーション。描画補間の時刻でも同じ式)。
// poseIkCount > 0 なら、どの経路の結果にも最後に 2 ボーン IK の鎖を書かれた順に解く (M89l、SolveTwoBoneIk)。
// 旧経路でフェードしていないときは ComputeJointLocals(model, clip, timeTicks / 60) そのもの (ビット一致)
void SampleSkinnedLocals(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                         std::vector<DirectX::XMMATRIX>& outLocals);

// 描画用 (M89f): ポーズプログラムの各層の時刻を前 tick と今の tick の間で interpAlpha 補間して引く。
// alpha >= 1 は SampleSkinnedLocals そのもの (決定的撮影・編集中は alpha = 1 なので絵が変わらない)。
// 旧経路 (poseLayerCount = 0) と重みは補間しない。★部位追従・ラグドールは使わない (sim の tick 境界の姿勢に付く)
void SampleSkinnedLocalsInterpolated(const SkinnedModel& model, const SkinnedMeshComponent& sm,
                                     float interpAlpha, std::vector<DirectX::XMMATRIX>& outLocals);

} // namespace mye
