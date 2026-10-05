# sub-06: AnimatorPlay と PlayAnimation、SubTree

- 依存: sub-05
- 状態: 未着手
- 往復: 0

## やること
- Engine に `AnimatorPlay(World&, EntityID, int32_t stateIndex, int32_t durationTicks)` を足す (`src\Engine\Engine\Animation\AnimatorController.{h,cpp}`)。duration > 0 = `transitionTo` / `transitionTick` / `transitionDuration` / `transitionToTime` を立てて遷移開始 (既存の遷移の評価と同じ式で混ぜる、`AnimatorController.cpp:283-370`)、0 = `currentState` を即切り替えて `stateTimeTicks = 0`。名前 → index の引き方 (`ControllerAsset::states[].name`) も関数にする。
- PlayAnimation ノード (spec 4.1.5)。waitForEnd は「そのステートのクリップの長さ (tick) 分」を待つ (loop のクリップでも 1 周)。
- SubTree ノード (spec 4.1.5): 同じ BB アセットでなければ Failure + 1 回警告、親の BB を共有、入れ子 8 段 (`kBtMaxSubTreeDepth`)。SubTree の中の Abort は spec 4.1.1 の優先順にそのまま参加する (UE と同じく部分木の Decorator も親の木の監視に入る)。
- `AnimatorControllerSelfTest` に AnimatorPlay の項目。

## やらないこと (このサブでは)
- ABI の `AnimatorPlay` (sub-11)、`defaultState` の未適用 (spec 7.、触らない)

## 触る場所 (planner の見立て)
- `AnimatorController.{h,cpp}`、`AnimatorControllerSelfTest.cpp`、`BehaviorTreeSystem.cpp` / ノード表 / `BehaviorTreeSelfTest.cpp`
- SelfTest の controller は `assets\anims\demo.controller.json` (Idle / Walk) か、テスト内で組んだ ControllerAsset

## 受け入れ条件 (このサブ)
1. (spec 9) AnimatorPlay の遷移と即切り替え、PlayAnimation の waitForEnd / 名前なし Failure、SubTree の BB 共有・不一致 Failure・9 段目で Failure・部分木の中の Abort — `BehaviorTreeSelfTest` / `AnimatorControllerSelfTest`。
2. (spec 3) SubTree の中で実行中に保存 → 復元 → 連続実行一致。
3. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
