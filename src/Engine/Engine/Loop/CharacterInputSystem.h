#pragma once
#include <cstdint>

namespace mye {

class World;
class InputActions;
struct InputBufferState;
struct InputSnapshot;

// 入力 → CharacterController の橋渡し (CharacterInputComponent、M97a)。
//
// CharacterInput + CharacterController + LocalTransform を持つ有効なエンティティについて、入力レーンの
// 直接キーと actions.json の軸 / アクションから CharacterController の moveInput / jumpSpeed を書く
// (kCameraRelative3D では LocalTransform の向きも書く)。CharacterInput が無いシーンでは何もしない。
//
// 呼ぶ位置: tick 頭の InputActions::Evaluate / UpdateBuffers の後、**スクリプト層 (フェーズ 3) の前**。
//   - スクリプトより前なので、スクリプトが同じ tick に CharacterMove / CharacterJump を呼べばそちらが勝つ
//     (気絶や演出で操作を奪える)。フェーズ 3.4 以降の NavMeshAgent / GridAgent2D / Agent / ルートモーションも
//     moveInput を書くので、併用するとそちらが勝つ。排他の仕組みは持たない
//   - 物理と同じゲート対象 (TimeControl のポーズ・ヒットストップ中は呼ばない)。止まっている間に書いた
//     jumpSpeed が再開の tick で発火するのを避ける。止まっている間の「押した瞬間」は拾わないので、
//     取りこぼしたくないジャンプは actions.json のアクションに bufferTicks を付ける
//
// 決定論: 読むのは記録済みの入力 (inputs と、それを評価した actions / inputBuffer) と World だけ。
// 回す順は entity index 昇順 (先行入力の消費 ConsumeBuffered が共有の sim 状態を書くため、順序を明示的な鍵で決める)。
// カメラの向きは WorldMatrix (描画時にも更新される派生値) ではなく LocalTransform の親子連鎖から組む。
//
// inputs は playerCount 本のレーン配列 (null なら直接キーは無入力)。戻り値は駆動した数 (selftest 用)
uint32_t UpdateCharacterInput(World& world, const InputActions& actions, const InputSnapshot* inputs,
                              InputBufferState& inputBuffer, uint32_t playerCount, float dt);

} // namespace mye
