//====================================================================================
//                          DisturbanceRender.h
//  MyEngin/ 秋田蓮音                                                       11/08/2026
//                                          かき乱しフィールド (M96) の sim 状態を描画への転送指示へ詰め替える
//====================================================================================
#pragma once

#include <cstdint>

namespace mye {

class World;
class DisturbanceState;
struct DisturbanceFieldUpload;
struct DisturbanceVolumeUpload;

// 場を持つエンティティ (entity index 昇順で先頭 kMaxDisturbanceFields 個、sim と同じ) のうち 2D 層を転送指示へ詰める。
// state が null、または場のスロットがまだ無い (編集中・Play の最初の step の前) 層は、状態を読まず
// restDensity の定数として描く指示になる。描画レーンは sim の状態を作らない。
// 戻り値: 層の数 (0 = 場が無い = 従来と同じ絵)。Volume は BuildDisturbanceVolumeUpload が受け持つ
int32_t BuildDisturbanceFieldUpload(World& world, const DisturbanceState* state, DisturbanceFieldUpload& out);

// 上と同じ並び (entity index 昇順で先頭 kMaxDisturbanceFields 個) のうち Volume を転送指示へ詰める (最大 4 個、同じ順)。
// 状態が無い Volume は restDensity の定数として描く指示になる。戻り値: Volume の数 (0 = 従来と同じ絵)
int32_t BuildDisturbanceVolumeUpload(World& world, const DisturbanceState* state, DisturbanceVolumeUpload& out);

} // namespace mye
