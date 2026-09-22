/*----
 ComputeAbiSelfTest.h  Compute ABI v21 の回帰テスト宣言 (M78e)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once

namespace mye {

// WARP デバイス上で Create→Set*→Dispatch→Release と失敗契約を検証する。
// Editor.exe --selftest から呼ばれる。
bool RunComputeAbiSelfTest();

} // namespace mye
