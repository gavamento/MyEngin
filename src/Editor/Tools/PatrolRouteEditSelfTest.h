//====================================================================================
//                          PatrolRouteEditSelfTest.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          巡回ルートの点の編集 (Inspector / ギズモ共通部分と Undo) の回帰テスト
//====================================================================================
#pragma once

namespace mye {

// PatrolRouteEdit の関数 (点の追加・削除・入れ替え・ワールド/ローカル変換) と、ギズモのドラッグ・Inspector のボタンが
// 1 操作 = 1 Undo エントリになること。D3D もウィンドウも作らない (ImGui の操作そのものは目視)
bool RunPatrolRouteEditSelfTest();

} // namespace mye
