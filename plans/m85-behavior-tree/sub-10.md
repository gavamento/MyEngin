# sub-10: BT 窓 (3) — ライブ表示とタイムライン操作中の表示、SceneView のデバッグ線

- 依存: sub-09
- 状態: OK (M85j としてコミット、ハッシュは台帳)
- 往復: 1

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

### round 1 (SELF_EVAL)
- 実装: BehaviorTreeSystem に表示用の読み口 (BtInstance::lastAbort = 直前の Decorator Abort の (tick, Decorator のノード id, 止められた葉の id)、`ActiveNodeIndices`、`AppendDebugLines`)。BT 節・ハッシュには入れない (復元後は空)。BtNodeDef に導出値 originTree / originId (SubTree 展開が写したノードだけ)、BehaviorTreeAsset に `OriginTreeOf` / `OriginIdOf` / `DisplayedIdOf`。EngineContext.behaviorTree (読むだけ)。BT 窓は `OnImGui(ctx, selection)` で毎フレーム `RefreshLive` (緑の太枠・SubTree の「中で実行中」・橙の Abort の矢印か枠・BB の現在値・「表示中の木と違う」+ 開くボタン)。SceneView は `DrawBehaviorTreeLabels` (頭上のタスク名)、線は TickRunner から `AppendDebugLines`。
- 画面確認 (一時プローブ。nav-demo にプローブ用エージェントと assets を足して撮り、プローブは削除済み): Play 一時停止 tick 8860 で Inspector の activeNodeId = 7・最後の Abort = 8850、窓は Wait ticks=10 (#7) が緑・矢印は 10 tick ぶん薄れ (scratchpad の c2.png)。巻き戻し (-30) tick 8830 で activeNodeId = 3、窓は Wait ticks=20 (#3) が緑 (d2.png)。tick 8853 まで進めた巻き戻し状態は activeNodeId = 7 と一致・矢印あり (d3.png)。親の木を開くと SubTree が緑 + 「中で実行中」、部分木のアセットを開くと元の id で緑 + 矢印 (h3.png / i1.png / i2.png)。デバッグ線: MoveTo の目的地への桃の線と十字 (g2c.png)、Patrol の向かっている点の黄の十字と杭 (g3c.png)、頭上に Wait / Patrol / MoveTo (f2c.png)。
- 検証: Debug / Release ビルド 0 警告、Editor --selftest Debug 1 回目 0 件 (途中の試走で 1 回目に Fracture 3 + net V1 2 の既知 flake、2 回目 0)・Release 0 件・Debug Server 0 件、check_rules 0 件、replay_verify 16 ジョブ PASS (撮り直しなし)。BehaviorTreeSelfTest に 7 項目追加 (Abort の記録 Self / LowerPriority / Timeout、記録が BT 節・ハッシュに入らない、実行中ノードの列、SubTree の元の id、部分木の中の葉の対応、MoveTo のデバッグ線と Abort の記録)。
- 仕様との差分: [追加] Abort の記録は Decorator 起点 (Self / LowerPriority / Timeout) だけ。SimpleParallel の Immediate が背景を止める Abort は記録しない (Decorator ではないため)。[追加] 巻き戻し (Restore) 直後は記録が空で、矢印は Restore より後に再シムした範囲の Abort だけ出る (spec 通り「空から」)。[追加] 何も開いていない窓でもエンティティを選べば「ライブ」の行と開くボタンを出す。[追加] SceneView の「記録を閉じる」処理は 4 か所目を足していないので集約していない。

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ 1〜5 を画像・自動テスト・replay_verify で確認。Abort 記録の範囲・巻き戻し直後の空・EngineContext の追加を承認
