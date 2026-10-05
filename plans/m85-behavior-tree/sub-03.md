# sub-03: Task 4 種 (MoveTo / RotateTo / SetBlackboard / ClearBlackboard)

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること
- spec 4.1.3 の 4 種 (Wait は sub-01 で済み)。
- MoveTo: NavMeshAgent の `destination` / `hasDestination` を書く (`EngineApiTable.cpp:1360-1376` と同じ書き方)。acceptanceRadius / Arrived で Success、NoPath / Inactive で Failure、Stuck はパラメータ `failOnStuck` (bool、既定 false、spec 2. #17) が false なら Running・true なら Failure (`hasDestination = false` にして止める)、observeTarget、終了と Abort で `hasDestination = false`、navFilter の一時差し替えと戻し、NavMeshAgent 無しで Failure。
  **確認すること**: 到着済みの Agent に同じ地点の MoveTo を 2 回目に出したとき (hasDestination を倒して立て直す) に再探索され Success で終わるか (`NavSystem.cpp:1460-1476` の `SameBits` 判定)。されないなら NavSystem に「hasDestination が偽 → 真で要求し直す」を足し、`nav` / `perception` の replay_verify ジョブが撮り直しなしで PASS することを確かめる (spec 7.)。足したなら仕様との差分に書く。
- RotateTo: LocalTransform の Y 回転、angularSpeedDeg (0 = Agent の値、Agent も無ければ 360)、toleranceDeg、実行中は `updateRotation = false`、終了・Abort で元の値に戻す。
- SetBlackboard / ClearBlackboard: spec 4.1.3。
- MoveTo / RotateTo の状態 (書き換え前の navFilter・updateRotation、最後に書いた目標) は BT 節に入る。

- (sub-01 VERDICT より) ノードの状態: sub-01 の `BtNodeState` は全ノード共通の固定 10 バイト。MoveTo / RotateTo の追加の状態 (目標の前回位置 float3、書き換え前の navFilter・updateRotation) を**全ノード共通の欄に足して全ノードを太らせない**。ノードの種類表 (`BtNodeTypeInfo`) に「その種類が持つ追加状態のバイト数」を持たせ、表がノードごとに固定長の追加領域を割り当てる形を推奨 (BT 節の書式は種類ごとの追加領域を生バイトで書く)。別の形にするなら理由を SELF_EVAL に。BT 節の書式が変わるので snapshot の版を +1。

## やらないこと (このサブでは)
- AI ノード、Patrol、デバッグ線 (sub-10)

## 触る場所 (planner の見立て)
- `BehaviorTreeSystem.cpp` / ノード表 / `BehaviorTreeSelfTest.cpp`、`NavSystem.cpp` (必要時のみ)
- SelfTest の NavMesh は `NavDeterminismSelfTest` / NavSystem の SelfTest が使う固定ジオメトリの作り方を流用

## 受け入れ条件 (このサブ)
1. (spec 6) MoveTo: Arrived で Success / 届かない目的地で Failure / Abort で Agent が止まる / Stuck を作った状況で failOnStuck = false は Running のまま・true は Failure で Agent が止まる / 同じ地点へ 2 回 / observeTarget で目標を追う / navFilter が戻る。RotateTo: 角速度どおり回り tolerance で Success、updateRotation が戻る。Set / Clear の 5 型 — `BehaviorTreeSelfTest`。
2. (spec 3) MoveTo 中に保存 → 復元 → 連続実行一致 (Nav 節と BT 節の両方)。
3. NavSystem を変えた場合: replay_verify の既存ジョブ全 PASS (撮り直しなし)。
4. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`、NavSystem を触ったら `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
