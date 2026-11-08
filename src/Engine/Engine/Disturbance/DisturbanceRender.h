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

// 場を持つエンティティ (entity index 昇順で先頭 kMaxDisturbanceFields 個、sim と同じ) のうち 2D 層を転送指示へ詰める。
// state が null、または場のスロットがまだ無い (編集中・Play の最初の step の前) 層は、状態を読まず
// restDensity の定数として描く指示になる。描画レーンは sim の状態を作らない。
// 戻り値: 層の数 (0 = 場が無い = 従来と同じ絵)。Volume は sub-07 まで描かない
int32_t BuildDisturbanceFieldUpload(World& world, const DisturbanceState* state, DisturbanceFieldUpload& out);

} // namespace mye
