# sub-07: M66g: 競合 — abort / ours / theirs / continue

- 依存: sub-06
- 状態: OK (commit 3c0bf56)
- 往復: 1

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

SELF_EVAL: sub-07 (round 1)

実装:
  - `tools/collab/src/porcelain.rs:parse_unmerged` / `UnmergedEntry::kind` — `u` レコードから
    パス・XY・**モード (m2/m3)** を読む。`ours` / `theirs` を採れるかは XY の文字ではなく
    モードで決める (`AU` は Y='U' なのに相手の版が無い = 文字で振ると checkout --theirs が落ちる)。
  - `ops.rs`: `conflicts` (未マージ + 「競合せずにマージ済み」= `diff --name-status HEAD` から
    未マージを引いたもの) / `resolve{paths[],side}` (版がある側は `checkout --ours|--theirs` + `add`、
    無い側は `git rm`) / `merge_abort` (`merge --abort` か `rebase --abort`。names は**実行前後の
    ディスクの在り方**から A/D/M) / `continue` (未解決なら `merge_in_progress` + 残り、
    全件解決なら `commit --no-edit` → names は pull 前 HEAD..マージコミット)。
    `pull` の競合エラーに `paths[]` (未マージ一覧) を追加。
  - `ops.rs:merge_state` / `git_dir` — status と repo_check の両方に `mergeInProgress` /
    `rebaseInProgress` を載せる。`.git` の場所は 1 回だけ聞いて覚える。
  - `SourceControlState.h/.cpp`: `ConflictFile` / `ConflictRow` / `ConflictList` と純関数
    `BuildConflictList` / `BuildConflictRows` (対の束ね) / `ConflictMatchesPath` (保存ガードの判定)。
    セッションに `RequestConflicts` (飛んでいる要求に相乗り) / `Resolve` / `MergeAbort` /
    `MergeContinue` / `IsConflictedPath` / `ConflictChangeSet`。`ApplyStatusResult` が
    マージ中を見て競合一覧を自動で取り直す。`Checkout` / `Pull` の重複を `SendTreeOp` に統合。
  - `GitTransaction`: `OpKind::MergeAbort` / `MergeContinue` と `Phase::ConflictScan`
    (競合した pull は **EndBatch({}) せず** conflicts を 1 往復聞いて「マージ済みだけ」を適用してから
    競合を報告する)。`BlockersForConflictOps` = MergeInProgress を除いたゲート。
  - `SourceControlWindow::DrawConflicts` — 競合中は Changes タブが競合一覧に切り替わる
    (タブもボタンも増やさない)。行ごとに ours / theirs、下に「中止 / 完了 / 外部ツール」。
  - `EditorApp`: `SaveCurrentScene` の先頭に競合ガード 1 箇所 / `openMergeTool`
    (`cmd /k ... git mergetool`) / `requestMergeAbort` / `requestMergeContinue`。
  - `LocalizationTable.inl` に 33 件 (競合モード / ours・theirs / 中止・完了のモーダル / 種別 8 種)。
  - 検証: `tests/collab/08_conflict.ndjson` (対の resolve + modify/delete の theirs + continue) と
    `09_merge_abort.ndjson`、cargo test 3 本、セルフテスト (j) 競合モデルの検査 14 件。

仕様との差分:
  - [逸脱] `resolve` の引数は `{path, side}` ではなく **`{paths[], side}`**。本体と `.meta` を
    1 回で解決しないと「本体は theirs、`.meta` は競合のまま」が 1 往復ぶん存在する。
    `stage` / `revert` と同じ `arg_paths` を通せる利点もある。
  - [追加] `status` の応答に `mergeInProgress` / `rebaseInProgress` を追加 (フィールド追加 =
    PROTO bump 対象外)。これが無いと `MergeInProgress` は repo_check (起動時 1 回) でしか
    更新されず、**pull が競合した直後のゲートが開いたまま**になる (決定 9 が成立しない)。
    既存の期待 NDJSON 7 本を撮り直した (差分がこの 2 キーだけであることを機械照合済み)。
  - [追加] 競合の種別は sub の 4 種に加えて `both_deleted` / `added_by_us` / `added_by_them` /
    `unmerged` を返す (git-status.txt の表がその 7 組を定義している。落とすと表示が嘘になる)。
  - [追加] `conflicts` の応答に `merged[]` (競合せずにマージ済み) と `ours` / `theirs`。
    sub は「pull の応答に mergedPaths」と書いていたが、ErrorBody を膨らませるより
    `conflicts` op に持たせる方が「段階分類の入力」と同じ `{path,status}` の形にできる。
  - [追加] `merge_abort` / `continue` の応答にも `remote` (pull と同型を保つ)。
  - [未実装] `continue` は **merge のみ**。外で始まった rebase の続行は `bad_request` で返す
    (エディタ経由の `rebase --continue` は GIT_EDITOR が要る)。中止は rebase も受ける。

検証:
  - `cd tools\collab && cargo test` → 24 + 34 + 6 = 緑 (新規 6 本: `u` の種別とモード 3 本、
    競合一覧 / resolve+continue / abort の実 git 3 本)
  - `toolsuild_collab.bat Debug` → 成功 (linker_messages の warning 1 件は既知・無害)
  - `tools\collab_verify.bat` → 9 シナリオ全 PASS (08 / 09 が新規)
  - `cmd /c bind\Debug\Editor.exe --selftest` → exit 0 / FAIL 0。Release も exit 0
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
  - `tools
eplay_verify.bat` → exit 0 (10 ジョブ、**無変更緑**)
  - 実機 (`cache\m66g_fx` + bare origin + peer クローン、シーンの同じ行を両側で変更):
    一時プローブ (検証後に削除済み) で
    `pull → non_fast_forward → マージして取り込む → conflict → conflicts → EndBatch(マージ済み)`
    → ゲートが `MergeInProgress` 1 件で閉じる → **保存が止まる**
    (`[collab] save blocked: the open document is conflicting`)
    → `resolve theirs` → `continue` → `[collab] continue applied: 1 path(s), stage B` +
    `scene loaded: ...main.scene.json (4 entities)` = **シーンが開き直り、相手の "Peer Cube" が入った**。
    別走で `merge_abort` → `stage B` で開き直し → ahead 1 / behind 1 / 変更 0 = pull 前に戻った。
    絵: `cache\scm_m66g_ui.png` (競合モードの一覧。Finish だけ無効) /
        `cache\scm_m66g_abort.png` (中止後。帯が通常表示に戻り、自分の版の立方体が出ている)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜4 を全部実測。逸脱 1 件 (`resolve` の引数) と追加 5 件を上に明示。
  正しさ: 4 — git 側の挙動 (u のモード / checkout --theirs が落ちる / abort は HEAD を動かさない /
    commit --no-edit) は実機で先に観測してから実装した。UI のクリックだけは一時プローブ代替。
  コード品質: 4 — 競合の後処理は既存の `TreeOpResult` / `ApplyResult` を共有し、
    checkout / pull の重複も `SendTreeOp` に畳んだ。ImGui 側は既存の作法どおり。
  テスト: 4 — 純関数 (u の解析 / 束ね / 保存ガード / ゲート) と実 git の往復
    (cargo test 3 本 + collab_verify 2 本) の両方。UI の描画そのものはスクショ 2 枚のみ。

不安・質問:
  - マージ中に `pull` を投げると `git_failed` + git の生文 ("Pulling is not possible because you
    have unmerged files.") になる。UI では Pull がゲートで閉じているので到達しないが、
    `merge_in_progress` へ分類する方が親切ではあるか (1 行。sub-10 で拾うなら申し送りに残す)。
  - `Scm_ComingSoon` は競合 UI でも使わなかった (競合中の文言は全部新規)。sub-10 で削除する想定。

触ったファイル:
  - `tools/collab/src/porcelain.rs` / `tools/collab/src/ops.rs`
  - `tools/collab/tests/porcelain.rs` / `tools/collab/tests/service.rs`
  - `tests/collab/08_conflict.ndjson` (+ `.expected.ndjson`) / `tests/collab/09_merge_abort.ndjson` (+ `.expected.ndjson`)
  - `tests/collab/0{1..7}_*.expected.ndjson` (status の 2 キー追加ぶんの撮り直し)
  - `src/Editor/SourceControl/CollabProtocol.h` / `SourceControlState.h` / `SourceControlState.cpp`
  - `src/Editor/SourceControl/GitTransaction.h` / `GitTransaction.cpp` / `SourceControlSelfTest.cpp`
  - `src/Editor/Windows/SourceControlWindow.h` / `SourceControlWindow.cpp`
  - `src/Editor/EditorApp.cpp` / `src/Engine/Core/LocalizationTable.inl`
  - `plans/m66-git-collab/sub-07.md` (この節)

申し送り:
  - **競合の入口は 2 つある**: pull の応答 (GitTransaction の ConflictScan) と、
    起動時に外の git で競合していた場合 (status の `mergeInProgress` → `ApplyStatusResult` が
    自動で `conflicts` を投げる)。後者は実機で通した (`[collab] the repository is in the middle of...`)。
  - 競合したシーンは **JSON として壊れている**ので起動時に読めず、エディタは空シーンで開く
    (実測: `0 entities`)。保存ガードが無いと Ctrl+S で空シーンを競合ファイルへ上書きできてしまう
    — ガードは `SaveCurrentScene` の先頭 1 箇所 (`IsConflictedPath`) が唯一の砦。
  - `resolve` はトランザクションを通さない (競合ファイルだけを書き換えるので、監視 →
    ReloadHub の通常経路で拾える)。実機でも `[reload] scene diff applied` が出て、
    解決した内容がその場で反映された。
  - `conflicts` の `merged[]` は `diff --name-status HEAD` 由来なので、**pull 前からあった
    未コミット変更も混ざる**。混ざった分は「変わっていないファイルをもう一度読む」だけで実害は無いが、
    アクティブシーンにローカル変更があると余分に開き直る (ゲートが保存済みを保証しているので損は undo 履歴だけ)。
  - collab_verify の次の番号は **10**。期待ファイルにマージコミットの件名を載せないこと
    (`Merge branch 'main' of <URL>` = リモート URL が入る)。
  - 一時プローブ (`MYE_COLLAB_M66G`) は削除済み。同じ検証をやり直すなら
    「GitTransaction の確認ボタンに `|| probeAuto_` を 3 箇所」+「EditorApp に段取りの状態機械」+
    「既定ドックのタブを Source Control にする」の 3 点セット (sub-05 の recipe と同じ)。
  - 生成物: `cache\m66g_fx` (+ `.gitconfig`)、`cache\m66g_origin.git`、`cache\m66g_peer`、
    `cache\scm_m66g_*.png`、`cache\m66g_*.log`、`cache\collab_verify\`。

## フィードバック履歴

## planner 追記 (sub-06 round 1 の判定から。coder は着手前に読む)

- 競合の入口は sub-06 で既にある: `pull{allowMerge:true}` が `error.code=conflict` + 固定文を返し、リポジトリはマージ途中で止まる。**競合ファイル一覧は `conflicts` op (porcelain v2 の `u` 行) から取る。git の案内文 (`CONFLICT (modify/delete)` など別形がある) を解析しない**。
- `continue` / `merge_abort` の応答は pull と同型 `{head, names, status, remote}` にし、`GitTransaction` は `OpKind` を足して `SendPredict` と `BeginOp` の 2 箇所だけ差し替える (`TreeOpResult` / `ApplyResult` を共有)。
- pull の段階 B/C は sub-06 で実機未検証。競合シナリオ (同じシーンを両側で変更 → pull) が B 経路を通るので、そこでシーン開き直しまで観測する。
- 実機 fixture にテキストを置くとエディタが `.meta` を作って未追跡が増え、checkout / pull が `local_changes_overwritten` で弾かれる (coder 申し送り)。競合用の変更はシーン / テクスチャで作る。
- collab_verify の次の番号は 08。bare origin は `init --bare -b main`。
- `Scm_ComingSoon` は未使用。競合 UI で使わないなら sub-10 で消す。
- round 1: VERDICT OK (planner、2026-09-03)。裏取り: `git diff -U0 tests/collab/0[1-7]_*.expected.ndjson` から mergeInProgress / rebaseInProgress を剥がすと差分 0 行 (撮り直しは 2 キーだけ = 規律どおり) / EditorApp.cpp:1256-1257 の保存ガード (actorEdit のパスも対象) / porcelain.rs:364-430 モード (fields[3] != 000000) で has_ours・has_theirs / GitTransaction.cpp:607-617 で競合時は EndBatch({}) せず ConflictScan、失敗経路では必ず EndBatch / golden*.rep 15:31 再生成 (replay 最終状態) / cache/scm_m66g_ui.png を目視 (赤帯「merge in progress」、Changes タブが競合一覧に切替、ours / theirs、Abort / Finish(無効) / Tool)。逸脱 1 + 追加 5 はすべて仕様として採用。質問 2 件は sub-08 の衛生へ。
