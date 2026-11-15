# sub-27: ファイルと Hunk の適用、復元ジャーナル、巻き戻し、起動時の回復 (M104p2)

- 依存: sub-15
- 状態: 未着手
- 往復: 0
- 順序: sub-15 → **sub-27** → sub-16 (番号は末尾。sub-26 の VERDICT で、元の M104p を 2 本に割った)
- 書き込みの深さ: **着手可** (2026-11-15、sub-15 の VERDICT で planner が sub-15 の結果と取り決めを書き足した)
- 質問の運用: 推奨で進め、Notion に後回し。

**この機能で初めて、利用者のファイルを書くサブ。** 安全性は P0 (要求 §15 Phase 5)。`C:\HAL\MyEngin\docs\ai-change-tracking\restore-safety.md` §4.1、§5、§6 (restore)。**試験は必ず一時プロジェクトで行い、このリポジトリの作業ツリーで復元を試さない。**

## やること

sub-15 の計画 (何を書くか・照合の根拠・ゲート・パスの検査) を、実際に適用する。

1. **復元ジャーナル** `<project>\.mye\ai_changes\recovery\<restoreId>\` (restore §5)。Editor が所有する (サービスは書かない)。`manifest.json` (状態、対象の一覧、各ファイルの適用前後のハッシュ、進んだところ) と `pre\0001.bin …` (適用前の内容)。`manifest.json` の更新は `WriteFileReplacing`。**適用前の内容を全部保存し終えてから、状態を `applying` にする。**
2. **実行の直前の判定し直し**: ゲート (sub-15 の C)、競合の検証 (今の内容を読み直す)、パスの検査 (sub-15 の D。2 回目)。確認の画面を出している間に変わっていたら、書かずに止めて理由を出す。
3. **適用** (restore §4.1): `ReloadHub::BeginBatch` 〜 `EndBatch(changes)` で囲む。**エラーで抜ける経路でも必ず `EndBatch({})`** (通さないとホットリロードが止まったままになる)。書き込みは `WriteFileReplacing` (`C:\HAL\MyEngin\src\Engine\Platform\PathUtil.h`)。削除・作成・名前を戻す、も同じ囲みの中。`BatchChange` の `Deleted` は `ReloadHub` が扱わない (既存のコメント) ので、削除を含む復元の後の扱いを着手時に確かめる。
4. **整合性の検証**: 書いたファイルを読み直してハッシュを確かめる。全部合えば `committed`。
5. **巻き戻し** (restore §6): 途中で 1 つでも失敗したら、既に書いたファイルを `pre\` の内容へ戻す (`rolling_back` → `rolled_back`)。戻す前に、今の内容が「自分が書いた内容」と一致することを確かめ、違えば (途中で誰かが書いた) そのファイルは触らず `recovery_required`。
6. **起動時の回復**: Editor の起動時 (対話起動のときだけ) に `recovery\` を見て、`applying` / `rolling_back` / `recovery_required` があれば回復の画面を出す。ファイルごとに 3 つの内容 (適用前、書こうとした内容、今の内容) と選択肢 (適用前へ戻す / 今のままにする)。**勝手には戻さない。** 回復待ちの間は新しい復元を始められない (sub-15 の「未完了があるか」の口を本物にする)。画面を閉じても Editor は普通に使える。`committed` のジャーナルは一定期間 (7 日) の後に消す (自分のジャーナルの掃除)。
7. **サービス側**: op `restore_record` (進行の記録。`restore_operations` 表) と `restore_expect` (これから Editor が書くパスと内容のハッシュ。観測が一致したら行為者 `restore`)。どちらも `role = editor` だけ。`restore_expect` は sub-23 の `editor_saved` と同じ作り (`attribution.rs` の知らせの照合、`session.rs` の `apply_event`、`revision.rs` の作り直し)。`changes.state` / `change_hunks.state` を `restored` / `partially_restored` に更新する (レビューの記録には触れない。要求 §10.5)。`pre_restore` のスナップショット。events を通し、作り直せること (spec §2 D26)。スキーマの版は 3。足りなければ移行 v4。
8. 自分の書き込みで「エディタの外でシーンが変更されました」の確認が出ないこと (一括適用の経路は口を通らない。sub-13)。自分の書き込みを `editor_saved` として二重に知らせないこと (`restore_expect` が先)。
9. AI Changes 窓: sub-15 のプレビューの「実行」を押せるようにする。進行の表示、結果 (成功 / 巻き戻した / 回復が要る)、復元の履歴 (`restore_operations`)。
10. git を 1 つも起動しない。Index を読みも書きもしない。

11. sub-15 の結果と、追加の取り決め (2026-11-15):
    - **書くことの全部は `RestorePlan.ops`** (`src\Editor\ChangeTracking\RestorePlan.h`)。操作は BeforeBlob (ハッシュの Blob を `blob_get` で取って書く) / Bytes (そのまま書く) / Delete / Create / Rename。このサブは、計画に無いものを書かない。競合・復元済み・戻せない、のとき `ops` は空。
    - 書く直前に、`CheckRestorePath` (`PathGuard`) と `EvaluateRestoreGate` (`RestoreGate`。窓からは `AiChangesWindow::RestoreBlocks`) を呼び直し、`BuildRestorePlan` を今の内容で作り直して、プレビューのときの計画と同じ (書く内容のハッシュが同じ) ことを確かめる。違えば書かずに止める。
    - `RestoreFileIo` は読み取り専用のまま残す。**書き込みの口は別のファイル (例 `RestoreWriteIo`) に分け、書く・消す・名前を変える・ディレクトリを作る、をその 1 か所に集める。** 試験が偽物に差し替えて、失敗を差し込めるようにする (2 つ目の書き込みで失敗、巻き戻しの途中で失敗、など)。
    - `BeforeBlob` の内容は、`blob_get` で全部取り、**取った内容のハッシュが、Revision の Before のハッシュと一致することを Editor 側でも確かめてから**書く (サービスや pipe の途中で化けた内容を書かない)。取っている間に切断したら、何も書かずにやめる (この時点ではジャーナルは `prepared` か未作成)。
    - **8 MiB を超えるファイル**: sub-15 は判定しない (復元できない) としている。`Sha256FileHex` (ストリーム) で今の内容のハッシュを取り、ファイル全体の復元だけ判定・適用できるようにする (Hunk 単位は対象外のまま)。`pre\` への保存と書き込みも、メモリに全部載せずに行う。ハッシュの計算と大きいファイルの複写がフレームを止めないこと (数フレームに分ける。進行を出す。キャンセルは、書き始める前だけ)。
    - `restoreJournalPending` (sub-15 では常に偽) を、`recovery\` の実際の状態に差し替える。「復元する」(`RestoreScreen::canApply`) を、ゲート・競合・パスが全部通っているときだけ押せるようにする。
    - 復元の画面 (実行、進行、結果、回復、履歴) は、`AiChangesWindow.cpp` (約 1600 行) に足さず、別のファイルに分ける。
    - 適用は 1 フレームで終えなくてよい (複数ファイル、大きいファイル)。**ただし `BeginBatch` 〜 `EndBatch` の間は、他の操作 (Play の開始、シーンの保存、別の復元、git の操作) を始められないこと。** 既存の `GitTransaction` が、進行中の op をゲート (`OpInFlight`) でどう表しているかを見て、復元の進行中も同じように立てる (`AiChangesHost` 経由)。
    - 復元の途中で Editor が落ちた場合を試験で作る: ジャーナルの各状態 (`prepared` / `applying` の 1 つ目の後 / `rolling_back`) のディレクトリを直接置いて、起動時の検出と回復を確かめる。
    - サービスに接続していない間は、復元を始められない (sub-15 のゲート)。適用の途中で切断した場合: ファイルの書き込みは Editor とジャーナルだけで完結させ (サービスが居なくても巻き戻せる。restore §5)、`restore_record` は後で送り直すか、送れなかったことを結果に出す。サービスから見ると、その書き込みは「誰のものか分からない変更」として観測される (`restore_expect` が届いていなければ)。この制限を文書に書く。
    - sub-15 の質問 2 の扱い: 7 つの窓 (Animation / Animator / Audio Mixer / Behavior Tree / Project Settings / Sprite / Tile Palette) は編集中の文書のパスを出さないので、その窓に未保存があれば、関係の無いファイルの復元も止まる (安全側)。変えない。

## やらないこと (このサブでは)

- ライブのシーンの復元 (sub-16)。開いているシーンのファイルが対象で未保存なら、ゲートで止まる (sub-15)。
- MCP からの復元。自動の復元。3 者のマージ。
- Engine 層の変更 (`ReloadHub`、`WriteFileReplacing` に手を入れない。足りなければ「不安・質問」で返す)。

## 受け入れ条件 (このサブ)

1. Hunk を 1 つだけ戻したファイルが、「After からその Hunk だけ Before に戻した内容」とバイト一致する。ファイル全体・追加の取り消し (削除)・削除の取り消し (作成)・リネームを戻す、のそれぞれ → `changetracking` スイート (一時プロジェクト) (要求 §16.4 の 1 番目)。
2. After の後で再編集された対象は競合になり、**ファイルが 1 バイトも書かれず、ジャーナルも `applying` にならない** → スイート (§16.4 の 3 番目)。
3. 確認の画面を出した後に状態が変わる (Play を始めた、対象が書き換えられた) と、実行の直前の判定で止まる → スイート (§16.5 の 2 番目)。
4. 3 ファイルの復元で 2 つ目の書き込みを失敗させる (差し込み) と、1 つ目が `pre\` の内容へ戻り、状態が `rolled_back`。巻き戻しの途中で 1 つ目が他者に書き換えられていたら `recovery_required` で、そのファイルは触らない → スイート (§16.4 の 7 番目)。
5. `applying` のジャーナルを残して起動すると回復が検出され、選ぶまで何も書かれず、新しい復元が止まる。「適用前へ戻す」「今のままにする」のそれぞれ。ジャーナルの `manifest.json` が壊れている場合も落ちずに回復待ちとして扱う → スイート (§16.4 の 8 番目)。
6. どの経路 (成功、競合、失敗、巻き戻し、例外) でも `BeginBatch` と `EndBatch` の回数が一致する → スイート (呼び出しの計数)。
7. **復元の前後で `.git\index` のバイト列が同じ。復元の間に git のプロセスが 1 つも起動されない** → スイート (一時の git リポジトリ) (§16.5 の 1 番目)。
8. 書く直前のパスの検査: プレビューの後で対象の親がジャンクションに差し替えられたら、書かずに止まる → スイート。
9. 復元で書いた変更が、サービスの履歴に行為者 `restore` で現れ、`restore_operations` に状態の遷移が残り、対象の `state` が `restored` / `partially_restored` になる。レビューの記録は変わらない。DB を壊して作り直しても同じ → `cargo test` + `tools\track_verify.bat` (§16.4 の 6 番目)。
10. `restore_record` / `restore_expect` が `mcp` / `hook` で `forbidden` → `tools\track_verify.bat`。
11. 復元の間と後に、「エディタの外でシーンが変更されました」の確認が出ない。`editor_saved` が二重に送られない → スイート。
12. 実サービスとの結合 (`MYE_TRACK_PROBE=1`、一時プロジェクト): AI の変更を記録 → Hunk を 1 つ復元 → ファイルが戻る → 履歴に `restore` → プロセスが残らない → 実行結果。実 Editor を人が操作する確認は未確認として残す。
13. スクショ (一時のプローブ): 実行の確認、結果、回復の画面、英語。**見た目の合否はユーザーの目視。**
14. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1`、`cargo test`、`tools\track_verify.bat` が通る。下位の層の差分が `LocalizationTable.inl` だけ。**このリポジトリの作業ツリーに、復元による変更が無い** (`git status` で確かめる)。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`MYE_TRACK_PROBE=1`、`cargo test --manifest-path tools\track\Cargo.toml`、`tools\build_track.bat Release`、`tools\track_verify.bat`、`pwsh -NoProfile -File tools\check_rules.ps1`、`git status --short`

## 実装メモ (coder が追記)

## フィードバック履歴