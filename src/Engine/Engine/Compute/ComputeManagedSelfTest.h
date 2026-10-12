//====================================================================================
//                          ComputeManagedSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          C# の Compute.ForEach と C# の CPU 版の回帰テスト宣言
//====================================================================================
#pragma once

namespace mye {

// M91g の回帰テスト。実際の CoreCLR + Roslyn で C# スクリプトをコンパイルして走らせる (MyeScripting.dll が無い環境では SKIP)。
//   - Kernels.gen.cs の生成 → コンパイル → [UnmanagedCallersOnly] のレンジ関数と [ComputeKernels] の登録 (GameLogic.dll 無しで State が成功する)
//   - C# の CPU 版と C++ の CPU 版の出力のバイト一致 (ホワイトリストの全関数を通るカーネルを含む)。
//     JIT の違い (DOTNET_TieredCompilation=0 など) は環境変数を変えてこのテストを再実行して確かめる
//   - 宣言していないカーネル・大文字小文字違い・スクリプトの外 (BT タスク) からの呼び出しが実行されずに失敗すること。
//     C# の再読込直後も同じ判定になること
//   - RW を KernelId 以外で添字する C# がコンパイルエラーになること
//   - 1M 要素の所要時間の計測 (ログのみ)
bool RunComputeManagedSelfTest();

} // namespace mye
