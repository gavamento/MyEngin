/*----
 ProjectEffectRunnerSelfTest.h  ProjectEffectRunner の回帰テスト宣言 (M78b)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once

namespace mye {

// ProjectEffectRunner (M78b) のヘッドレス回帰テスト。
// 挿入点ソート / 空スタック恒等 / 優先度安定ソートを検証する。
// Editor.exe --selftest から呼ばれる。
bool RunProjectEffectRunnerSelfTest();

} // namespace mye
