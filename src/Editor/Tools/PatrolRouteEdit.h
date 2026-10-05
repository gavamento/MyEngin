//====================================================================================
//                          PatrolRouteEdit.h
//  MyEngin/ 秋田蓮音                                                       10/06/2026
//                                          巡回ルートの点の編集 (Inspector とシーンビューのギズモが共有)
//====================================================================================
#pragma once
#include <DirectXMath.h>

#include "Engine/Core/Ecs/Components.h"

namespace mye {

// 巡回ルート (PatrolRouteComponent) の点の編集。Inspector のボタンとシーンビューのドラッグが同じ関数を通る。
// どれも Undo の記録は呼び出し側 (UndoStack::Record / BeginRecord〜EndRecord) が持つ

// 末尾へ 1 点足す。最初の点は原点、以降は直前の点から +Z へ kPatrolAddStep ずらす。待ちは直前の点の値を引き継ぐ。満杯なら false
bool PatrolRouteAddPoint(PatrolRouteComponent& route);

// index の点を消して後ろを詰める。範囲外なら false
bool PatrolRouteRemovePoint(PatrolRouteComponent& route, int index);

// index の点を delta (-1 = 上へ / +1 = 下へ) だけ隣と入れ替える (点と待ちを一緒に動かす)。端・範囲外なら false
bool PatrolRouteMovePoint(PatrolRouteComponent& route, int index, int delta);

// ローカル座標の点 -> ワールド座標 (routeWorld = ルートのエンティティのワールド行列)
DirectX::XMFLOAT3 PatrolPointLocalToWorld(const DirectX::XMFLOAT4X4& routeWorld, const DirectX::XMFLOAT3& local);

// ワールド座標 -> ルートのローカル座標。ワールド行列が特異 (スケール 0 など) なら false で out は触らない
bool PatrolPointWorldToLocal(const DirectX::XMFLOAT4X4& routeWorld, const DirectX::XMFLOAT3& world, DirectX::XMFLOAT3& out);

} // namespace mye
