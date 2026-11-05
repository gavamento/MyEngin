//====================================================================================
//                          FeelTriggerSystem.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          接触で手触りを鳴らす FeelTrigger (M94d、ADR-035) の評価
//====================================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace mye {

class World;
class FeelState;
struct TimeControl;
struct SolidContact;

// 物理 (フェーズ 3.6) の直後に 1 回。FeelTrigger を持つエンティティが 1 つも無ければ何もしない (存在ゲート)。
// 接触列は PhysicsSystem の出力規約どおり key 昇順で、その順に評価する (= 鳴らす順が決定的)。
// 戻り値は鳴らした数 (selftest 用)
uint32_t UpdateFeelTriggers(World& world, const std::vector<SolidContact>& contacts, FeelState& feel,
                            TimeControl& time);

} // namespace mye
