//====================================================================================
//                          SimParallelSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/09/2026
//                                          sim の並列化 (ADR-028) の jobs あり / なし一致の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// 並列化した 4 系 (CPU 粒子 / Perception / TwoBoneIk + PartFollow) を、同じ初期状態から
// jobs あり (ワーカー起動) と jobs なし (直列) で毎 tick 回し、ワールドハッシュと各系の出力が
// 一致することを確かめる。ワーカーへ実際に配ったこと (parallelBatches の増加) も確かめる。
// 実シーンでの照合は tools\replay_verify.bat の jobsab ジョブ。
bool RunSimParallelSelfTest();

} // namespace mye
