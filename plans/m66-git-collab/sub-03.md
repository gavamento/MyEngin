# sub-03: M66c: stage / unstage / commit / History / diff / identity

- 依存: sub-02
- 状態: OK (commit 83d0b4f)
- 往復: 1

## やること

spec §4.1 (op: stage / unstage / commit / log / diff / identity_check) / 決定 7 / §5 の 2 (commit / log), 4(c), 8。

1. **Rust ops**: `stage{paths}` (`git add -- <paths>`、削除済みは `git add -u -- <path>` でなく `git rm --cached`? → **`git add -A -- <paths>` で統一**) /
   `unstage{paths}` (`git restore --staged -- <paths>`、2.23+; 古い git は `reset -q HEAD --`) / `commit{message}` (`git commit -F -` で本文を stdin から、`nothing_to_commit` の分類) /
   `log{n}` (`git log -n <n> --format=%H%x00%an%x00%aI%x00%s -z`、上限 200) / `diff{path, staged}` (`git diff [--cached] -- <path>` のテキスト、バイナリは git の 1 行そのまま) /
   `identity_check` (`git config user.name` / `user.email` が両方非空か → `{ok, name, email}`)。
2. **対の規則 (決定 7、S13 で確定)**: C++ `PairRule::Collect(primaryPaths, exists)` → `{toStage[], toEnsureMeta[]}`。本体 + `.meta` + `.terrain.edit` を束ね、
   `.meta` が無い資産 (`AssetDatabase::ClassifyPath` が資産と判定するもの) は `AssetDatabase::EnsureMeta` を呼んでから対で stage。純関数部分をセルフテストで叩く。
3. **Changes タブ**: 選択 → Stage / Unstage / 差分表示。コミット欄 (複数行) + 「コミット」。`IsSceneDirty()` なら欄の上に「未保存の変更は含まれません」+ 「保存してコミット」(保存 → commit)。
   `identity_check` NG なら「`git config --global user.name / user.email` を設定してください」の案内 (設定 UI は作らない)。
   **commit は working tree を書かないので GitTransaction の対象外** (ゲート不要)。
4. **History タブ**: `log{n=100}` の一覧 (短 SHA / author / 日時 / subject)、選択で subject 全文。
5. **差分の子窓**: `git diff` のテキストを読み取り専用で (`InputTextMultiline` の `ReadOnly` か `TextUnformatted` の child)。シーン JSON の意味付けはしない (v1.5)。
6. **collab_verify**: `tests\collab\02_commit.ndjson` = 変更 → stage (対) → status (index=M) → commit → log (n=2) → `<sha>` 正規化で期待一致。`identity_check` の NG ケース (隔離 config で name を消して) も 1 本。

## やらないこと (このサブでは)

- revert (sub-04、書き込み系トランザクションが要る)。push (sub-06)。

## 触る場所 (planner の見立て)

- 変更: `tools\collab\src\ops.rs`、`SourceControlWindow.cpp` (Changes / History)、`SourceControlState.*` (index/worktree の別を保持)、
  新規 `src\Editor\SourceControl\PairRule.h/.cpp`、`SourceControlSelfTest.cpp`、`LocalizationTable.inl`、`tests\collab\02_*.ndjson`、`tools\collab_verify.ps1` (シナリオ追加)。
- 参照: `src\Engine\Engine\AssetDatabase.{h,cpp}` (`EnsureMeta` `:226-238`、`ClassifyPath`)、`src\Editor\AssetOps.cpp:199-206` (登録の前例)。

## 受け入れ条件 (このサブ)

1. `cargo test` に log / diff の解析テストが増えて緑。
2. `collab_verify.bat` 緑 (01 + 02 + identity NG)。
3. `--selftest` 緑 (c: `.meta` 欠落 → `toEnsureMeta` に載る / `.terrain.edit` 同居 → `toStage` に載る / `.meta` 単独選択 → 本体も束ねる)。
4. 実機 (fixture): 変更 → stage → commit → History に載る & `git log` で確認。dirty 時に「保存してコミット」で 1 回の操作で両方通る。identity を消すと案内が出る。
5. `check_rules.ps1` / `replay_verify.bat` 緑。

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

### round 1 (SELF_EVAL の写し)

**実装**
- `tools/collab/src/git.rs`: `run` を `build`(共通引数+env) + `output` に分解し、`run_with_stdin`
  (`git commit -F -` 用) を追加。`classify_error` に `identity_missing` の 3 文言を追加。
- `tools/collab/src/ops.rs`: `stage` / `unstage` / `commit` / `log` / `diff` / `identity_check` を
  dispatch に追加。`arg_paths` (空配列・`..`・先頭 `-` を bad_request)、`run_per_path_chunk`
  (64 本ずつ = Windows のコマンドライン長対策)、`status_after_write` (応答に status を載せ
  `last_status`/`last_head` も更新 = 自分の書き込みで通知が二重に出ないようにする)。
- `tools/collab/src/porcelain.rs`: `parse_log_z` / `clip_text` を純関数として追加 (cargo test 対象)。
- `src/Editor/SourceControl/PairRule.{h,cpp}` (新規): `pairrule::{PairPlan, SidecarCandidates,
  IsAssetPath, Collect, ListedPaths}`。`PrimaryPathFor` の逆写像。`exists` を注入して純関数化。
- `src/Editor/SourceControl/SourceControlState.{h,cpp}`: `CommitInfo` / `DiffView` /
  `SourceControlModel::FindEntry` / `StageRows` / `UnstageRows` / `StageSavedPath` / `Commit` /
  `RequestLog` / `RequestDiff` / `RequestIdentity` / `TakeLastCommit` / `WriteInFlight` /
  `ApplyWriteResult` / `AbsolutePathOf`。repo_check 成功時に `RequestIdentity` も投げる。
- `src/Editor/Windows/SourceControlWindow.{h,cpp}`: Changes タブ (Stage/Unstage/選択件数/一覧/
  差分ペイン/コミット欄/保存してコミット)、History タブ、`SourceControlHost` (dirty + 保存手段)。
- `src/Editor/EditorApp.cpp`: 窓の既定表示 (`open = !projectRoot.empty()`)、`SourceControlHost` の
  組み立て、commit 成功トースト。
- `src/Engine/Core/LocalizationTable.inl`: Scm_* を 16 本追加 (en/ja)。
- `tools/collab_verify.ps1`: `# git <args>` ディレクティブ (fixture の前提条件を作る口) と、
  diff の `index <blob>..<blob>` 正規化。ディレクティブ判定を `^#\s(verb)\s` の厳密一致に。
- `tests/collab/03_commit.ndjson` / `04_identity.ndjson` (+ expected)。

**この実装で確定させた解釈** (詳細は SELF_EVAL「仕様との差分」)
- unstage は `git restore --staged` ではなく **`git reset -q -- <paths>`** (未出生ブランチで
  restore が "could not resolve HEAD" になるのを実測)。
- 「保存してコミット」は **保存 → 保存した文書を対の規則で stage → commit** の 3 手。
- identity NG は**案内のみ**でボタンは塞がない (spec の「案内」に忠実)。
- シナリオ番号は 02 が埋まっていたので `03_commit` / `04_identity`。

**検証**: cargo test 32 / collab_verify 4 本 / --selftest 両構成 exit 0 / check_rules 0 /
replay_verify 10 ジョブ / 実機 fixture のスクショ 3 枚 + 一時プローブで stage→commit→log を
実走 (`git show --stat` で `fresh.png` と `fresh.png.meta` が同じコミットに入ることを確認)。


## フィードバック履歴

## planner 追記 (sub-02 round 1 の判定から。coder は着手前に読む)

- **対の規則の訂正**: `.terrain.edit` は `x.terrain.json` の `.json` を `.edit` に差し替えた名前 (`TerrainEdit.cpp:469 EditPathFor`)。上の「本体 + `.meta` + `.terrain.edit`」は「`x.terrain.json` + `x.terrain.json.meta` (あれば) + `x.terrain.edit`」と読むこと。sub-02 の `PrimaryPathFor` が既にこの形で束ねているので、`PairRule::Collect` は同じ判定を再利用する (二重実装しない)。
- [should] Source Control 窓の既定表示: プロジェクト起動では `open = true` (Assets と同束のタブ)、裸起動では false (spec §4.3)。`EditorApp` の窓登録 1 か所。
- `.meta` 無し資産の判定に使う `AssetDatabase::ClassifyPath` が `.terrain.edit` を資産扱いするかは未確認 — 資産扱いなら `.terrain.edit` 自身にも `.meta` が付くので、束ねは「存在するサイドカーだけ」を集める実装で依存しないこと (spec §7)。
- `SourceControlSession::Busy()` / `Client()` / `HintChanged()` は sub-02 が用意した未使用の口。stage / commit はこの Session 経由で呼ぶ。
- round 1: VERDICT OK (planner、2026-09-03)。裏取り: ops.rs:291 `add -A` / :302 `reset -q` / :222 `status_after_write` が last_head を更新 / MAX_DIFF_BYTES 256 KB / PairRule.cpp:65-86 が SidecarCandidates + EnsureMeta 後に .meta を stage 集合へ / SourceControlState.cpp:543-547 で EnsureMeta を git 呼び出し前に実行 / SourceControlWindow.cpp:477-484 保存してコミットが StageSavedPath を経由 / EditorApp.cpp:223 で既定表示 = projectRoot 非空 / cache の golden*.rep が 06:56 に再生成 (replay_verify 最終状態で実行) / cache/scm_m66c3.png と c4.png で既定ドックの窮屈さを確認 (質問 5 は実害あり → 左列 + Diff 別窓に裁定、sub-05)。質問 1 承認 / 2 = ボタン無効に裁定 (sub-04) / 3 = 仕様の文言を訂正 / 4 承認 / 6 = ヘッダ不要で確定。
