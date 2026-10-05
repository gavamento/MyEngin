# sub-06: AnimatorPlay と PlayAnimation、SubTree

- 依存: sub-05
- 状態: OK (M85f としてコミット、ハッシュは台帳)
- 往復: 1

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

SELF_EVAL: sub-06 (round 1)
- AnimatorPlay / FindControllerState (AnimatorController.{h,cpp}) と PlayAnimation ノード。
- SubTree は実行木の「平らな展開」(BtExpandSubTrees、BehaviorTreeLibrary.cpp)。部分木のノードを呼び出し側の表へ写し、id は元の最大 id の後ろへ連番で振り直す。BtInstance は 1 つのまま = snapshot v37 のまま (版は上げていない)。部分木の Decorator は親の木の MonitorNode にそのまま入る。
- 展開結果は BehaviorTreeSystem::expansions_ (GUID 引き) にキャッシュし、BtExpansion::IsCurrent (登録の shared_ptr の同一性) で作り直す。
- BehaviorTreeSystem::Update に controllers / clips を末尾の既定引数で追加 (TickRunner が渡す)。
- 検証: Debug / Release ビルド 0 警告、Editor --selftest Debug (1 回目 Fracture 3 + net V1 2 の既知 flake、2 回目 0) / Release 0、Server Debug --selftest 0、check_rules 0 / 0。
- nit 確認: 配達を Update の中へ戻すと BT selftest が 2 件 FAIL (配達は tick の頭の項目と、その前提の項目)。確認後に戻した。

## フィードバック履歴
- round 1: VERDICT OK (planner)。平らな展開・AnimatorPlay の引数追加・waitForEnd の数え方・遷移中の再要求を承認 (spec 8.)。ライブ表示の id 対応は sub-10 へ
