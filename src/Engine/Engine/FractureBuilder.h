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

// root の Destructible が参照する資産の破片数と、直子の `FracturePiece.index` の集合が
// 過不足なく一致するか (spec §4.1 エッジケース)。asset==nullptr (.mfrac が見つからない/
// 読めない) も不一致として扱う。不一致なら ERROR を 1 回出して false を返す — 呼び出し側
// (sub-07 の破断ロジック) はこの Destructible を「無効」として扱い、破断しない
bool ValidateFracturePieces(World& world, EntityID root, const FractureAssetHandle* asset);

// root 直下の `FracturePiece.root == root` な子の数 (ログを出さない照会版、M80i)。
// Inspector が毎フレーム「資産と子が一致しているか」を表示するために使う —
// ValidateFracturePieces は不一致のたび ERROR を出すため、呼び出し元 (FractureSystem) 以外の
// UI ポーリングには使えない
int CountFracturePieceChildren(World& world, EntityID root);

} // namespace mye
