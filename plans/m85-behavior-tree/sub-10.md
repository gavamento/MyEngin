# sub-10: BT 窓 (3) — ライブ表示とタイムライン操作中の表示、SceneView のデバッグ線

- 依存: sub-09
- 状態: 未着手
- 往復: 0

## やること
- ライブ表示 (spec 4.3): Play 中、選んだエンティティ (BehaviorTreeComponent を持つ) の木を BT 窓に出し、実行中の経路を緑の太枠、直前の Abort (どのノードの Decorator が何を止めたか) を橙の矢印 (30 tick で薄れる)、BB パネルに現在値。窓は毎フレーム `BehaviorTreeSystem` から取り直す (spec 2. #15)。このために BehaviorTreeSystem に読み取り専用の問い合わせ口 (実行中ノードの列、直前の Abort の記録、BB の値) を足す。Abort の記録は表示用なのでハッシュに入れない・BT 節にも入れない (巻き戻し後は空からでよい) — 入れるなら理由を書く。
- タイムライン (TimeTravel) で巻き戻した tick でも同じ表示 (BT 節が復元されるので取り直すだけで出るはず。出なければ原因を書く)。
- 選んだエンティティの木と窓で開いている木が違うときは「表示中の木と違う」と出し、ワンクリックでその木を開く。
- SceneView のデバッグ線 (`drawDebug`、NoHash): MoveTo の目的地への線、SearchArea の点、Patrol の向かっている点、頭上に実行中のタスク名 (知覚の `AppendDebugLines` の流儀、`TickRunner.cpp:481-502`)。

- (sub-06 VERDICT で追加) SubTree は実行時に平らに展開され、部分木のノード id は振り直される (`BehaviorTreeSystem::FindInstance(e)->tree` が実行木)。展開時に各ノードへ「由来の木の GUID と元の id」(導出値、BT 節・ハッシュには入れない) を持たせ、BT 窓は (1) 親の木を開いているときは SubTree ノードを「中で実行中」として強調、(2) 部分木のアセットを開いているときは元の id で強調する。Abort の矢印も同じ対応表を通す。

## やらないこと (このサブでは)
- What-if の非ライブ分岐の表示 (spec 3. やらない)

## 触る場所 (planner の見立て)
- `BehaviorTreeWindow.*`、`BehaviorTreeSystem.{h,cpp}` (問い合わせ口・AppendDebugLines)、`TickRunner.cpp`、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 12) Play 中の画像と、タイムラインで巻き戻した後の画像 (一時プローブ。実行中ノードの強調が tick に合っていることを、画像と同じ tick の `activeNodeId` のログで照合)。
2. Abort の矢印が出る画像 (デモの「発見で巡回を Abort」の瞬間)。
3. デバッグ線の画像 (MoveTo の線・Patrol の点)。
4. replay_verify の既存ジョブが撮り直しなしで PASS (デバッグ線が sim に触れていない確認)。
5. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、Editor `--selftest` 両構成、`tools\check_rules.ps1`、`tools\replay_verify.bat`、一時プローブの `--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
