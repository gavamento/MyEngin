/*----
 ProjectComputeRunnerSelfTest.h  ProjectComputeRunner のヘッドレス回帰テスト宣言 (M78d)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once

namespace mye {

// ProjectComputeRunner (M78d) のヘッドレス回帰テスト。
// DispatchPoint 変換・HasPasses・上限・グループ計算を検証する。
// CameraOverride 時の CS 停止 (ClearPasses / Resolve 非注入) は RenderSystem 側の
// ShouldInjectProjectFxStack を FxStackSelfTest::TestProjectFxStackInjectionPolicy で検証する。
// Editor.exe --selftest から呼ばれる。
bool RunProjectComputeRunnerSelfTest();

} // namespace mye
