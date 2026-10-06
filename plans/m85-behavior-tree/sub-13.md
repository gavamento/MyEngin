# sub-13: デモ `--bt-demo`、replay_verify `bt`、golden `bt`

- 依存: sub-12b
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
