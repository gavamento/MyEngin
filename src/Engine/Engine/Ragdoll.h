#pragma once
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct SkinnedModel;

// ラグドール (M60g1)。**骨の駆動方向を反転させる**だけの層で、新しい骨の表現は持ち込まない。
//
//   RagdollComponent.active == false … アニメ → 骨 → 部位 (PartFollowSystem が LocalTransform
//                                       を書き、物理は部位を kinematic として扱う)
//   RagdollComponent.active == true  … 物理 → 部位 → 骨 (PartFollowSystem は skip し、
//                                       描画側が部位の LocalTransform からパレットを組む)
//
// ★判定をここ 1 本に閉じ込めているのは `Parts::ResolvePartSource` と同じ理由 —
//   物理 (収集の kinematic 分岐) と PartFollowSystem (skip 判定) と描画 (パレット) の
//   3 者が**同じ答え**を見ないと、「片方だけが駆動を諦めて部位が固まる」が静かに起きる。
namespace ragdoll {

// シーンに RagdollComponent が 1 つでもあるか。**存在ゲート**専用 —
// false なら呼び側は以降の探索をまるごと飛ばしてよい (既存シーンはここで抜ける)
bool AnyRagdoll(World& world);

// source (SkinnedMesh 側) のラグドールが作動中か。Ragdoll を持たなければ false
bool IsSourceDriven(World& world, EntityID source);

// part が「休止中のラグドールに属する骨追従部位」か = **物理が動かしてはいけない**。
// Part を持たない / joint が空 / 供給元にラグドールが無い、はすべて false
bool IsPartHeld(World& world, EntityID part);

// 作動中のラグドールのボーンパレットを組む。
// skinnedMesh の**直子**のうち骨追従部位 (Part + joint 名 + LocalTransform) を集め、
// M48g の v1 規約 `partLocal == jointGlobal` を**逆に読んで** jointGlobal を得る。
// 部位を持たない骨は locals (アニメのポーズ) から階層合成で埋まる。
//
// ★入力は ECS 状態 (部位の LocalTransform) だけ = **純関数**。RenderSystem::Render() は
//   ビュー毎に呼ばれる (SceneView / GameView / AssetPreview / ProbeBaker …) ので、
//   ここにキャッシュや「前フレームの値」を混ぜると**同じ tick でビュー毎に絵が割れる**。
// ★逆行列を 1 つも使っていない。部位が直子である限り partLocal がそのまま jointGlobal
//   なので、`XMMatrixInverse` も `XMMatrixDecompose` も要らない (どちらも構成間で
//   ビットが割れうる — PartFollowSystem が分解を手書きしているのと同じ配慮)
void BuildBonePalette(World& world, EntityID skinnedMesh, const SkinnedModel& model, int clip,
                      float timeSec, std::vector<DirectX::XMFLOAT4X4>& out);

} // namespace ragdoll
} // namespace mye
