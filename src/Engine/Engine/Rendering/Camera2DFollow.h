//====================================================================================
//                          Camera2DFollow.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          2D カメラの追従 (デッドゾーン・ダンピング・先読み・境界)
//====================================================================================
#pragma once

namespace mye {

class World;
class TilemapSystem;

// Camera2DFollow を持つエンティティの LocalTransform.position.xy を、追従対象へ寄せて書く。
// 物理 (3.6) の後・Transform (4) の前に tick ごとに 1 回呼ぶ (stepSim の tick だけ)。
// 対象の位置は LocalTransform の連鎖から合成する (前 tick の WorldMatrix は読まない = ラグ無し)。
// 状態はカメラの LocalTransform だけで、実行系は何も持たない。Camera2DFollow が無いシーンでは走査だけで終わる。
// tiles は boundsFromTilemap の解決用 (null なら手書きの boundsMin / boundsMax だけ)
void UpdateCamera2DFollow(World& world, const TilemapSystem* tiles);

} // namespace mye
