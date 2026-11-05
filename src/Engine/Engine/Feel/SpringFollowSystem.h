//====================================================================================
//                          SpringFollowSystem.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          ばね追従 (SpringFollow、M94g、ADR-035) の更新
//====================================================================================
#pragma once

#include <cstdint>

namespace mye {

class World;

// フェーズ 4 の Transform 確定の直前に 1 回 (物理と同じゲート対象)。SpringFollow が無ければ何もしない。
// 追従は entity index 昇順で回す (アーキタイプの並びに依らない)。戻り値は動かした数 (selftest 用)
uint32_t UpdateSpringFollow(World& world, float dt);

} // namespace mye
