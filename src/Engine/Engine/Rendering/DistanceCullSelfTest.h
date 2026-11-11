//====================================================================================
//                          DistanceCullSelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/11/2026
//                                          距離カリングの判定と RenderCulling コンポーネントの回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 実効距離の最小値の規則・AABB までの距離・境界・掛けるビューの純関数と、RenderCulling の登録・NoHash・
// シーン保存の往復・プレハブ上書き。全項目成功で true
bool RunDistanceCullSelfTest();

} // namespace mye
