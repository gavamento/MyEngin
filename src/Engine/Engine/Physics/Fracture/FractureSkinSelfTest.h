//====================================================================================
//                          FractureSkinSelfTest.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          スキンメッシュの破壊 (M80j) のヘッドレス回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 骨追従の破片 (FractureSkinBake.h) の回帰テスト。守っている不変量:
//   - 骨に割り当てた破片はすべて幾何的に閉じ (BakeFracture の合否判定を再利用)、
//     外側面の頂点のウェイトから正しい骨に割り当たる
//   - 骨空間へ変換した破片頂点は、割り当てた骨のバインドポーズの jointGlobal を掛け直すと
//     変換前の (ソース空間の) 絶対位置に戻る (相対 1e-5)
//   - 同じ入力から 2 回焼いて骨を割り当てても、.mfrac 相当のバイト列が一致する (決定論)
//   - BuildFracturePieces は SkinnedMesh を持つ root を kinematic にし、骨が割り当たった
//     破片へ PartComponent(joint=骨名) を付ける
//   - アニメ再生中に骨へ追従した破片に球を当てると接着が切れて剛体化し、PartComponent が
//     外れる。残った破片は PartComponent を保ったまま追従を続ける
bool RunFractureSkinSelfTest();

} // namespace mye
