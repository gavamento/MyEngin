# sub-27: ファイルと Hunk の適用、復元ジャーナル、巻き戻し、起動時の回復 (M104p2)

- 依存: sub-15
- 状態: OK (commit は台帳 harness.md を参照)
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

SELF_EVAL: sub-27 (round 1)
実装:
  - src\Editor\ChangeTracking\RestoreWriteIo.{h,cpp} (新規) — 利用者のファイルとジャーナルを書く・消す・名前を変える・複写する・ディレクトリを作る口を 1 か所に集めた (Write は WriteFileReplacing)。試験が偽物 (FaultyWriteIo) で N 回目の失敗・例外・他者の書き込みを差し込む。読み取りの RestoreFileIo は読み取り専用のまま
  - src\Editor\ChangeTracking\RestoreJournal.{h,cpp} (新規) — `<project>\.mye\ai_changes\recovery\<restoreId>\` の manifest.json (状態・操作・パスごとの「軌跡」・進んだ操作の数)、pre\0001.bin、起動時の検出 (Scan)、prepared の破棄と 7 日後の掃除 (Prune)、壊れた manifest の扱い (dismissed.txt)
  - src\Editor\ChangeTracking\RestoreRunner.{h,cpp} (新規) — 判定し直し (プレビューと同じ入力から計画を作り直し、書く内容のハッシュまで同じことを確認。ゲート・パスの検査 (2 回目)) → blob_get で Before を取り Editor 側でハッシュ確認 → 適用前の内容を pre\ へ全部保存して読み直し → ゲートとパスと内容の最後の確認 → applying → restore_expect / restore_record → BeginBatch → 操作を 1 フレーム 1 つ (操作ごとにパスの検査と「今の内容が軌跡の直前の状態」の確認) → 読み直して検証 → committed → EndBatch。失敗は軌跡に合うファイルだけ適用前へ戻す巻き戻し (他者が書いた内容は触らず recovery_required)。起動時の回復 (StartRecoveryRollback / KeepCurrent / Dismiss)、記録の送り直し、履歴 (restore_list)、Shutdown。例外でも EndBatch を 1 回通す。8 MiB 超は別スレッドのハッシュ・複写・ジャーナルへの塊ごとの取得で、メモリに載せずフレームを止めない
  - src\Editor\ChangeTracking\RestoreRunFormat.{h,cpp}、src\Editor\Windows\Project\RestoreRunWindow.{h,cpp} (新規) — 進行・結果 (モーダル)・起動時の回復の画面・履歴の画面。文面は ImGui を呼ばない純関数 (試験が確かめる)。AiChangesWindow.cpp には足していない
  - src\Editor\ChangeTracking\RestorePreview.{h,cpp}、RestorePlan.h — 8 MiB 超のファイル全体は Sha256FileHex を別スレッドで回してハッシュだけで判定 (Hunk は従来どおり NowTooLarge)。NowState.bytesOmitted、NowOld()
  - src\Editor\ChangeTracking\ChangeTrackingSession.{h,cpp} — RestoreRunner を持つ (窓の開閉と無関係に Poll で進む。client_ より後に壊れる宣言順)
  - src\Editor\Windows\Project\AiChangesWindow.{h,cpp}、RestoreFormat.{h,cpp} — 「復元する」を押せるようにした (照合 Ready + 止める理由なしのときだけ。押すと ApplyRestore → 実行の直前にゲートを判定し直す)。BuildRestoreBlocks (プレビュー・実行の直前・回復が同じ口)。回復待ちの知らせと復元の履歴のボタン。説明の一言は折り返し
  - src\Editor\App\EditorApp.{h,cpp} — RestoreRunHooks を ReloadHub の BeginBatch / EndBatch へ繋ぐ、対話起動のときだけ ScanRecovery、restoreJournalPending を実状態に差し替え、BuildGateInputs の opInFlight に WriteSectionActive を足す、保存・Undo のショートカットとファイルのドロップを書き込み中は受けない、Shutdown で EndBatch、RestoreRunWindow を毎フレーム
  - src\Editor\ChangeTracking\TrackProtocol.h、src\Engine\Core\Localization\LocalizationTable.inl — op 名と文面 (英日。下位の層の差分はこの 1 ファイルだけ)
  - tools\track\src\restore.rs (新規)、ops_record.rs、ops.rs、server.rs、protocol.rs、session.rs、revision.rs、attribution.rs、lib.rs — restore_record (restore_operations の更新、applying で pre_restore のスナップショット、committed で changes / change_hunks の state を restored / partially_restored に。reviews には触れない)、restore_expect (パスごとのイベント。観測が先でも後でも行為者 restore。窓は予告の 10 秒前〜120 秒後)、restore_list。events から作り直せる (rebuild_derived が DELETE FROM restore_operations と 2 つの kind を含む)。スキーマは版 3 のまま
  - 試験: src\Editor\ChangeTracking\RestoreApplySelfTest.cpp (新規。一時プロジェクト・本物のファイル)、ChangeTrackingSelfTest.{h,cpp} (呼び出しと MYE_TRACK_PROBE の実サービスでの Hunk 復元の追加)、RestoreSelfTest.cpp (canApply の期待と、8 MiB 超の判定の期待を直した)、tools\track\tests\session.rs (5 件)、tests\track\28_restore.ndjson + expected
  - 文書: docs\ai-change-tracking\restore-safety.md §8.2、ipc-contract.md §4.4、db-schema.md、attribution-rules.md、docs\test_checklists.md (M104p2 の手動確認)
仕様との差分:
  - [逸脱] 条件 4 の「3 ファイルの復元で 2 つ目の書き込みを失敗」: sub-15 の計画の作り手は触るファイルが 2 つまで (名前の戻し + 内容)。2 つ目の書き込みの失敗は、名前を戻して内容を書く 2 操作で試験した。3 ファイルは、ジャーナルを手で組んで起動時の回復の巻き戻しを回し、「軌跡に合う 2 つは戻り、他者が書いた 1 つは触らない」を確かめた (RestoreRunner の巻き戻しはファイル数に依らない作り)
  - [逸脱] 条件 7 の「一時の git リポジトリ」: 試験は .git\index と HEAD を持つ一時ディレクトリで、実際の git は使っていない。.git\index のバイト列が復元の前後で同じこと、復元の間に git.exe を含む子プロセスが 1 つも現れないこと (毎ラウンドのプロセス列挙)、書き込みの口が .git の下を 1 度も呼ばれないこと、実行系のソースが書き込み・プロセス起動の API を持たないこと (grep)、で確かめた
  - [未実装] 段階 B / C (スクリプトの再コンパイル・再起動) の自動の作り直し。EndBatch へは変更集合 (Modified / Added / Renamed / Deleted) を渡す。ReloadHub は Deleted を扱わない (既存のコメントのとおり) ので、削除したアセットの登録は残る。プレビューの注記が見込みを出し、利用者がする (restore-safety.md §8.2 に書いた)。Engine 層は触っていない
  - [未実装] Hunk の複数選択の UI (任意とされていた)。計画も RestoreRunner も複数の Hunk を扱えるが、窓は今までどおり 1 つ選んで復元する
  - [追加] 状態 `recovered` (起動時の回復の画面で利用者が選んで済ませた。resolution = restored_pre / kept_current / dismissed)。restore_operations.state の値と restore_record の state に足した
  - [追加] op `restore_list` (role = editor。復元の履歴の画面に要る。restore_operations を新しい順に返す)。ipc-contract.md に書いた
  - [追加] RestoreRunner::Shutdown (Editor の終了時に BeginBatch の対になる EndBatch を通し、ジャーナルは applying のまま残す)
  - [追加] restore_expect の窓: 予告は書き込みの前に届くので、観測は予告の時刻の 10 秒前から 120 秒後まで (editor_saved の 10 秒より長い。大きいファイルの書き込みと監視のデバウンスの猶予。パスと内容が同じ変更にしか効かない)
  - [追加] 大きい内容の所定位置への置き換えは MoveFileExW (ジャーナルの new\ から名前の付け替え)、pre\ への保存は CopyFileW (別スレッド)。8 MiB 以下は WriteFileReplacing。どちらも RestoreWriteIo の中だけ
  - [追加] 回復待ちのジャーナルがあるときの AI Changes 窓の知らせ (「回復が必要な復元があります」+ 開くボタン)、復元の履歴ボタン
検証:
  - ビルド: MyEngine.sln を Debug|x64 / Release|x64 で -p:MyeWarnAsError=true → 成功 (警告 0。libtess2 の既存の警告は対象外)。スクショ用プローブ (RestoreShotProbe.h と EditorApp の 2 行) を外してから Release / Debug の Editor をビルドし直した。外したことを grep (SHOTPROBE / RestoreShotProbe) と gen_project_files の再実行で確認
  - Debug MYE_SELFTEST_ONLY=changetracking → 0 失敗 (871 PASS、18.6 秒)。Release → 0 失敗。Release + MYE_TRACK_PROBE=1 → 0 失敗 (900 PASS。実サービスで Hunk を 1 つ復元 → ファイルが戻る → 履歴に行為者 restore → restore_operations が committed → 変更が restored、BeginBatch / EndBatch が 1 回ずつ、実行後 mye_track.exe なし)
  - Debug の physics を除く全 124 スイート (--selftest-list の fast / full 行から。MYE_SELFTEST_ONLY) → 0 失敗 (762 秒)。Release --selftest-full → 125 スイート 0 失敗 (253 秒)。最後の小変更のあとの Release --selftest → 112 スイート 0 失敗 (26 秒)
  - cargo test --manifest-path tools\track\Cargo.toml (全体。ignored は付けない) → 全て ok (新規: tests\session.rs の 5 件 = 予告が先 / 観測が先・別内容や別パスでは付かない・editor_saved が勝つ、committed で Hunk 単位 / 全体の state と pre_restore・reviews が 1 行も変わらない・restore_list、引数の検査とロール、DB を壊して作り直しても restore_operations・changes / change_hunks の state・根拠が同じ。restore.rs の単体 2 件と、protocol.rs の役割の表の検査への追加)
  - tools\build_track.bat Release (mye_track.exe が動いていないことを確認してから。tools\mcp\target\release には触れていない) → tools\track_verify.bat → 全シナリオ PASS (新規 28_restore: restore_record / restore_expect / restore_list、mcp / hook は forbidden、bad_args)
  - pwsh -NoProfile -File tools\check_rules.ps1 → 0 error (66 warning は既存)
  - 一時プロジェクトでの実走 (changetracking スイート。本物のファイルと本物の読み取り口 + FaultyWriteIo): ファイル全体・Hunk 1 つ (After からその Hunk だけ Before に戻した内容とバイト一致)・追加の取り消し (削除)・削除の取り消し (作成。親も作る)・名前の戻し (2 操作 / 1 操作) / 競合では 1 バイトも書かれずジャーナルも作られず Begin / End が 0 / 確認のあとに対象が書き換えられた・Play を始めた (ゲート)・書く直前にゲートが閉じた・保存のあとに対象が書き換えられた・プレビューのあとで親がジャンクションに差し替えられた (本物のジャンクション) → 書かずに止まる。applying のあとで差し替えられたら操作ごとの検査が止める / 2 つ目の書き込みの失敗 → rolled_back / 巻き戻しの途中の他者の書き込み → recovery_required でそのファイルは触らない / 例外 → 巻き戻して EndBatch 1 回 / 書いたあとの他者の書き換え → 上書きせず recovery_required / 起動時の回復 (prepared・applying の 1 つ目の後・rolling_back・recovery_required・committed・壊れた manifest・manifest の無いディレクトリを直接置く。検出だけでは何も書かない・新しい復元が止まる・今のままにする・適用前へ戻す (ゲートも効く)・無視する) / 書き込みの途中の Shutdown / Blob の取得中の切断・ハッシュ不一致・ジャーナルを書けない → 何も書かない / 8 MiB 超 (ハッシュ・複写・取得が数フレーム、ジャーナルへ複写、巻き戻し、取り消し) / 記録の送り直しと履歴 / 窓のボタンと進行・結果の画面 (英日) / 書き込み API とプロセス起動 API が RestoreWriteIo.cpp 以外に無いこと (grep)
  - スクショ (一時のプローブ + Release の Editor.exe --frames 40 --shot-frame 36 --screenshot。プローブは外した): C:\HAL\MyEngin\cache\m104p2_shots\ に confirm_ja / confirm_en (実行の確認)、progress_ja (進行)、done_ja / done_en (成功)、rolled_ja / rolled_en (巻き戻した)、recovery_ja / recovery_en (回復の画面)。撮れた・文字が化けていない・タイトルの ### が出ない・説明が切れない、は確認。見た目の合否はユーザーの目視 (未確認)。回復の画面は既定の浮動窓 (Animation / Assets) と重なって写っている
  - 作業ツリー: git status --short に復元による変更は無い (このリポジトリで復元を試していない。.mye も作っていない)。src\Engine の差分は LocalizationTable.inl だけ。tests\track\28_restore.* と tools\track\src\restore.rs は新規
  - 未実施: replay_verify (Editor 層と文面だけの変更でシミュレーション状態に触れないため回していない)、実 Editor を人が操作する確認 (押す・取り消す・回復の選択・強制終了からの回復・見た目)、ReloadHub が開いているシーンへ実際に差分を当てる経路 (EndBatch が Modified を流すところまで。ReloadHubSelfTest の既存の試験が一括適用の経路は口を通らないことを確かめている)、stage 済みの実走、CI での実走
自己採点 (1-5):
  仕様適合: 4 — 条件 1〜12・14 を試験・プローブ・cargo・track_verify で満たした。条件 13 はスクショを撮ったが見た目の合否は目視待ち。差分欄の 3 ファイルの計画・git の一時リポジトリは代替で確かめた。段階 B / C の自動の作り直しと Hunk の複数選択の UI は未実装
  正しさ: 4 — 書く前の確認 (計画の同一・ゲート・パス・内容)・軌跡による巻き戻し・他者の書き込みの保護・EndBatch の対・Blob のハッシュ確認を、本物のファイルと差し込んだ失敗で確かめた。実 Editor の操作、ReloadHub の実適用、強制終了の実機での回復は未確認
  コード品質: 4 — 書き込みの口を 1 か所、実行・ジャーナル・画面を別ファイルに分けた。RestoreRunner.cpp は約 1000 行で段階ごとの関数に分けているが長い。回復の画面の見た目は調整の余地あり
  テスト: 4 — 一時プロジェクトで本物のファイルを書いて確かめ、実サービスのプローブと Rust 側の再構築も確かめた。3 ファイルの計画・本物の git は代替。実 Editor の GUI 操作は無し
不安・質問:
  - [planner] 段階 B / C (スクリプトの再コンパイル・再起動) を復元の後で自動にしない判断 (注記だけ) と、ReloadHub が Deleted を扱わないために、削除した (追加の取り消しをした) アセットの登録が残る件。sub-16 か別サブで扱うか決めてほしい。Engine 層は触れない取り決めなので触っていない
  - [planner] `recovered` (回復の画面で利用者が選んだ結果) を restore_operations.state の値に足した。db-schema.md の v1.sql のコメントは凍結なので、その行のコメントは db-schema.md 側 (コピー) にだけ足した
  - [planner] restore_expect が届いていない書き込み (サービス切断中の復元) は「行為者が分からない変更」になる。届くまで書き込みを待たせる (復元を始められない) 運用にするかは、現状はゲートが接続を要求するので始めた後の切断だけが対象。許容してよいか
  - [planner] 計画の作り手は 2 ファイルまで (sub-15)。複数 Hunk の選択を UI に出す・複数ファイルの計画を作る拡張は sub-16 以降か
触ったファイル:
  新規: src\Editor\ChangeTracking\RestoreWriteIo.h / .cpp、RestoreJournal.h / .cpp、RestoreRunner.h / .cpp、RestoreRunFormat.h / .cpp、RestoreApplySelfTest.cpp、src\Editor\Windows\Project\RestoreRunWindow.h / .cpp、tools\track\src\restore.rs、tests\track\28_restore.ndjson、tests\track\28_restore.expected.ndjson
  変更: src\Editor\ChangeTracking\ChangeTrackingSelfTest.h / .cpp、ChangeTrackingSession.h / .cpp、RestoreFormat.h / .cpp、RestorePlan.h、RestorePreview.h / .cpp、RestoreSelfTest.cpp、TrackProtocol.h、src\Editor\Windows\Project\AiChangesWindow.h / .cpp、src\Editor\App\EditorApp.h / .cpp、src\Engine\Core\Localization\LocalizationTable.inl、tools\track\src\attribution.rs / lib.rs / ops.rs / ops_record.rs / protocol.rs / revision.rs / server.rs / session.rs、tools\track\tests\session.rs、docs\ai-change-tracking\restore-safety.md / ipc-contract.md / db-schema.md / attribution-rules.md、docs\test_checklists.md、plans\m104-ai-change-tracking\sub-27.md (実装メモ)
  生成物だが新規ファイルの登録に要る (司会がステージ): build\Editor.vcxproj、build\Editor.vcxproj.filters (tools\gen_project_files.ps1 の出力)
  含めない: cache\m104p2_shots\ のスクショ (コミットしない)
消した・止めたもの:
  - 消した: src\Editor\ChangeTracking\RestoreShotProbe.h (スクショ用の自分の一時プローブ。EditorApp の 2 行も外した)、cache\m104p2_shots\progress_en.png (自分が撮った不要な 1 枚)、試験が %TEMP% に作る mye_restore_apply_* の一時ディレクトリ (試験が自分で消す。残りなし)
  - 触らなかったもの: 既存の %TEMP%\mye_track_*、%TEMP%\mye_m104b\、cache\m104*_shots の既存分、tools\mcp\target\release\ (ユーザーの mye_mcp.exe が動いていた)、C:\HAL\MyEngin\.mye は作っていない、.claude\settings.json と .mcp.json
  - 止めたプロセス: なし (自分が起動した Editor.exe / mye_track.exe は各試験が終了まで回した。終了後に mye_track.exe が残っていないことを Get-Process で確認)。build_track.bat は mye_track.exe が動いていないことを確認してから回した
申し送り:
  - reviewer: 安全性の中心は RestoreRunner.cpp の TickRecheck / TickSave / BeginApply / TickApply / TickRollback。軌跡 (JournalFile.states) による「自分が書いた内容のファイルだけを戻す」規則が、巻き戻しと起動時の回復の両方を決めている (restore-safety.md §8.2)
  - sub-16 (ライブのシーンの復元): RestoreRunner は File / Hunks のファイルだけ。Begin / End のフックと opInFlight、回復の画面、履歴、restore_record / restore_expect は共有できる。Undo を使うシーンの復元はジャーナルに内容を持たない (restore §5)
  - 実 Editor での確認 (docs\test_checklists.md の M104p2 の節): 強制終了からの回復、大きいファイルの書き込み中の操作の遮断、サービス切断中の復元、見た目
  - pre_restore のスナップショットを Blob の回収から守る処理はこのサブでは足していない (db-schema.md §の「消さないもの」は保存期間の掃除が入るサブで)。Editor の pre\ がジャーナルだけで巻き戻せる実体なので、サービスの Blob の回収には依存しない
  - 起動時の回復で適用前のハッシュの計算は同期 (大きいファイルは画面を開いた最初の 1 フレームが数百 ms 止まりうる)。見た目の目視で気になれば別フレームへ分ける
  - コミットは司会が行う。このサブで足した試験の追加によって Debug の changetracking は約 19 秒 (Release 約 3〜9 秒)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、作業ツリーに復元による変更なし、下位の層の差分は `LocalizationTable.inl` だけ、スキーマ・`tools\mcp`・`.claude`・`.mcp.json` に差分なし、プローブ (`RestoreShotProbe.h`) の残りなし。**`src\Editor\ChangeTracking\` と `RestoreRunWindow.cpp` で、ファイルを書く・消す・名前を変える・複写する・プロセスを起動する API を grep して、`RestoreWriteIo.cpp` (と既存の `TrackLaunch` / `TrackIo` / 試験) の外には 0 件。** `BeginBatch` / `EndBatch` の新しい呼び手は `EditorApp.cpp` の復元のフック (275、280 行目) と終了時 (581 行目) だけ。planner の実走: Debug の `MYE_SELFTEST_ONLY=changetracking` が 871 PASS / 0 FAIL、Release の `--selftest` 全体が 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`cargo test` (track) 285 件通過、`tools\build_track.bat Release` → `tools\track_verify.bat` 全シナリオ PASS、`tools\check_rules.ps1` 0 error。`--selftest-full`、Debug の physics を除いた全スイート、`MYE_TRACK_PROBE=1`、スクショは coder の結果による。
  - 逸脱の判定: 条件 4 の「3 ファイル」を、2 操作の適用 + 手で組んだ 3 ファイルのジャーナルの回復で確かめたのは可 (巻き戻しはファイル数に依らない作りで、軌跡による保護は 3 ファイルで確かめている)。条件 7 を、`.git\index` を持つ一時ディレクトリ + 子プロセスの列挙 + 書き込みの口が `.git` を呼ばない + grep で確かめたのは可。
  - 追加の判定: `recovered` の状態、`restore_list`、`RestoreRunner::Shutdown`、`restore_expect` の窓 (10 秒前〜120 秒後)、大きい内容の `MoveFileExW` / `CopyFileW`、回復待ちの知らせ、は全部採る。
  - 質問への回答: (1) 段階 B / C の自動の作り直しはしない (注記のまま)。削除の後にアセットの登録が残る件は、sub-16 で Editor 層の既存の再走査の経路を探して呼ぶか、限界として画面に出す (Engine 層は触らない)。(2) `recovered` を足したのはよい。凍結した `v1.sql` のコメントを触らず `db-schema.md` 側に書いたのも正しい。(3) 切断中に書いた分が「誰のものか分からない変更」になるのは許容 (始める時点で接続を要求している。文書に限界として書いてある)。(4) Hunk の複数選択の UI と複数ファイルの計画は M104 では行わない (spec §3 の後回し)。
  - 未確認: 実 Editor を人が操作する確認 (押す・取り消す・回復の選択・強制終了からの回復)、`ReloadHub` が開いているシーンへ実際に差分を当てる経路、stage 済みの実走、スクショの見た目。