# sub-07: 巡回ルート — PatrolRoute コンポーネント・ギズモ編集・Patrol タスク

- 依存: sub-06 (MoveTo を使う。BehaviorTreeSystem と snapshot の版を触るので直列)
- 状態: 未着手
- 往復: 0

## やること
- `PatrolRouteComponent` (TypeId = 登録時点の末尾 + 1。79 の見込み): spec 4.1.10。点 32 個の固定長、ローカル座標、waitTicks、pointCount、mode。Inspector で点の追加・削除・上下移動 (Undo に乗る通常のフィールド編集)。`kSimSnapshotVersion` +1。
- SceneView のギズモ (選択中): 点の球・線・向きの矢印・番号。点のドラッグ移動 (1 ドラッグ = 1 Undo。手本は M82e のコライダーの独立ギズモ編集 `0f659f3` と NavMeshObstacle の描画 `SceneViewWindow.cpp:878-890`)。定数は `gizmo` 名前空間。
- Patrol タスク (spec 4.1.5 / 2. #11): 次の点 index と PingPong の向きをノードのインスタンス状態 (BT 節) に。Abort 後の再開は一番近い点 (同距離は index 小)。ルートのエンティティが無い / 点が 0 個なら Failure。
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

## フィードバック履歴
