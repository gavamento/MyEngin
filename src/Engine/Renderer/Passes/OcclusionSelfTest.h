//====================================================================================
//                          OcclusionSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          GPU オクルージョンの回帰テスト宣言
//====================================================================================
#pragma once

namespace mye {

// max-Z HZB の縮小 (CPU 鏡と GPU の一致、min 版が不変)、保守的な AABB 判定の境界値、
// Deferred / Forward の ON/OFF 画素一致 (履歴なし / 定常 / カメラカット)、リソース作成失敗の局所化。
// GPU の部分は WARP デバイスで走らせる。全て通れば true
bool RunOcclusionSelfTest();

} // namespace mye
