/*----
 ProjectShaderPropertiesSelfTest.h  Properties DSL パース・パック の回帰テスト宣言
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once

namespace mye {

// Properties DSL (M78a) のヘッドレス回帰テスト。
// パース正常系・エラー系・CB パックの順序/サイズを検証する。
// Editor.exe --selftest から呼ばれる。
bool RunProjectShaderPropertiesSelfTest();

} // namespace mye
