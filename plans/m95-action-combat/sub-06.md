# sub-06: イベント時刻の秒・フレーム指定

- 依存: sub-00 (Wave 1。sub-01 / sub-02 / sub-04 と並列)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 最初と最後に両方で `git status` を確認し、SELF_EVAL に貼る
- `AnimatorControllerSelfTest.cpp` への追記は `// ---- M95 sub-06 ----` の直下
- `AnimatorController.cpp` で触ってよいのは `FromJson` の **`clipEvents` ブロック** (`:1188-1221`) と `ToJson` の `clipEvents` のコメントだけ。遷移 (sub-04)、ステートの `skel` (sub-05)、`Update` (sub-01 / 04 / 05)、`root["controller"]` の行 (sub-04) は触らない
- 同じ波の他サブのファイルを触らない

## やること

spec §4.1 F。`clipEvents` の読み込みで `time` (秒) / `frame` + クリップ単位 `fps` を受けて tick に丸める (1 箇所)。
保存は tick 配列のまま。イベント帯の tick スライダに秒を併記する。

## やらないこと (このサブでは)

- 内部表現の変更 (tick のまま)、保存形式の変更、イベント単位の `fps`
- 発火規則 (`ClipEventPassDistance`) の変更

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Animation\AnimatorController.cpp:1188-1221` `FromJson` の `clipEvents`: `list` が配列ならそのまま、オブジェクトなら `fps` (正の数、既定 60、不正は `MYE_LOG_WARN` して 60) と `events` 配列を取る。各要素で `tick` → `time` → `frame` の順に解決する換算関数を 1 つ置く (`std::llround`、double)
- `:1007-1046` `ToJson` は変更なし (tick 配列)。コメントに「`time` / `frame` は読みのみ」を足す
- `src\Editor\Windows\Animation\AnimationPreviewWindow.cpp:729` `AnimPrev_EvTick` のスライダの右に `snprintf` で `= %.3f s` を作って `ImGui::TextUnformatted` (`Tr()` を書式に渡さない)。新しい `StrId` は不要 (単位だけなら翻訳不要。翻訳するなら `LocalizationTable.inl` の `// ---- M95 ----` 直下に `AnimPrev_EvSeconds`、両言語)
- `src\Engine\Engine\Animation\AnimatorControllerSelfTest.cpp` の `// ---- M95 sub-06 ----` 直下に 26 / 27
- `engine_spec.md` / `test_checklists.md` は sub-07

## 受け入れ条件 (このサブ)

spec §5 の 26, 27, 28。
- 26: 表の 7 ケース (`tick` 優先、`time 0.5 → 30`、`time 0.0083 → 0`、`frame 7 @ 30 → 14`、`frame 1 @ 24 → 3`、`fps` 無し → 60 扱い、`fps 0` → 警告して 60) と配列形の不変
- 27: `time` / `frame` で読んだアセットを `ToJson` → `FromJson` して同じ tick。既存 v2 ファイル (tick 配列) の round-trip が不変
- 28: 目視 (sub-07 の checklist)

## 検証コマンド

- ビルド Debug / Release (worktree)
- 往復中: `$env:MYE_SELFTEST_ONLY='animatorcontroller'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat` (読み込みの既存経路が不変であることの確認)

## 実装メモ (coder が追記)

## フィードバック履歴
