# sub-05: M66e: Branches — 一覧 / 作成 / checkout + 段階の事前判定

- 依存: sub-04
- 状態: OK (commit c3be569)
- 往復: 1

## やること

spec §4.1 (op: branches / branch_create / checkout / diff_names、S7 = a) / §5 の 2 (branch / checkout / diff_names), 10。

1. **Rust ops**: `branches` (`git for-each-ref refs/heads refs/remotes --format=%(refname:short)%00%(objectname)%00%(upstream:short)%00%(HEAD) -z` 相当 → `{current, locals[], remotes[]}`) /
   `branch_create{name, from}` (`git branch <name> <from>`、既定 `from = HEAD`) / `checkout{name}` (`git checkout <name>`、リモート追跡だけなら `-t`) /
   `diff_names{from, to}` (`git diff --name-status -z <from> <to>` → `[{status: M|A|D|R, path, oldPath?}]`)。
   `checkout` の stderr `would be overwritten` → `error.code=local_changes_overwritten` + `paths[]` (行の抽出)。
2. **Branches タブ**: ローカル / リモートの一覧、現在ブランチの強調 (`themeColor::Accent`)、「作成」(名前入力、既定 from = 現在) 、「切替」。
3. **切替はトランザクション経由**: 押下 → `diff_names(HEAD, target)` で段階を**先に**判定 → 確認ダイアログに「A: その場 / B: シーンを開き直します / C: 再起動します」と対象件数 → OK で `checkout` → 実際の変更集合で再分類 → 後処理 (sub-04)。
   `local_changes_overwritten` → モーダルに一覧 + 「対象を破棄してから再実行」。
4. **起動時の残骸検査 (決定 13)**: `repo_check` の `mergeInProgress` / `rebaseInProgress` → トースト WARN + Changes タブ上部の帯 (解決 UI は sub-07)。
5. **collab_verify**: `tests\collab\03_branch.ndjson` = branch_create → 変更 + commit → checkout main → diff_names(main, feature) の期待 / ローカル変更が重なる checkout → `local_changes_overwritten` の期待。

## やらないこと (このサブでは)

- リモートブランチの fetch (sub-06)。ブランチ削除・リネーム (v1 外)。

## 触る場所 (planner の見立て)

- 変更: `ops.rs`、`SourceControlWindow.cpp` (Branches タブ)、`GitTransaction.cpp` (事前判定の分岐)、`StageClassifier` (diff_names の入力型)、`LocalizationTable.inl`、`tests\collab\03_*.ndjson`、`tools\collab_verify.ps1`。

## 受け入れ条件 (このサブ)

1. `cargo test` に `diff_names` 解析 (R のトークン順) と `would be overwritten` の抽出。
2. `collab_verify.bat` 緑 (03 を含む)。
3. 実機 (fixture、3+1 種): (A) テクスチャだけ違うブランチへ切替 → 再起動なしで絵が変わる / (B) アクティブシーンが違う → 開き直し / (C) `assets\schemas\` が違う → 再起動案内 → 再起動後に切り替わっている / ローカル変更と重なる → 一覧が出て何も変わらない。
4. 実機: `git merge` を外で途中放置して起動 → WARN トースト。
5. `--selftest` / `check_rules.ps1` / `replay_verify.bat` 緑。

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

実装:
- `tools/collab/src/ops.rs` — `branches` / `branch_create` / `checkout` を dispatch に追加。`checkout` は
  **応答の中で `before..after` の差分まで返す** (`names`)。`branch_name_arg` は `-` 始まり / 空白 / `..` を弾く
  (checkout に `--` は置けない = `git checkout -- x` は別操作)。ローカルに無くリモート追跡だけある名前は `-t`。
  `local_changes_overwritten` は `paths[]` + **自前の固定 detail** (git の案内文は版で変わる)。
- `tools/collab/src/porcelain.rs` — `parse_for_each_ref` (for-each-ref に `-z` は無い = LF レコード + `%00` フィールド) /
  `parse_overwritten_paths` (見出し行 → タブ字下げの塊) / `parse_z_paths`。
- `src/Editor/SourceControl/SourceControlState.*` — `BranchList` / `BuildBranchList` / `ChangesFromNames` (どちらも純関数) /
  `RequestBranches` / `CreateBranch` / `Checkout` / `RequestDiffNames` / `TakeMergeWarning`。
- `src/Editor/SourceControl/GitTransaction.*` — `OpKind{Revert,Checkout}` と `Phase::Predict` を追加。
  checkout は「押す → `diff_names(HEAD,target)` で予測 → 確認モーダル (件数 + 段階 A/B/C) → checkout →
  応答の `names` で再分類 → 後処理」。`ResolveMetaGuidChanges` が実行前後の guid を比較 (予測に無い `.meta` は C へ倒す)。
  モーダルの位置を `ImGuiCond_Always` にして中央固定 (sub-04 の nit)。
- `src/Editor/Windows/SourceControlWindow.*` — Branches タブ (一覧 / 現在ブランチを Accent / 切替 / 作成モーダル)、
  差分を別 dockable 窓「Diff」へ移動 (選択で開き、`SetNextWindowFocus` で手前に出す)。
- `src/Editor/EditorApp.*` — 既定ドックで Source Control を左列へ / Diff を下段右へ (既定は閉) /
  束の既定タブを `SelectedTabId` で明示 / [Rebuild Scripts] を `StartGameLogicBuild` へ寄せてハンドルを保持 (`PollScriptBuild`)。
- `tools/collab_fixture.ps1` — テクスチャを貼った床 + 立方体のシーン + `.mat.json` + 3 つの `.meta` (guid 固定)。
- `tests/collab/06_branch.*` — 一覧 / 作成 / 切替 / 差分名 / ローカル変更衝突 / 不正名の 10 行。

検証 (すべて緑): `cargo test` 50 本 (新規 10) / `collab_verify.bat` 6 シナリオ / `--selftest` Debug+Release /
`check_rules.ps1` / `replay_verify.bat` 10 ジョブ / 実機 4 種 (A: `cache/scm_m66e_texA.png`、
B: `cache/scm_m66e_sceneB.png`、C: `cache/scm_m66e_restartC.png` + 再起動後に `FixtureHealth` が TypeId 50 で登録、
衝突: `cache/scm_m66e_overwrite.png`) / マージ残骸 (`cache/scm_m66e_merge.png`) / 既定レイアウト (`cache/scm_m66e_layout.png`)。

仕様との差分 (詳細は SELF_EVAL):
- [追加] `checkout` の応答に `names` を載せた (C++ から `diff_names` を 2 往復目に投げない)。
- [追加] `CollabClient::Shutdown` の `FreeLibrary` を**やめた** — notify 8.2.0 の内部スレッドが join されず、
  checkout 直後に終了すると 0xC0000005 で落ちた (実測 1〜2/6 → 0/12)。spec §4.0 の「destroy -> FreeLibrary」に抵触。
- [追加] `AssetOps::RebuildGameLogic` (fire-and-forget) を削除して `StartGameLogicBuild` 1 本に寄せた。
- [追加] `for-each-ref` の `-z` はサブ仕様の想定と違い**存在しない**。
- [追加] 予測時の `activeSceneRel_` 更新 (sub-04 は BeginOp でしか更新せず、初回の予測が必ず A に倒れていた)。

## フィードバック履歴

## planner 追記 (sub-03 round 1 の判定から。coder は着手前に読む)

- [should] **既定ドックを左列 (Hierarchy 束) のタブへ**移す。下段帯 (Assets 束、高さ ≒ 200 px) では Changes 一覧が 2 行で切れ、コミット欄が見えない (`cache/scm_m66c3.png`)。`DockBuilderDockWindow` の行き先 1 か所 + layouts のパネル表。**`###` の右辺は 1 バイト一致**させること (sub-02 申し送り)。
- [should] **差分は別の dockable 窓「Diff」** (読み取り専用、選択時に開く / 閉じられる) にし、Changes タブの inline ペインを外す。元計画 M66c の「読み取り専用の子窓」に戻す形。`###Diff` の ID を LocalizationTable に足す (規則 10)。
- 上 2 点は「ユーザーの手触り」で再調整しうるので、実装後に既定レイアウトのスクショを 1 枚 `cache/` に残し、SELF_EVAL にパスを書く。
- Branches タブの一覧も同じ高さ制約を受けるので、左列前提で組む。

## planner 追記 (sub-04 round 1 の判定から。coder は着手前に読む)

- [should] **ゲートの穴を塞ぐ**: Asset Browser の [Rebuild Scripts] = `AssetOps::RebuildGameLogic` (`AssetOps.cpp:1427-1447`) は `ShellExecuteW` の fire-and-forget でハンドルを持たず、その間 `GateBlocker::ScriptBuildRunning` が立たない。checkout は `src/GameLogic/Scripts/` を丸ごと入れ替えるので、ここで初めて実害になる。直し方: `StartGameLogicBuild` (ハンドルを返す既存の口) に寄せ、`EditorApp` が HANDLE を保持して毎フレーム `PollProcess`、`GateInputs.scriptBuildRunning` に OR で流す。DllReloader の監視は従来どおり (ビルド出力の場所は変えない)。
- [should] **fixture にテクスチャを参照するエンティティを 1 個置く** (`tools/collab_fixture.ps1`)。sub-04 の受け入れ条件 3 後半 (テクスチャだけ変えて破棄 → 絵が戻る) と本サブの受け入れ条件 3 (A: テクスチャだけ違うブランチへ切替 → 絵が変わる) は、今の fixture (エンティティ 0 件) では `HandleChange` が no-op で画素証拠が撮れない。最小: 床の平面 1 枚 + `test.png` を貼る `.mat.json`。既存の 5 シナリオの期待 NDJSON が変わる (entries が増える) ので `--update` 後に**全行を読んで**採用する。
- 段階 C のモーダルは sub-04 の差し戻しで『再起動』のみになる。checkout の事前予測が C のときは、実行**前**の確認ダイアログで「再起動します。続けますか」を取る (実行後は取り返しがつかないので、ユーザーが止められるのは実行前だけ)。
- 書き込み系 op を `GitTransaction` に足すときは coder 申し送りどおり「変更集合の決め方」だけ差し替える (`BeginOp` / `ApplyResult` / `BuildChangeSet` は op 非依存)。checkout は事前 `diff_names(HEAD, target)`、事後 `diff_names(before, after)`。
- [nit、sub-04 round 2 から] 段階 C モーダルが画面中央でなく上寄り (y ≒ 137px) に出る (`cache/scm_m66d_restart.png`)。破棄の確認モーダルが open stack に残ったまま次の OpenPopup をしている疑い (未確認)。Branches / Diff のモーダルを足すときに「Applying を 1 フレーム描いて CloseCurrentPopup を明示」の形で一緒に見る。実害なし (modal として入力は止まっている)。
- `GitTransaction::Hooks::relaunch` は **bool を返す契約** (sub-04 round 2)。checkout / pull で段階 C を通すときも失敗を握り潰さない (握り潰すとモーダルが閉じて「あとで」と同じ状態になる)。
- round 1: VERDICT OK (planner、2026-09-03)。裏取り: notify-8.2.0/src/windows.rs:590-596 の Drop は `tx.send(Stop)` + wakeup のみで join 無し (coder の主張を一次情報で確認) / CollabClient.cpp:276-282 に撤去理由のコメント / 旧 RebuildGameLogic は `cmd /c build_scripts.bat` で bat 側が失敗時 `pause` = 可視窓でエラーが読めていた (質問 2 の判断材料 → 同等 UX を sub-08 の should に) / ops.rs:672 checkout 応答に names、:518 diff_names / EditorApp.cpp:1388 左列ドック、:1408-1411 SelectedTabId、:712,1306 PollScriptBuild / golden*.rep 10:46 再生成 (replay 最終状態) / cache/scm_m66e_layout.png (左列に Source Control タブ、床 + 立方体の fixture) と scm_m66e_overwrite.png (中央のモーダル、error.code + 固定文 + paths) を目視。質問 1〜5 はすべて coder の判断を採用し spec §4.0 / §4.1 に確定。
