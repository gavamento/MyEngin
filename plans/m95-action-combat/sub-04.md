# sub-04: 遷移の割り込み (controller v3 canInterrupt、AnimatorPlay の確定)

- 依存: sub-00 (Wave 1。sub-01 / sub-02 / sub-06 と並列)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 最初と最後に両方で `git status` を確認し、SELF_EVAL に貼る
- `AnimatorControllerSelfTest.cpp` への追記は `// ---- M95 sub-04 ----` の直下、`LocalizationTable.inl` は `// ---- M95 ----` の直下
- `AnimatorController.cpp` で触ってよいのは: `ToJson` / `FromJson` の**遷移**ブロック、`root["controller"]` の行、`Update` の **1. 遷移チェック**、`AnimatorPlay`。`Update` の冒頭 (sub-01)、`FromJson` の `clipEvents` (sub-06)、ステートの `skel` (sub-05) は触らない
- 同じ波の他サブのファイル (`Components.*`、`PhysicsSystem.cpp`、`EngineAPI.h`、`AnimationPreviewWindow.cpp`) を触らない

## やること

spec §4.1 C。`ControllerTransition.canInterrupt` を足し、遷移中も `canInterrupt` の遷移を評価して「近い側へ確定 → 新遷移」を行う。
`AnimatorPlay(duration > 0)` の遷移中の挙動も同じ確定規則にする。controller アセットを v3 にし (v1 / v2 は読める)、
Editor の遷移リストにチェックを足す。

## やらないこと (このサブでは)

- 混合ポーズの保存・混ぜ直し (spec §2 #6)
- `hasExitTime = 1` の遷移の割り込み (評価しない)
- `rootMotionY` (sub-05。同じ v3 だが別サブ)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Animation\AnimatorController.h:95-101` `ControllerTransition` に `int32_t canInterrupt = 0;`
- `src\Engine\Engine\Animation\AnimatorController.cpp`
  - `:964` `root["controller"] = 3` + 版コメント (v3: 遷移の `canInterrupt`。sub-05 が `rootMotionY` を追記する)
  - `:1058-1064` `ToJson` の遷移に `canInterrupt`、`:1163-1186` `FromJson` に `t.value("canInterrupt", 0)`
  - `:1739-1770` `Update` の 1.: 遷移中 (`transitionTo >= 0`) の走査を足す。`canInterrupt == 0` だけのアセットでは走査に入らない (アセット側に `anyInterruptible` を持たせるか、走査の先頭で `canInterrupt` を見て `continue`。前者なら `FromJson` で立てる)
  - 確定の処理は 5. (`:1849-1860`) と同じ代入なので関数に切り出して共有 (「遷移先へ確定」と「元へ確定 = 遷移欄を消すだけ」)。5. の本体は変えない
  - `:1600-1632` `AnimatorPlay`: `durationTicks > 0` かつ `transitionTo >= 0` なら先に確定
- `src\Editor\Windows\Animation\AnimatorControllerWindow.cpp:523-526` `hasExitTime` のチェックの隣に `canInterrupt`
- `src\Engine\Core\Localization\LocalizationTable.inl` の `// ---- M95 ----` 直下に `Anim_CanInterrupt` (英: "canInterrupt"、日: "割り込み可")
- `src\Engine\Engine\Animation\AnimatorControllerSelfTest.cpp` の `// ---- M95 sub-04 ----` 直下に 17〜19。既存の v1 / v2 読みテスト (`EditorMain.cpp` の旧コメント "M94g: 既存の v1 / v2 の読みも含む") が v3 書き戻しでも通ること

## 受け入れ条件 (このサブ)

spec §5 の 17, 18, 19, 20。
- 17: 遷移 A→B (duration 8) の tick 2 (未満 → A へ確定) と tick 6 (以上 → B へ確定) で `canInterrupt` の遷移 Any→C を採用し、確定後の `currentState` / `stateTimeTicks` / `statePhase` / 新遷移の欄を断言。`hasExitTime = 1` + `canInterrupt` は採用されない。`from = B` (= `transitionTo`) の遷移が採用される。元へ確定したとき `to == 元` なら採用しない
- 18: 既存の遷移テストが全て PASS (遷移中に `canInterrupt = 0` の遷移が採用されない断言を 1 つ足す)。v2 JSON (`canInterrupt` 無し) → 0、v3 で書いて再読
- 19: `AnimatorPlay(C, 4)` を A→B の tick 2 / tick 6 に呼んだときの確定が 17 と同じ
- 20: 目視 (test_checklists の M95 節へ、sub-07)

## 検証コマンド

- ビルド Debug / Release (worktree)
- 往復中: `$env:MYE_SELFTEST_ONLY='animatorcontroller'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`cmd /c C:\HAL\三校\tools\verify.bat` (三校のシーンは AnimatorController を持つ = 遷移の走査が変わっていないことの確認)

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1) — canInterrupt (controller v3)、SettleTransitionToNearest、Update 1. の else 節、AnimatorPlay の確定、Editor チェック、Anim_CanInterrupt、selftest 17〜19 を実装。Debug ビルド + --selftest (110 suites, 0 failed) PASS。Release / full / check_rules / replay_verify / 三校は未実施。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-04)。コードは仕様どおり (ToJson / FromJson / SettleTransitionToNearest / Update 1. の else 節 / AnimatorPlay / 窓のチェック / Anim_CanInterrupt / テスト 17〜19 を diff で確認)。must は 1 件だけ: 最終検証 (Release ビルド、--selftest-full Debug / Release、check_rules、replay_verify、三校 verify.bat) を回して round 2 で結果を貼る。「走査内 continue」方式は仕様へ採用 (spec §4.1 C を緩和)
- round 2: VERDICT OK (planner、2026-11-04)。コード変更なし。--selftest-full Debug 814 s / Release 108 s とも 110 本 0 failed、check_rules 0 error、replay_verify 15 シーン + 全ジョブ PASS。三校 verify の 2 件 FAIL は三校 DLL の API v30 不一致 (基点以前からの別件、司会が master で作り直して再実行) = 採点外。nit #2 (元のまま確定でも stateEntered=0) は sub-07 の文書へ申し送り
