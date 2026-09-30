/*----
 FxStackSelfTest.h  FxStackAsset のロード／保存回帰テスト宣言 (M78c)
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once

namespace mye {

// FxStackAsset (M78c) のヘッドレス回帰テスト。
// JSON ロード / 保存 / PropValue 変換を検証する。
// Editor.exe --selftest から呼ばれる。
bool RunFxStackSelfTest();

} // namespace mye
