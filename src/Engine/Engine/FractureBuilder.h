//====================================================================================
//                          FractureBuilder.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片エンティティの事前生成 (root proxy)
//====================================================================================
#pragma once
#include "Engine/Core/EntityID.h"

namespace mye {

class World;
struct FractureAssetHandle;

// ---- 破片エンティティの事前生成 (M80f) ----
// spec §4.1「破片エンティティ」どおりに root の子として破片を組む
// (RagdollBuilder.cpp と同じ置き場の流儀。sim には破断ロジックを足さない — 純粋な構築器)。
//
//   root (Rigidbody(compoundColliders), MeshRenderer=元メッシュ)
//    ├ Frag0  LocalTransform / MeshRenderer(#frag0) / Collider(shape=5,#frag0#hull) / FracturePiece(index=0)
//    │  └ _cap  MeshRenderer(#frag0#cap)
//    └ ...
//
// 既存の `FracturePiece.root == root` な子を先に消してから組み直す (再生成。2 回呼んでも
// 子は倍にならない)。root に Rigidbody が無ければ付けて compoundColliders=true にし、
// root 自身の Collider は外す (複合の子と二重に当たらないように。警告 1 回)。
// 戻り値は作った破片数 (asset.pieces が空 / root が無効なら 0)
int BuildFracturePieces(World& world, EntityID root, const FractureAssetHandle& asset);

// root 直下の `FracturePiece.root == root` な子の数 (ログを出さない照会版、M80i)。
// Inspector が「生成済み」の表示 (焼き直後、まだ何も割れていない状態) に使う。
// ValidateFracturePieces は不一致のたび ERROR を出すため、UI ポーリングには使えない
int CountFracturePieceChildren(World& world, EntityID root);

// root の Destructible が資産 (asset) と整合するか。**階層を辿らず** world 中の全 FracturePiece
// から root==root なものを集めて判定する — 割れて塊がルートの親の下へ移った後 (broken==true)
// も見失わない。判定規則は FractureSystem::Update と共有 (FracturePieceIndicesMatchAsset)
bool DestructiblePiecesMatchAsset(World& world, EntityID root, const FractureAssetHandle& asset,
                                  bool broken);

} // namespace mye
