# sub-05: M66e: Branches — 一覧 / 作成 / checkout + 段階の事前判定

- 依存: sub-04
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴

## planner 追記 (sub-03 round 1 の判定から。coder は着手前に読む)

- [should] **既定ドックを左列 (Hierarchy 束) のタブへ**移す。下段帯 (Assets 束、高さ ≒ 200 px) では Changes 一覧が 2 行で切れ、コミット欄が見えない (`cache/scm_m66c3.png`)。`DockBuilderDockWindow` の行き先 1 か所 + layouts のパネル表。**`###` の右辺は 1 バイト一致**させること (sub-02 申し送り)。
- [should] **差分は別の dockable 窓「Diff」** (読み取り専用、選択時に開く / 閉じられる) にし、Changes タブの inline ペインを外す。元計画 M66c の「読み取り専用の子窓」に戻す形。`###Diff` の ID を LocalizationTable に足す (規則 10)。
- 上 2 点は「ユーザーの手触り」で再調整しうるので、実装後に既定レイアウトのスクショを 1 枚 `cache/` に残し、SELF_EVAL にパスを書く。
- Branches タブの一覧も同じ高さ制約を受けるので、左列前提で組む。
