//====================================================================================
//                          ComputeServiceSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach のエンジン核 (ストア・判断・Fx GPU) の回帰テスト宣言
//====================================================================================
#pragma once

namespace mye {

// ComputeBufferStore / DecideExecution / ComputeService / FxGpuBackend の回帰テスト (M91c)。
//   - バッファストアの決定的な採番・上限・無効ハンドルの無害化
//   - 決定表 (spec 4.1.4) の全行と、宣言なしが他の判定より先に効くこと
//   - CPU 実行 (C++ / C# の呼び出し元、ジョブ内拒否、バッファ不変、対応表)
//   - Fx の GPU 経路 (WARP): 遅延 Read、Wait、枠、デバイス消失からの復旧
bool RunComputeServiceSelfTest();

} // namespace mye
