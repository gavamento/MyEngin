//====================================================================================
//                          ComputeSettingsSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          project_settings の compute 節・Compute 欄の行・カーネルのホットリロードの回帰テスト宣言
//====================================================================================
#pragma once

namespace mye {

// M91h の回帰テスト。
//   - "compute" 節の読込 (既定値・不正な値・上書き) と適用 (forceCpu を保つ)、上書きの書き戻し (他のキーを消さない)
//   - project_settings.json の compute 節が contentHash に入ること
//   - Compute 欄の行 (宣言のみ・宣言なしで失敗・両方・宣言あり・カーネル無し・検査違反・版ずれ・C# の版)
//   - ReloadKernelSource (大文字小文字・変更の検出・削除) と、CPU / GPU が別のファイルを読むときの失敗
bool RunComputeSettingsSelfTest();

} // namespace mye
