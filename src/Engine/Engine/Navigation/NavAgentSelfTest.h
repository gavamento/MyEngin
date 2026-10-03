//====================================================================================
//                          NavAgentSelfTest.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          NavMeshAgent・dtCrowd・SimSnapshot の Nav 節の回帰テスト (M82c)
//====================================================================================
#pragma once

namespace mye {

// 物理 (CharacterController) と TransformSystem を回す最小の tick で、次を確かめる:
//   - Agent が段差・坂を越えて目的地へ着く (Arrived)。届かない目的地は部分経路 (pathPartial)、
//     NavMesh 外は NoPath、CharacterController が無ければ Inactive
//   - すれ違いの最小距離: 回避ありは半径の和以上、回避なしは下回る
//   - tick T の SimSnapshot を空の NavSystem へ復元して N tick 進めた結果が、連続実行とビット一致する
//   - Nav 節の中身のバイト列が 撮る -> 戻す -> 撮る で一致する
//   - NavMesh 系が無い / Agent が居ないシーンのワールドハッシュを動かさない
//   - 容量 (128) を超えた Agent は Inactive。128 体の Update 時間と Nav 節の大きさを計測してログに出す
bool RunNavAgentSelfTest();

} // namespace mye
