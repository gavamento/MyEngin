//====================================================================================
//                          FeelMathSelfTest.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りの共有数学 (Shared/FeelMath.h、M94a) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// イージングの端点と連続性、多項式 sin / exp2 の精度、カスタムカーブの Hermite 評価、
// ばねの安定性 (硬い設定でも発散しない)、SmoothDamp の収束、値ノイズの範囲と再現性を確かめる。
// 決定論 (Debug / Release の一致) そのものは replay_verify の feel ジョブが見る
bool RunFeelMathSelfTest();

} // namespace mye
