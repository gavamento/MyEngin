//====================================================================================
//                          FractureEditorSelfTest.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破壊物 Inspector (M80i) のヘッドレス回帰テスト
//====================================================================================
#pragma once

namespace mye {

// Editor 層の焼き回り (非同期ワーカー・焼き成功結果の確定・Undo/Redo) の回帰。
// D3D もウィンドウも作らない。分割コアそのもの (閉じ判定・切断・凸包) は
// Engine 層の RunFractureSelfTest が持つ — ここは Editor が足した皮 (スレッド境界・
// .mfrac の保存/登録・BuildFracturePieces との配線・Undo) だけを確認する
bool RunFractureEditorSelfTest();

} // namespace mye
