//====================================================================================
//                          ComputeScriptSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          GameLogic のカーネル表・COMPUTE_KERNELS・ForEach の ABI の回帰テスト宣言
//====================================================================================
#pragma once

namespace mye {

// ABI v29 (M91e) の回帰テスト。
//   - Compute* スロットの往復 (バッファ・ForEach・ジョブ・所有者の受け渡し)、宣言していないカーネルと
//     スクリプトのコールバックの外 (BT タスクなど) からの呼び出しが実行されずに失敗すること
//   - ReplaceKernels / 言語ごとの宣言表の丸ごと差し替え
//   - GameLogic.dll の ComputeProbe を HeadlessSim で走らせ、Start・毎 tick・ホットリロード直後・ソース版ずれで同じ判定になること、
//     生成したカーネル表のハッシュ (PowerShell) がエンジンの C++ 版と一致すること、持ち主の破棄でバッファが解放されること
bool RunComputeScriptSelfTest();

} // namespace mye
