//====================================================================================
//                          MeshShape.h
//  MyEngin/ 秋田蓮音                                                       11/19/2026
//                                          物理・音がメッシュから形を作るときの頂点
//====================================================================================
#pragma once
#include <vector>

#include <DirectXMath.h>

#include "Engine/Renderer/Device/GpuResources.h"

namespace mye {

// 頂点をバインドポーズ (クリップ無し = clip -1 の姿勢) のパレットで動かした位置を out へ。
// 式はスキニングのシェーダ (forward_skinned.hlsl) と同じ: 重みの和が 1e-4 以下の頂点はそのまま、
// それ以外は sum_k w_k * (p * transpose(palette[k]))。joints に無いボーンの影響は足さない。
// positions が空・skin の数が positions と合わない・結果が非有限は false (out は空)
bool ComputeBindPosePositions(const SkinnedModel& model, const std::vector<DirectX::XMFLOAT3>& positions,
                              const std::vector<MeshSkinVertex>& skin, std::vector<DirectX::XMFLOAT3>& out);

// 凸包・メッシュコライダー・ModalSound がメッシュから形を作るときに使う頂点。
// mesh.skinModel が空なら mesh.positions を返す。スキンに結ばれたメッシュ (FBX) はバインドポーズの
// パレットを掛けた頂点を scratch に作って返す — FBX のスキンはエンティティが恒等で、cm→m の 0.01 を
// パレットが持つため、positions のままだと描画より 100 倍大きい形になる。
// スキンが未登録 (後から登録されうる) は nullptr = 呼び出し側は結果をキャッシュしないこと。
// 属性が壊れていてバインドポーズを求められないときは警告して positions を返す
const std::vector<DirectX::XMFLOAT3>* MeshShapePositions(const SkinnedModelLibrary& models, const Mesh& mesh,
                                                         std::vector<DirectX::XMFLOAT3>& scratch);

} // namespace mye
