# sub-15: ファイルと Hunk の復元、ジャーナル、回復 (M104p)

- 依存: sub-14
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。sub-14 の VERDICT で planner が Phase 5 を見直してから着手する。大きければ「ゲートと競合検証」「適用とジャーナルと回復」の 2 本に割る (spec §7.2)。

この機能で初めて作業ツリーを書くサブ。安全性は P0 (要求 §15 Phase 5)。**復元は one-way door に近い操作なので、テストは必ず一時プロジェクトで行い、リポジトリの作業ツリーで試さない。**

## やること

restore の §1〜§8 のうち、ファイルと Hunk の分。

1. ゲートの共有: `EditorApp::BuildGateInputs` (private) を `RestoreManager` からも使える形にする。`ComputeBlockers` はそのまま。対象ごとの効かせ方は restore §2 の表。追跡に固有の条件 3 つ。理由は**全部**列挙し、実行の直前に判定し直す。
2. 競合の検証: restore §3.1 (ファイル全体。追加・削除・リネームを含む) と §3.2 (Hunk。バイト列で比べる、位置ずれは 1 か所だけ一致のとき、文脈 0 行はずれを認めない、後ろの Hunk から当てる、1 つでも競合ならそのファイルは 1 つも当てない)。
3. 依存の確認 (restore §3.4 のファイルの分)。分からないことは確認の画面に書く。
4. パスの検査 (restore §8): プレビューのときと書く直前の 2 回。`..`、ドライブ名、UNC、予約デバイス名、シンボリックリンク / ジャンクション、`.git` / `.mye` の下。
5. 復元ジャーナル `recovery\<restoreId>\` (restore §5): `pre\` を全部保存し終えてから `applying`。`manifest.json` は `WriteFileReplacing`。
6. 適用 (restore §4.1): `ReloadHub::BeginBatch` 〜 `EndBatch(changes)` で囲む。**エラーで抜ける経路でも必ず `EndBatch({})`**。書き込みは `WriteFileReplacing`。`StageClassifier` の分類。
7. 整合性の検証と巻き戻し (restore §6): 書いたファイルを読み直す。失敗したら `pre\` へ戻す。戻す前に「自分が書いた内容」と一致するか確かめ、違えば `recovery_required`。
8. 起動時の回復: `recovery\` に `applying` / `rolling_back` / `recovery_required` があれば回復の画面 (3 つの内容と選択肢)。**勝手には戻さない。** 回復待ちの間は新しい復元を始められない。
9. サービス側: `restore_record` / `restore_expect` (`role = editor` だけ)、`actor = restore` の判定 (`restore_expected`)、`changes.state` / `change_hunks.state` の更新、`pre_restore` のスナップショット。
10. プレビュー (戻した後の内容と今との差を DiffView で) と確認。stage 済みのファイルと重なるときの注記 (restore §7。Index は読みも書きもしない。重なりは既存の `SourceControlSession` の status で分かる範囲)。
11. git を 1 つも起動しない。`src\Editor\ChangeTracking\` から `SourceControl\` の書き込み系を呼ばない。

## やらないこと (このサブでは)

- ライブのシーンの復元 (sub-16)。開いているシーンのファイルが対象で dirty なら止める (restore §2)。
- MCP からの復元。自動の復元。3 方向のマージ。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\src\Editor\ChangeTracking\RestoreManager.*`、`RestoreJournal.*`、`HunkApply.*`、`PathGuard.*`、`AiChangesWindow.*`
- `C:\HAL\MyEngin\src\Editor\App\EditorApp.*` (ゲート入力の共有、起動時の回復)
- `C:\HAL\MyEngin\tools\track\src\` (`restore.rs`)、`tests\track\`
- 参考: `C:\HAL\MyEngin\src\Editor\SourceControl\GitTransaction.h` / `.cpp`、`C:\HAL\MyEngin\tools\mcp\src\notes.rs` (パスの検査)

## 受け入れ条件 (このサブ)

1. restore §3.1 の表の 3 行と、追加・削除・リネームの条件が、それぞれテストで確かめられる → `changetracking` スイート。
2. restore §3.2 の表の 4 行、文脈 0 行、複数 Hunk の後ろからの適用、1 つ競合で全部やめる、改行の種類の保持 → スイート。C++ の Hunk 適用と Rust の sub-04 のテスト用適用が、同じ固定入力で同じ結果になる → 固定表。
3. Hunk を 1 つだけ戻したファイルが、「After からその Hunk だけ Before に戻した内容」とバイト一致する → スイート (要求 §16.4 の 1 番目)。
4. After の後で再編集された対象は競合になり、ファイルが 1 バイトも書かれない → スイート (§16.4 の 3 番目)。
5. ゲート: Playing、NetActive、ビルド中、マージ中、サービス未接続、回復待ち、Blob 欠落のそれぞれで止まり、理由が全部並ぶ。確認の画面を出した後に Playing になったら、実行の直前の判定で止まる → スイート (§16.5 の 2 番目)。
6. 3 ファイルの復元で 2 つ目の書き込みを失敗させると、1 つ目が `pre\` の内容へ戻り、状態が `rolled_back`。巻き戻しの途中で 1 つ目が他者に書き換えられていたら `recovery_required` で、そのファイルは触らない → スイート (§16.4 の 7 番目)。
7. `applying` のジャーナルを残して起動すると回復が検出され、選ぶまで何も書かれず、新しい復元が止まる → スイート (§16.4 の 8 番目)。
8. どの経路 (成功、競合、失敗、巻き戻し) でも `BeginBatch` と `EndBatch` の回数が一致する → スイート (呼び出しの計数)。
9. **復元の前後で `.git\index` のバイト列が同じ。復元の間に git のプロセスが 1 つも起動されない** → スイート (一時 git リポジトリ) (§16.5 の 1 番目)。
10. パスの検査: `..`、絶対パス、UNC、`CON`、ジャンクション越し、`.git` の下、のそれぞれが拒否される → スイート。
11. 復元で書いた変更が、サービスの履歴に `actor = restore` で現れ、`restore_operations` に状態の遷移が残る → `tools\track_verify.bat` (§16.4 の 6 番目)。
12. プレビューと回復の画面のスクショ (英日) → `--screenshot`。
13. 実サービスと一時プロジェクトで、AI Changes 窓から Hunk を 1 つ戻す操作を手動で 1 回通す。通せなければ「未確認」と書く → 手動。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest-full`、`cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
