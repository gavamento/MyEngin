//====================================================================================
//                          NavEditorSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMesh Surface のエディタ側 (Create / Bake / Clear) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// Editor 層の皮 (Create メニューの Undo・非同期ワーカー・ベイク結果の確定・Clear) の回帰。
// D3D もウィンドウも作らない。ベイクそのもの (入力収集・タイルベイク・.mnav) は Engine 層の
// RunNavSurfaceSelfTest が持つ。ここでは「ワーカーが返したバイト列が Engine 層の関数を直接呼んだ結果と一致する」
// ことで、エディタの Bake が同じ関数を通っていることを確かめる
bool RunNavEditorSelfTest();

} // namespace mye
