# review-1 — m85-behavior-tree

- 対象: baf29bf0632d5daccd2b0f8496513c704b7d1237..HEAD (e47b6c8)。別件コミット 1c8a69e は回帰が無いことだけ確認した
- 日付: 2026-10-06

```
REVIEW: PASS
round: 1
軸 (1-5):
  製品の深度: 4 — 目的 (spec 1.) の「見張る → 追う → 見失って探す → 巡回へ戻る」を、アセットだけで組んだ木 (guard.bt.json + SubTree guard_sense) が 1 回の実行で通した (tick 196 spot / 198 B が GuardAlert で捜索 / 208 chase / 307 lost / 308 search / 400 back to patrol)。600 tick の後も破綻しない: tick 520 のダンプでは B が北側で再発見して A へ知らせ、A が再び SearchArea (activeNodeId 20、lastAbortTick 438) に入っている = 2 周目も木として筋が通る。壊れた .bt.json の防波堤 (型違い・非有限・範囲外の丸め、BehaviorTreeLibrary.cpp:241-263)、復元時の形の不一致 (BehaviorTreeSystem.cpp:2435-2445)、コンポーネント除去時に Agent への書き込みを戻す経路 (同 1957-1961) も確認した。減点は minor 1 (再有効化時の BB の扱いが spec の文面と食い違う)
  機能性: 4 — 受け入れ 1〜13・15〜17 は自動テストと実行で確認できた (下の検証欄)。14 / 18 (C# の実走) は今回は再実行していない (C# は replay の被覆外で、coder の一時プローブの記録だけ)。新規 FAIL 0、replay_verify 17 ジョブ PASS、golden bt は maxDiff 0。減点は minor のみ
  ビジュアルデザイン: 4 — Runtime.exe で --bt-demo を frame 260 / 360 / 520 で撮った。260 は golden と完全一致。追跡中の MoveTo の線 (桃) と十字、捜索中の SearchArea の円と点 (橙) は床の水色と区別できる。BT 窓は今回プローブを作り直さず、coder の画像 (bt_window_demo.png・c2.png) で確認した: ノードの箱・Decorator の帯・接続線の番号・ライブの緑枠・Abort の橙の矢印・日本語 UI。全体表示の倍率では箱の中の 2 行目の文字 (「ticks=20」など) が小さく読みにくいが、ズームで読める (規約に基準が無いので指摘しない)
  コード品質: 4 — 層の向き (Engine/AI → Editor は読むだけの EngineContext.behaviorTree)、ABI は末尾追加・版表・C# ミラー (check_rules 0)、3 点セット (BT 節・SimRefs・SimSources の内容ゲート) に抜けが無い (SimRefs を組む全箇所を grep: EngineLoop.cpp:584、HeadlessSim.cpp:372、ClientSimRunner は EngineLoop の refs を受ける)。コメントは理由を書いている。減点は minor 2 件
指摘:
  1. [minor] 宛先: planner — spec 4.1.9 は「BehaviorTreeComponent が付いた・**有効になった tick に BB を初期値にして**根から始める。無効になったら…止める (BB は保持)」。実装は再び有効にしても BB を初期値へ戻さない (保持した BB のまま根から)。テストもこの挙動に依存しており (BehaviorTreeSelfTest.cpp:1553-1558: flag を立てて enabled を落とし、戻した後に flag = true のまま入り直すことを前提にしている)、ADR-025 決定 5 (docs\adr\ADR-025-behavior-tree.md:96-98) は再有効化の扱いを書いていない — 根拠: BehaviorTreeSystem.cpp:2064-2078 (無効では hasInstance を残す) と 2080-2089 (InitBlackboard は表が無いときだけ) — 期待: 「有効になった tick」を「初めて有効になった tick (表を作る tick)」と読む実装を正として、spec 4.1.9 の文面と ADR-025 決定 5 に「再び有効にしたときは BB を保ったまま根から」と 1 行足す。逆に初期値へ戻すのが意図なら coder 宛ての修正になる
  2. [minor] 宛先: coder — VisitPatrol で、次の点へ進むときの PatrolPointWorld の戻り値を見ていない。失敗すると target が前の点の座標のまま残り、その点をもう一度目的地に書く — 根拠: src\Engine\Engine\AI\BehaviorTreeSystem.cpp:1137 (同じ関数の 1092 行では失敗を Failure にしている)。親付きのルートで WorldMatrixComponent が無いときだけ起きるので、今のデモと既存のテストには出ない — 期待: 1092 行と同じく、失敗したら EndBody(Failure) にする
  3. [minor] 宛先: coder — docs\test_checklists.md の M84 節の「v26」の行を v27 = 158 に書き換えたまま (sub-14 VERDICT nit#1 が未解消)。各節は「その時点の版」を残す書き方で、M83 節の v25 は残している — 根拠: plans\m85-behavior-tree\sub-14.md:31 の [逸脱] と harness.md:84 — 期待: M84 節を v26 に戻し、M85 節に「外部の GameLogic.dll は v27 = 158 で再ビルド」を 1 行
検証した手段:
  - 読んだもの: spec.md、sub-01〜14 / 12b、harness.md、ADR-025 の決定 4〜5。diff は BehaviorTreeSystem.{h,cpp} 全部、TickRunner / SimSnapshot / WorldHasher / EngineApiTable / EngineAPI.h / ReloadHub / AssetOps / DemoContent (bt-demo) / BtDemoDriver / replay_verify.bat / shot_verify.bat / PartSelfTest、BehaviorTreeLibrary の ReadParam、EditModel の Connect / SetRoot / MoveNode、InspectorWindow の AgentBrain 警告。「仕様との差分」に無い変更は見つからなかった (PartSelfTest・SourceControlSelfTest・AcousticAudioSelfTest の変更は版の追従)
  - ビルド: MSBuild Release / Debug /p:MyeWarnAsError=true → どちらも EXIT 0 (警告で止まらない)
  - bin\x64\Release\Editor.exe --selftest → exit 0、FAIL: 0 件、PASS 6702 行。BehaviorTree / PatrolRouteEdit / BehaviorTreeEditModel すべて ALL PASS。[perf] 100 trees x 30 nodes: Update 35.7 µs、StateHash 117.9 µs (基準 0.5 ms 以下)
  - bin\x64\Debug\Editor.exe --selftest → exit 0、FAIL: 0 件 (既知の flake も今回は 0 件)。Debug の perf は Update 1330.6 µs
  - bin\x64\Release\Server.exe --selftest → exit 0、FAIL: 0 件
  - tools\check_rules.ps1 → 0 error 0 warning
  - tools\replay_verify.bat (MYE_EXTRA_ARGS=--no-audio) → [parallel] all 17 jobs passed in 189.4 s。この前に `--job bt` を単独でも実行して PASS (Server.exe の headless verify も hash-identical)。起動時のクラッシュや無反応は再発しなかった
  - --bt-demo を Release Editor で 900 tick 記録し、段階のログを確認 (上の tick)。WARN は provenance (dirty build) だけで、BT の警告は 0
  - --hash-dump-tick 420 / 520 で BehaviorTree の行を確認 (Guard A / B の activeNodeId・lastAbortTick、instances 2)
  - Runtime.exe --bt-demo --shot-frame 260 / 360 / 520 (shot_verify と同じ引数) → scratchpad の bt_260.png / bt_360.png / bt_520.png。bt_260.png と tests\golden\bt.png の差は PIL で maxdiff 0・bbox None。scratchpad の bt_crop.png (golden の拡大) で線の色を確認
  - BT 窓: coder の一時プローブの画像 scratchpad\bt_window_demo.png・c2.png を見た (今回は作り直していない)
  - UE の規則: Web で公式ドキュメント (Behavior Tree Node Reference の Decorators / Composites / Tasks) を照合した。Observer Aborts の 4 種、Simple Parallel の Finish Mode と「メインは単一の Task」、Time Limit で打ち切って Failure、Notify Observer の OnResultChange は spec 4.1 と一致した。Sequence では None / Self だけ・Cooldown は Abort でも計時を始める、は二次資料と UE 5.4 由来のコード片で一致した。Loop の子が Failure のとき・背景を最初からやり直すか・根の再開が同じ tick か、は公式資料で確かめられなかった (GitHub のエンジンソースは認証が要って読めない)。ADR-025 の「未検証」欄はそのままでよい
  - 未実施: C# タスクの実走 (受け入れ 14 / 18)、shot_verify の全枚数 (bt の 1 枚だけを直接比較した)、SceneView の巡回点のドラッグの実操作 (実マウス操作は使わない指示のため)
前回指摘の消込: (round 1 のため無し)
```
