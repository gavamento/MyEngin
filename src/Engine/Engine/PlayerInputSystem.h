#pragma once
#include <cstdint>

namespace mye {

class World;
class InputActions;
struct InputSnapshot;

// 入力レーン → PlayerInputComponent のミラー書き込み (M52g)。
//
// tick 頭の InputActions::Evaluate の**直後・スクリプト層の直前**に呼ぶ。ここで書いた値は
// そのまま tick 末のワールドハッシュに載る = 「レーン n の入力がレーン n の
// エンティティに届いたか」が 600 tick のリプレイ照合で機械検証される
// (Components.h の PlayerInputComponent にある「なぜミラーを ECS に置くのか」参照)。
//
// ★ゲート対象ではない。TimeControl のポーズ中も入力の評価とミラーは走る (M51g の
//   「スクリプト層 / 入力 / ハッシュは非ゲート」と同じ扱い) — 止めると sim 側から
//   アンポーズできなくなる。
// inputs は playerCount 本のレーン配列 (パッド接続状態を写すためだけに要る)。
// PlayerInput 非使用シーンでは完全 no-op (= 既存シーンのリプレイ不変)
void UpdatePlayerInputMirror(World& world, const InputActions& actions, const InputSnapshot* inputs,
                             uint32_t playerCount);

} // namespace mye
