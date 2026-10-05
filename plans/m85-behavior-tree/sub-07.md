# sub-07: 巡回ルート — PatrolRoute コンポーネント・ギズモ編集・Patrol タスク

- 依存: sub-06 (MoveTo を使う。BehaviorTreeSystem と snapshot の版を触るので直列)
- 状態: OK (M85g としてコミット、ハッシュは台帳)
- 往復: 1

## やること
- `PatrolRouteComponent` (TypeId = 登録時点の末尾 + 1。79 の見込み): spec 4.1.10。点 32 個の固定長、ローカル座標、waitTicks、pointCount、mode。Inspector で点の追加・削除・上下移動 (Undo に乗る通常のフィールド編集)。`kSimSnapshotVersion` +1。
- SceneView のギズモ (選択中): 点の球・線・向きの矢印・番号。点のドラッグ移動 (1 ドラッグ = 1 Undo。手本は M82e のコライダーの独立ギズモ編集 `0f659f3` と NavMeshObstacle の描画 `SceneViewWindow.cpp:878-890`)。定数は `gizmo` 名前空間。
- Patrol タスク (spec 4.1.5 / 2. #11 / 2. #18。`failOnStuck` を MoveTo と同じ意味で持つ): 次の点 index と PingPong の向きをノードのインスタンス状態 (BT 節) に。Abort 後の再開は一番近い点 (同距離は index 小)。ルートのエンティティが無い / 点が 0 個なら Failure。
- 最小の巡回 BT `assets\ai\patrol_only.bt.json` + `assets\ai\patrol.bb.json` (spec 2. #10。デモ sub-13 でも使う)。

## やらないこと (このサブでは)
- Play 中の「向かっている点」の強調 (sub-10 のデバッグ線)

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp` (末尾 append)、`SimSnapshot.h` (版)、`AcousticAudioSelfTest.cpp:129`、`src\Editor\Windows\Scene\SceneViewWindow.cpp`、`InspectorWindow.cpp`、`BehaviorTreeSystem.cpp`、`BehaviorTreeSelfTest.cpp`、`LocalizationTable.inl`、`assets\ai\`

## 受け入れ条件 (このサブ)
1. (spec 10) Loop / PingPong / Once、待ち時間、Abort 後に一番近い点から、ルート無しで Failure、同じルートを 2 体が別々の進み具合で共有 — `BehaviorTreeSelfTest`。
2. (spec 10) SceneView で点をドラッグでき 1 ドラッグ 1 Undo — 一時プローブの `--screenshot` 画像 (選択中のルートの線と矢印) と、Undo の手順を SELF_EVAL に記録。可能ならギズモ編集を既存の Undo の SelfTest の形で機械検査。
3. (spec 3) 巡回中に保存 → 復元 → 連続実行一致。
4. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0、既存 replay_verify ジョブが撮り直しなしで PASS (コンポーネント追加の不変確認)。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、一時プローブの `--screenshot`

## 実装メモ (coder が追記)

SELF_EVAL: sub-07 (round 1)
- 実装: PatrolRouteComponent (TypeId = AIStimulusSource + 2 = 79、末尾 append、点の欄は kFieldHidden でシーン保存・ハッシュには入る) / snapshot v38 / BT の Patrol ノード (Task、route キー + acceptanceRadius + failOnStuck、追加状態 BtPatrolState 16 バイト) / Inspector の点の一覧 (追加・削除・上下・待ち、1 操作 1 Undo) / SceneView のギズモ (点の球・線・向きの矢印・番号、点クリックで選んで移動ギズモ、1 ドラッグ 1 Undo) / PatrolRouteEdit (Inspector とギズモが共有する純関数) / assets\ai\patrol_only.bt.json + patrol.bb.json (+ .meta)
- 検証: Debug / Release ビルド 0 警告、Editor --selftest Debug (1 回目 5 件 = Fracture 3 + net V1 2 の既知 flake、2 回目以降 0)・Release 0、Debug Server --selftest 0、check_rules 0、replay_verify PASS (golden 撮り直しなし)
- 詳細は SELF_EVAL (司会への返信) を参照

## フィードバック履歴
- round 1: VERDICT OK (planner)。初回も最近傍 (spec 2. #19)、点クリック優先、専用 UI の Undo を承認。BB の Entity キーの割り当て手段は sub-08 へ (spec 2. #20)。GUI 実操作の目視は sub-08 の回へ
