# sub-07: M66g: 競合 — abort / ours / theirs / continue

- 依存: sub-06
- 状態: 未着手
- 往復: 0

## やること

spec §4.1 (op: merge_abort / conflicts / resolve / continue) / 決定 9 / §7 (競合中の Save 停止) / §5 の 2 (競合), 12。

1. **Rust ops**: `conflicts` (porcelain v2 の `u` 行 → `[{path, kind: both_modified|deleted_by_us|deleted_by_them|added_by_both}]`) /
   `resolve{path, side: ours|theirs}` (`git checkout --ours|--theirs -- <path>` → `git add -- <path>`; 削除系は `git rm --cached` / `git add` の分岐) /
   `merge_abort` (`git merge --abort`; rebase 中なら `git rebase --abort`) / `continue` (全件解決済みを確認 → `git commit --no-edit`; 未解決があれば `merge_in_progress` + 残りのパス)。
   `pull{allowMerge:true}` が `conflict` を返すとき、`paths[]` に競合ファイルと、`mergedPaths[]` に競合なしでマージ済みのファイルを載せる。
2. **競合トランザクション**: `pull` が `conflict` → `EndBatch(mergedPaths のうち A 段階のもの)` を適用し、**競合ファイルには触れない**。
   アクティブシーンが競合一覧にある間は `SaveCurrentScene` を止めてトースト (保存 = マーカーを潰して黙って ours を選ぶのと同じ)。`MergeInProgress` で他の書き込み系はゲートで無効 (決定 9)。
   `abort` → トランザクション経由。変更集合 = `paths ∪ mergedPaths` (競合時に記録しておく) → 段階分類 → 後処理。
   `resolve` (対で送る) → 一覧が減る。全件解決 → `continue` → トランザクション経由。変更集合 = `diff_names(pull 前 HEAD, HEAD)` → 後処理。
3. **Changes タブの競合モード**: `MergeInProgress` の間は一覧が競合一覧に切り替わり、各行に「ours」「theirs」、下部に「マージを中止」「解決を完了」(全件解決で有効)。
   「外部ツールで解決」= `git mergetool` を `ShellExecuteW(cmd.exe /c start ...)` で起動 (結果は監視で拾う)。
4. **collab_verify**: `tests\collab\05_conflict.ndjson` = 2 クローンが同じファイルを変更 → B が pull (allowMerge) → `conflict` → `conflicts` → `resolve ours` → `continue` → log にマージコミット / `merge_abort` の別本。
5. **セルフテスト**: `u` 行の解析はすでに sub-01 の cargo test。C++ 側は `MergeInProgress` が `ComputeBlockers` に載る 1 ケースと、競合一覧モデル (対の束ね: `.meta` だけ競合) を追加。

## やらないこと (このサブでは)

- fileId 単位の 3-way マージ、競合マーカー入り JSON の可視化 (別計画)。rebase の開始 (v1 は merge のみ)。

## 触る場所 (planner の見立て)

- 変更: `ops.rs`、`porcelain.rs` (`u` の kind)、`GitTransaction.cpp` (競合分岐と記録)、`SourceControlWindow.cpp` (競合モード)、`EditorApp.cpp` (`SaveCurrentScene` のガード 1 箇所)、`SourceControlSelfTest.cpp`、`LocalizationTable.inl`、`tests\collab\05_*.ndjson`。
- 参照: `AssetOps.cpp:1195-1206` (`ShellExecuteW` の流儀)。

## 受け入れ条件 (このサブ)

1. `collab_verify.bat` 緑 (05 の 2 本)。
2. `--selftest` 緑。
3. 実機: 2 クローンで同じシーンを変更 → pull (マージ) → 競合一覧 → 他の書き込み系ボタンが無効 + 理由 `MergeInProgress` → シーン保存がトーストで止まる → theirs → 解決を完了 → シーンが開き直り相手の変更が見える。もう一度作って「中止」→ pull 前に戻る。
4. `cargo test` / `check_rules.ps1` / `replay_verify.bat` 緑。

## 検証コマンド

```
cd tools\collab && cargo test
tools\build_collab.bat Debug
tools\collab_verify.bat
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視)
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

## フィードバック履歴
