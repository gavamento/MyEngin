# sub-13: デモ `--bt-demo`、replay_verify `bt`、golden `bt`

- 依存: sub-12b
- 状態: OK (M85m としてコミット、ハッシュは台帳)
- 往復: 1

## やること
- デモシーン `--bt-demo` (`ShowcaseScenes.cpp:24-59` の表の末尾、`DemoContent.cpp` に Build 関数、`EngineCli.cpp` のフラグ表と `EngineCliSelfTest.cpp`)。NavMesh の床と遮蔽の壁、`PatrolRoute` 1 本、敵 2 体 (NavMeshAgent + AIPerception + BehaviorTree + AnimatorController)、プレイヤー役 (AIStimulusSource、決まった経路を動くドライバのスクリプト。`NavDemoDriver.cpp` の流儀)。
- 敵の木 `assets\ai\guard.bt.json` + `guard.bb.json`: Selector の
  1. [BB: target IsSet、abort LowerPriority] 追跡 (MoveTo target、observeTarget、PlayAnimation Walk)
  2. [BB: lastKnownPos IsSet、abort LowerPriority] 捜索 (SearchArea usePrediction、Timeout、終わったら ClearBlackboard lastKnownPos)
  3. 巡回 (Patrol)
  と、FindNearestTarget で target / lastKnownPos を書く仕組み (見失ったら target を Clear する部分木、または Parallel の背景)。発見時に SendEvent で僚機へ知らせ、僚機は BB の eventName で反応する。GameLogic の C++ タスクを 1 つ使う (replay 被覆の確認)。**RotateTo を少なくとも 1 回通す** (std::sin / cos / atan2 を使うので、Debug / Release / Server のビット一致を replay_verify で確かめる。sub-03 VERDICT)。
- ルートは `BehaviorTreeComponent` の Entity キーの初期値 (spec 2. #20) で割り当てる。C++ / ABI で BB を書く経路 (sub-11) もデモの僚機で 1 回使う。
- ドライバがプレイヤー役を「見つかる → 壁の裏へ逃げる → 離れる」順に動かし、巡回 → 発見 → 追跡 → 見失う → 捜索 → 巡回へ戻る、が 1 回の実行に入る tick 長 (目安 900 tick) にする。各段階の開始 tick をログに出す (BT の状態遷移ログ)。
- replay_verify に `bt` ジョブ (`tools\replay_verify.bat` の 4 か所: ジョブ一覧 `:82`、`:job_bt`、`:failed` の診断、`:diagnose` の一覧)。
- shot_verify に golden `bt` (`tools\shot_verify.bat`、frame は追跡か捜索が見える tick、`MYE_SHOT_SKIP_BT`)。`tests\golden\bt.png` を `--update` で作る (bt だけ)。

## やらないこと (このサブでは)
- 既存 golden の更新 (`nav` の既知 FAIL は f6c7bef 起因、ユーザー判断待ち。触らない)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Demo\ShowcaseScenes.cpp`、`DemoContent.{h,cpp}`、`src\Engine\Engine\App\EngineCli.cpp`、`EngineCliSelfTest.cpp`、`src\GameLogic\Scripts\BtDemoDriver.cpp` (名前は任意)、`assets\ai\`、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tests\golden\bt.png`
- メモ `demo-entity-order-is-rng-stream`: デモへ物を足すなら関数の末尾 (新しいデモ関数なので既存デモは触らない)

## 受け入れ条件 (このサブ)
1. (spec 15) `--bt-demo` の 1 回の実行で 5 段階の開始 tick がログに順に出る (ログの写しを SELF_EVAL に)。
2. (spec 15) replay_verify の `bt` ジョブ (Debug 記録 / Debug 検証 + snapshot stress / Release / Server.exe) PASS、既存ジョブも全 PASS。
3. (spec 15) shot_verify の `bt` が PASS (golden 作成後の再実行)。他の golden の結果を報告 (`nav` は既知の FAIL として切り分け)。
4. 実操作: Editor で `--bt-demo` を Play し、BT 窓のライブ表示で発見時の Abort が見える画像。
5. (spec 16) ビルド 0 警告、selftest 新規 FAIL 0、check_rules 0。

## 検証コマンド
- Debug / Release ビルド、`tools\replay_verify.bat`、`tools\shot_verify.bat`、Editor `--selftest` 両構成、Server `--selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL)
- 実装: `--bt-demo` (DemoContent.cpp `BuildBtShowcaseScene`、ShowcaseScenes.cpp の表、EngineCliSelfTest.cpp)、GameLogic `BtDemoDriver` (プレイヤー役の移動 + 見張り A の BB を `BtGetBlackboard` で読んで段階をログ + 僚機 B の route を `BtSetBlackboard`)、assets\ai の `guard.bt.json` / `guard_sense.bt.json` (SubTree) / `guard.bb.json` / `guard.controller.json` + 動きの無いクリップ 2 本、replay_verify の `bt` ジョブ、shot_verify の `bt` (frame 260)、`tests\golden\bt.png`。
- 段階ログ (Debug 記録 / Debug 検証 / Release 検証 / Server 検証で同一): tick 1 Guard B route assigned / 196 spot / 198 Guard B searches (GuardAlert) / 208 chase / 307 lost sight / 308 search / 400 back to patrol。
- RotateTo は追跡の頭で通る (tick 208〜213 の activeNodeId = 29、その後 MoveTo)。BtProbeTask は発見の一拍 (tick 196〜207、12 tick)。
- 仕様との差分: [逸脱] 追跡の abort は LowerPriority のまま (仕様どおり)、ただし sub-13.md の「5 段階」は BB の `stage` (Int) に各枝が書く値 + `target` の外れで判定 (見失うは target が外れた tick、捜索の開始と同じ tick か直前)。[追加] 感知を SubTree `guard_sense.bt.json` に切り出した (3 か所の背景で共用 = SubTree の実使用)。[追加] 僚機への知らせは SendEvent の宛先を Entity キー `buddy` にした (宛先 All だと送り主自身にも届いて自分の捜索位置を上書きするため)。buddy は BehaviorTreeComponent の Entity キー初期値 (A: route + buddy、B: buddy)。[追加] 動きの無いクリップ 2 本 + `guard.controller.json` を assets\ai に足した (既存の demo.controller のクリップは root の LocalTransform.position を書くので、AnimatorController を付けた見張りが原点へ飛ぶ — 実測)。[追加] 感知ループは先頭に Wait 3: 生成直後の tick 0 は WorldMatrix が単位行列のため、全員が原点にいる扱いで知覚が立ち「発見」が tick 1 に誤発火した (実測)。[逸脱] replay_verify の TICKS は 600 のまま (目安 900 より短いが、5 段階は tick 400 までに収まる)。
- 検証: Debug / Release フルビルド 0 警告。Editor --selftest Debug 1 回目 0 FAIL (既知 flake 0 件) / Release 0 FAIL / Server Debug 0 FAIL。check_rules 0 error 0 warning。replay_verify: 1 回目 PASS (17 ジョブ。bt は Debug 記録 / Debug 検証 + snapshot stress / Release / Server.exe すべて "VERIFY PASS: 600 ticks hash-identical")。2 回目は nav (Release 検証が tick 0 の起動中に未処理 C++ 例外、crash bundle bin\x64\Release\crash\20261006_111539) と flow (Debug 検証が約 9 分無反応で手動 kill) で FAIL — bt を含む他の 15 ジョブは PASS、ハッシュ照合まで届いておらず (mismatch マーカー無し)、原因は未特定。3 回目は全 17 ジョブ PASS (bt / nav / flow を含む)。flaky は新規 (既存の cook キャッシュ並列コールドスタートが疑わしいが未確認)。shot_verify: bt = PASS (maxDiff 0、2 回)。他は parts / joints / acoustic_forward / acoustic_deferred / fracture_after / nav が FAIL — **HEAD 3d20b33 の clean な worktree で同じ shot_verify を回しても同じ 6 枚が同じ数値で FAIL** (parts 198/3625、joints 208/137、acoustic 83/596 と 82/594、fracture_after 150/192、nav 214/21664) なので sub-13 起因ではない。nav は既知 (f6c7bef)、他 5 枚は台帳に載っていない既存の FAIL (parts / joints / fracture_after は物理・スキンの終端状態、acoustic は不明)。--update は一切していない。
- 画面確認 (受け入れ 4): ユーザーが Minecraft を前面で操作中だったので実入力は使わず、一時プローブ (EditorApp に環境変数 MYE_PROBE_BT で Guard A を選択して BT 窓で guard.bt.json を開く、撮影後に削除済み) + `Editor.exe --bt-demo --autoplay --warp --screenshot --shot-frame 212` で撮った (scratchpad の probe_f212.png / probe_f198.png)。Live: Guard A - running、実行中ノード (RotateTo 側) が緑の太枠、巡回を Abort した橙の矢印、BB パネルに target / quarry = Player、lastKnownPos、route = Route A、buddy = Guard B、stage = 2、probeStarts = 1 / probeTicks = 12。
- 自己採点: 仕様適合 4 (900 tick 目安でなく 400 tick で完結 / 実入力でなく一時プローブの撮影) / 正しさ 4 (replay 3 回中 2 回 PASS、1 回の flaky 原因が未特定) / コード品質 4 / テスト 4 (--bt-demo の CLI 検査 + replay + golden。ドライバのロジックの単体テストは無し)。
- 不安: replay_verify 2 回目の nav 起動時クラッシュと flow ハングの原因が未特定 (3 回目は PASS)。既存 golden の 5 枚が HEAD で FAIL (台帳の「nav だけ」と食い違う)。見失う瞬間の tick は SearchArea の開始より前後する (ログの 307 / 308)。
- 申し送り (sub-14 / reviewer): docs\test_checklists.md に --bt-demo の手動確認項目を足していない (ADR-025 と一緒に)。C# タスクはデモに入れていない。`stage` は BtDemoDriver が読むための BB キー (ゲーム側の意味は無い)。
- 木の形: 根 Selector [追跡 (target IsSet) / 警報 (alertPos IsSet) / 捜索 (lastKnownPos IsSet) / 巡回]。感知 (FindNearestTarget currentlySensedOnly、見えなければ target を Clear) は SubTree `guard_sense` を各枝の SimpleParallel の背景に入れる (SimpleParallel の主枝はタスクでなければならないため根には置けない)。

## フィードバック履歴
- round 1: VERDICT OK (planner)。5 段階のログ、replay_verify の bt ジョブ (3 回とも PASS、全体は 3 回中 2 回 全 PASS)、golden bt PASS、RotateTo / BtProbeTask / ABI の被覆を確認。2 回目の nav (Release 検証の起動中の未処理 C++ 例外) は crash.txt の直前のログがモデルの cook (並列ジョブの cold cook) で止まっており、BT の読み込みより後・tick 0 の前 = M85 起因の根拠なし。別件として司会へ
