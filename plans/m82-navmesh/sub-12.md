# sub-12: 同じ目的地の渋滞を到着として扱い、Stuck で止めない + レビューの小さな指摘

- 依存: sub-11
- 状態: OK (コミット待ち)
- 往復: 1

## 背景
review-1 #1 (major、planner 宛 = 仕様の穴): 同じ目的地へ向かう複数の Agent が、既定の回避 (avoidanceQuality 2) のままだと Stuck で恒久停止する。
- 2 体では、両方が残り 0.34 / 0.42 m で止まる。
- 4 体では、1 体だけ Arrived で、3 体が Stuck。
- 判定箇所 `NavSystem.cpp:1495-1515` は spec どおりに実装されているので、直すのは仕様の側。

spec 4.1 を、次の 2 点に改めた。
- Stuck は止めない (表示と通知だけ)。
- 目的地の近くにいる、または到着済みの Agent に接していれば、到着として扱う。
あわせて、coder 宛の小さな指摘 #4〜#7 もここで直す。

## やること
1. **Stuck は止めない** (spec 4.1): Stuck にしても dtCrowd の移動目標と moveInput を保ったまま押し続ける。前進 (残り距離が基準から max(radius/4, 1 cm) 以上縮む) が戻ったら `Moving` へ自動で戻す。WARN は Stuck に入った tick に 1 回だけ出す。`EngineAPI.h:715` 付近のコメント (Stuck から戻らない) を新しい規則に合わせる。
2. **渋滞を到着として扱う** (spec 4.1): 前進が 60 tick 止まった Agent が次のどちらかを満たせば、`Arrived` にする。
   - (a) 残り距離が max(stoppingDistance, 2 × radius) 以内。
   - (b) 同じ目的地 (差 ≤ stoppingDistance) で既に `Arrived` の Agent に接している (中心の水平距離 ≤ 半径の和 + 余裕。余裕は名前付き定数)。
   (b) は連鎖する。順序はエンティティキー順で決定的にし、同じ tick の中で連鎖させるか次の tick に持ち越すかを決めて理由を書く。部分経路の到着 (既存) とは両立させる。
3. **SelfTest**: 回避あり (既定) で、2 / 4 / 8 体が同じ目的地へ向かうと、全員がいずれ `Arrived` になること。止まり続けたり、恒久的に Stuck のままの Agent が残らないこと。
   - 塞いでいた壁 (Obstacle) を消すと、Stuck の Agent が `Moving` へ戻って到着すること。
   - 渡りの途中で復元しても連続実行と一致すること (Nav 節にカウンタがあるので維持できているか)。
   - review-1 のプローブ (2 体・4 体) と同じ条件で再現して、直ったことを示す。
4. (#4、minor) `RestoreSimSnapshot` の「どこで失敗しても現世界は無傷」(`SimSnapshot.cpp:554`) を Nav 節が破っている。直し方は次の 2 案から選ぶ。
   - (i) World を差し替える前に、Nav 節の検証で失敗しうる点 (.mnav が読めない、書式) をすべて潰す。
   - (ii) 差し替え後の失敗は「World は変わったが Nav は空」として明記し、`EngineLoop.cpp:1033` 側の扱いを合わせる。
   まず (i) を検討し、無理なら (ii) にして理由を書く。
5. (#5、minor) ベイク結果は、今は Surface の Inspector が描かれているときにしか取り込まれない (`TakeResult` の呼び出し元は `InspectorWindow.cpp:1819-1821` だけ)。選択に関係なく確定するよう、取り込みを Editor の毎フレームの処理 (例: EditorApp のフレーム更新) へ移す。ベイク中に別のエンティティを選んでも .mnav と SceneView が更新されることを、SelfTest か手順で確かめる。
6. (#6、minor) 古いコメントを現状に合わせる。
   - `Components.h:1880` と `Components.cpp:1389`: M82f の UI の記述
   - `Components.h:1964` と `NavSystem.h:57`: y 回転だけの Box は回転箱で切る
   - `PATCHES.md:5`: パッチの件数 (4 件)
   - `PATCHES.md:61`: frand の現在の方式
7. (#7、minor) ADR-023:393 の「通常ビルドは警告 0」を「M82 のファイルからの警告は 0 (既存の ProjectComputeRunnerSelfTest.cpp の C4127 は通常ビルドでも 14 件出る)」と正確に書く。
8. ADR-023 の Stuck / 到着の節と、engine_spec.md 10.9、docs\engine-feature-guide.md 9.4 を新しい規則に合わせる。

## やらないこと (このサブでは)
- 高さの補正 (sub-11)
- ABI の版を上げること (署名は不変。コメントだけ直す)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Navigation\NavSystem.cpp` (`:1495-1515` 付近の到着と Stuck の判定)、`NavAgentSelfTest.cpp`
- `src\Engine\Engine\Replay\SimSnapshot.cpp`、`EngineLoop.cpp:1033`
- `src\Editor\Windows\Scene\InspectorWindow.cpp`、Editor のフレーム更新箇所、`NavBakeService`
- `Components.h/.cpp`、`NavSystem.h`、`external\recastnavigation\PATCHES.md`、`src\Shared\EngineAPI.h` (コメント)
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`、`docs\engine-feature-guide.md`

## 受け入れ条件 (このサブ)
1. (spec 20) 2 / 4 / 8 体、回避あり: 全員が `Arrived`。塞ぎを消すと Stuck → Moving → Arrived。— NavAgentSelfTest
2. #4〜#7 が直っている (差分と、#5 の確認手順)。
3. replay_verify 全 15 ジョブ PASS。両構成の selftest で Nav 系は ALL PASS。shot_verify は nav が PASS、既知の 5 枚は値が不変。焼き直した期待値は「旧値 → 新値、理由」を列挙する。
4. 警告 0、check_rules 0。

## 検証コマンド
- 両構成ビルド、`Editor.exe --selftest` と `Server.exe --selftest` を両構成で直列実行
- `tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-12 (round 1)
実装:
  - NavSystem.cpp:UpdateSurface — Stuck を止めない (stuck 時の crowd 目標解除と moveInput=0 を廃止。status は計算し直し、WARN は入った tick に 1 回)。前進が戻ると Moving。noProgressTicks は kStuckTicks で頭打ち (毎 tick 判定するため)
  - NavSystem.cpp:IsJamArrival (新規メンバ) — (a) remaining <= max(arriveDistance, 2 x radius) (b) 同じ目的地 (差 <= arriveDistance) で Arrived の Agent に水平距離 <= (半径の和) x 2 で接している。wanted (キー順) を走査するので決定的で、同じ tick に先に着いた Agent も使う (連鎖は 1 tick 内)
  - NavSystem.cpp:進捗判定 — 「遠ざかったら基準を取り直す」を「基準から半径以上」に変更 (kStuckRebaseRadiusScale)。渋滞で ±7.5 cm 揺れる Agent が永遠に Moving のまま 8 体中 1 体残った (実測) ため
  - NavAgentSelfTest.cpp — 2/4/8 体 全員 Arrived、塞ぎ消去で Stuck -> Moving -> Arrived、渋滞中の撮影 -> 復元 -> 毎 tick ハッシュ一致。旧 Stuck 試験の「moveInput 0」を「位置が進まず Stuck のまま」へ
  - #4: (ii) を選択。.mnav の読み込みは差し替え後の World が要り事前検証できない (書式は ValidateSnapshot で事前検証済み)。SimSnapshot.h/.cpp と EngineLoop.cpp:SeekTo のコメントに「Nav 節の失敗だけ World 差し替え後に false」を明記 (挙動は不変)
  - #5: InspectorWindow::CommitReadyNavBakes (新規) を OnImGui の先頭 (open の判定より前) で毎フレーム呼ぶ。NavBakeService::ReadyIds (新規) で Ready を走査。Surface が消えていれば WARN して結果を捨てる。DrawNavMeshSurfaceNotes 内の取り込みは削除。NavEditorSelfTest に ReadyIds の検査を 1 件
  - #6: Components.h/.cpp、NavSystem.h、PATCHES.md (4 件・frand の現状)、EngineAPI.h:715 のコメントを更新
  - #7/8: ADR-023 (Stuck の節、警告 0 の言い方、限界の節)、engine_spec.md 10.9、engine-feature-guide 9.4
仕様との差分:
  - [追加] 進捗判定の再基準を「半径以上遠ざかったとき」に狭めた (spec 4.1 は「遠ざかったら取り直す」) — 渋滞の揺れで 8 体中 1 体が Moving のまま残るため
  - [追加] (b) の「接している」を半径の和の 2 倍とした (spec は「半径の和 + 余裕」) — 実測で dtCrowd の分離が 1.07 m (接触 0.6 m) 離した。名前付き定数 kJamTouchScale
  - [追加] Surface が消えたベイク結果は WARN して捨てる
  - [未実装] #5 の「別エンティティを選んだまま確定」の GUI での実操作確認 (InspectorWindow は SelfTest で駆動できない)。ReadyIds と CommitNavBake は SelfTest 済み
検証:
  - Debug / Release Editor.exe --selftest → Nav 系 (NavDeterminism / NavSurface / NavEditor / NavAgent) ALL PASS、[jam] 2/4/8 体 全員 Arrived。exit 1 は既知の Source control 2 件のみ
  - Debug / Release Server.exe --selftest → exit 0、同じ Nav 系 PASS
  - tools\check_rules.ps1 → 0 error 0 warning。両構成ビルドの警告・エラー出力なし
  - tools
eplay_verify.bat → 全 15 ジョブ PASS (nav を含む)。期待ハッシュの焼き直しなし (Nav 節の書式は不変、kSimSnapshotVersion 29 のまま)
  - tools\shot_verify.bat → nav PASS。既知 5 枚は値が不変 (parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 20 を実測で満たす。定数 2 点を実測で調整した (差分欄)
  正しさ: 4 — 2/4/8 体・塞ぎ消去・復元一致を確認。実ゲームの渋滞は未検証
  コード品質: 4 — 判定を IsJamArrival に切り出し、定数に名前を付けた
  テスト: 4 — 3 種の SelfTest を足した。#5 は GUI 操作で確認していない
不安・質問:
  - 再基準を狭めたことで、迂回の始まりが半径未満の経路延長だと停滞として数えられる (60 tick 縮まなければ Stuck 表示。止めないので害は表示のみ)
触ったファイル: src/Engine/Engine/Navigation/NavSystem.cpp, NavSystem.h, NavAgentSelfTest.cpp, src/Engine/Engine/Replay/SimSnapshot.cpp, SimSnapshot.h, src/Engine/Engine/Loop/EngineLoop.cpp, src/Engine/Core/Ecs/Components.h, Components.cpp, src/Shared/EngineAPI.h, src/Editor/Tools/NavBakeService.h, NavBakeService.cpp, NavEditorSelfTest.cpp, src/Editor/Windows/Scene/InspectorWindow.h, InspectorWindow.cpp, external/recastnavigation/PATCHES.md, docs/adr/ADR-023-navmesh.md, engine_spec.md, docs/engine-feature-guide.md, plans/m82-navmesh/sub-12.md
申し送り: なし

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-04)。定数の調整 2 点 (接触 = 半径の和 × 2、取り直し = radius 以上) を spec 4.1 に反映した。#4 は案 (ii) を承認。should: #5 の GUI での確認 (ベイク中に別のエンティティを選ぶ、Inspector のタブを閉じる) は reviewer が行う。取り込みは Inspector の OnImGui の先頭なので、Inspector そのものが描かれない構成で確定するかも見ること。
