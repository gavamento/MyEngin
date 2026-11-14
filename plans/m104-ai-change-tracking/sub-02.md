# sub-02: `tools\track` の骨組み、DB、Blob、ジャーナル (M104c)

- 依存: sub-01
- 状態: OK (commit 79008917)
- 往復: 0

## 着手前にユーザー確認が要る未決事項

司会が着手時に聞く (計画の未決事項 4)。

- 聞きたいこと: Blob の除外パターンの既定値と、容量の上限・保存期間の既定値。
- 推奨: 設計どおり。除外は `.env`、`.env.*`、`*.pem`、`*.key`、`*.pfx`、`*.p12`、`id_rsa*`、`credentials*.json`。上限 2 GiB、保存期間 90 日。どれも設定で変えられる。
- 逆を選ぶと何が変わるか: 既定値の定数とテストの期待値が変わるだけで、構造は変わらない。除外を「無し」にすると、秘密情報の原本が `.mye\ai_changes\blobs\` に残る (要求 §14.2)。
- このサブで作るのは「除外の判定」と「`blob_state = excluded` の記録」まで。容量の回収 (db §7) は実行する側が Editor の確認を要するので sub-09 以降。上限と期間は定数として置くだけ。

## やること

sub-01 の結果 (`C:\HAL\MyEngin\docs\ai-change-tracking\prototype-results.md`) を先に読む。依存の版はそこで決まったものを使う。

sub-01 で決まったこと (2026-11-14 の見直しで追記):

- `Cargo.toml`: edition 2021、`rust-version = "1.89"` (spec §2 D22)、`rusqlite = { version = "0.42", features = ["bundled"] }`、`sha2 = "0.11"`、`notify = { version = "8", default-features = false }`、`windows-sys = "0.61"` (feature は ipc §1 の 7 つ。このサブで使わない feature は、使うサブで足してよい)、`serde`、`serde_json`。冒頭コメントに、collab と違って最低の Rust が 1.89 になる理由を書く。
- `sha2` 0.11 の出力は `{:x}` で整形できない。16 進化は自前で書き、既知のベクタ (空文字列と `abc` の SHA-256) でテストする。
- `Cargo.lock` をコミットする (collab / mcp と同じ)。
- 計画の進捗表で M104b が「実装済み・判定待ち」のまま残っている。このサブで進捗表を更新するときに「完了」に直す。

1. `tools\track\` に単独の Rust crate `mye_track` を作る (workspace にしない。edition 2021、`panic = "unwind"`。`C:\HAL\MyEngin\tools\collab\Cargo.toml` の冒頭コメントと同じ理由を書く)。出力は bin `mye_track.exe`。ロジックは lib 側に置き、`tests\` から呼べるようにする (collab の `cdylib + rlib` ではなく `rlib` + bin)。
2. CLI の骨組み: `mye_track.exe serve --project <root>` と `mye_track.exe --version`。このサブの `serve` は「DB を開いて起動時の検査を走らせ、結果を標準エラーへ出して終わる」まで (監視は sub-03、pipe は sub-05)。
3. projectId: 正規化したプロジェクトルートの SHA-256 の先頭 16 桁 (arch §3)。
   **(round 1 の VERDICT で直した。spec §2 D25)** `NormalizePathKey` は末尾の区切りを残すので、`C:\Proj\MyGame` と `C:\Proj\MyGame\` が別の projectId になる (round 1 の固定表の 1 行目と 3 行目)。projectId は mutex と pipe の名前になるので、同じプロジェクトにサービスが 2 つ立ち、同じ DB に書き手が 2 つできる。**projectId は「`NormalizePathKey` と同じ結果から、末尾の区切りを全部落としたもの」のハッシュにする。** ただしドライブのルート (`c:\`) は落とさない。`normalize_path_key` は C++ と同じ結果のまま残し、落とす処理は別の関数にして `project_id` から呼ぶ。固定表は「入力 / NormalizePathKey の結果 / projectId のもとになる文字列 / projectId」の 4 列にし、末尾の区切りの有無で projectId が同じになる行を持つ。sub-09 の C++ 側も同じ関数の分け方にする。
   正規化は `C:\HAL\MyEngin\src\Engine\Platform\PathUtil.h` の `NormalizePathKey` と**同じ結果**になること。C++ と Rust で同じ入力から同じ projectId が出ることを、固定の入出力の表でテストする (sub-09 が同じ表を C++ 側で使う)。
4. SQLite: db §2 の設定、§3 の 14 表と索引、`schema_migrations`。移行スクリプトの正本は `tools\track\src\schema\`。db §8 (移行前のコピー、1 トランザクション、自分より新しい版は `db_degraded`)。
5. Blob ストア: db §4 (内容アドレス、一時ファイル → 読み直して検証 → rename、先頭 2 桁のディレクトリ、除外パターン)。
6. イベントジャーナル: db §5 (DB より先に追記、16 MiB で次の番号、起動時に DB より後ろの行を入れ直す、Blob の中身は書かない)。
7. 起動時の検査: db §6 の 1〜6 (7 のベースラインは sub-03)。どの段で失敗してもプロセスは落ちず、「記録できない状態」を返す。
8. Writer スレッド 1 本に書き込みを直列化する形 (arch §3.1) を、このサブで作る。後続のサブはこのキューへ積むだけにする。
9. `.gitignore` に `tools/track/target/` を足す (spec §2 D6)。

## やらないこと (このサブでは)

- ファイル監視、ベースライン (sub-03)。行 diff (sub-04)。pipe と `service.json` (sub-05)。
- 容量の回収の実行 (db §7 の 2、3)。
- `check_rules.ps1`、`tools\mcp\`、`src\` の変更。`tools\build_track.bat` (sub-05)。
- `recovery\` への書き込み (Editor の持ち場)。

## 触る場所 (planner の見立て)

- 新規: `C:\HAL\MyEngin\tools\track\Cargo.toml`、`Cargo.lock`、`src\main.rs`、`src\lib.rs`、`src\project.rs` (projectId と正規化)、`src\db.rs`、`src\schema\` (移行)、`src\blob.rs`、`src\journal.rs`、`src\writer.rs`、`src\startup.rs`、`tests\storage.rs`。分け方は coder が決めてよい。
- `C:\HAL\MyEngin\.gitignore`
- `C:\HAL\MyEngin\docs\ai-change-tracking\db-schema.md` — 実装で形が変わった表だけ直す。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表。
- 一時ディレクトリの作り方は `C:\HAL\MyEngin\tools\collab\tests\service.rs` の `temp_repo()` の前例。

## 受け入れ条件 (このサブ)

1. 空のディレクトリを `--project` に渡すと `.mye\ai_changes\tracking.db`、`blobs\`、`journal\` ができ、スキーマ版 1 の全表がある → `cargo test`。
2. 同じ DB に移行を 2 回当てても結果が同じ。版が自分より新しい DB は書き込まれず `db_degraded` になる → `cargo test`。
3. `tracking.db` を壊す (先頭を上書き) と、`tracking.db.broken-<時刻>` へ退避され、空の DB が作られ、ジャーナルから入れ直せた分が入り、入れ直せなかった区間が `coverage_gaps` に残る → `cargo test`。
4. 同じ内容の Blob を 2 回入れてもファイルは 1 つ。書いた後の検証で不一致なら `snapshots` に行が足されない (検証を失敗させる差し込み口でテストする)。改行と BOM がバイト単位で保たれる → `cargo test`。
5. 除外パターンに当たるパスは `blob_hash = NULL`、`blob_state = excluded` で、Blob ファイルが作られない → `cargo test`。
6. ジャーナルへ追記した後、DB へ入れる前に止めた状態を作り、起動し直すと DB に入る。二重には入らない → `cargo test`。
7. `sessions.status = running` の残りが、居ないプロセスなら `interrupted` になる。`operations.finished_at` が NULL の残りが `abandoned` になる → `cargo test`。
8. projectId の固定表 (大文字小文字、末尾の区切り、`/` と `\`、日本語を含むパス) が通る。**末尾の区切りの有無 (`\`、`/`、複数個) で projectId が変わらない。`C:\` はそのまま** → `cargo test`。
9. 既存の検証が変わらない → `pwsh -NoProfile -File tools\check_rules.ps1` が 0 error。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `cargo build --release --manifest-path tools\track\Cargo.toml`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git status --short` (`tools\track\target\` が出ないこと)

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
実装: `tools\track\` に crate `mye_track` (rlib + bin)。`schema/` (v1.sql = 14 表 + 索引)、`db.rs` (開閉・退避・移行)、`blob.rs`、`journal.rs`、`store.rs`、`startup.rs` (db §6 の 1〜6)、`writer.rs`、`process.rs`、`project.rs`、`hash.rs`、`main.rs` (`--version` / `serve --project`)。`tests\storage.rs` (16 件)、`tests\project_id.rs` + `project_id_cases.tsv` (C++ の NormalizePathKey の実測 12 行)。`.gitignore`、db-schema.md、計画の進捗表 (M104b 完了 / M104c 判定待ち)。
仕様との差分: [追加] `events(journal_pos)` の一意索引 / [追加] `coverage_gaps.reason = journal_unreadable` と `events.kind = journal_unreadable` (読めないジャーナル行) / [追加] 退避は NOTADB・CORRUPT・quick_check 不合格のときだけ (ロック等では退避しない) / [追加] db_degraded 中もジャーナルには追記する / [追加] PID 無しの running は interrupted / operations は result=abandoned のみで finished_at は NULL のまま。いずれも db-schema.md に反映済み。[逸脱] `serve` の終了コードは Recording=0、それ以外=1 (仕様に指定なし)。
検証: cargo test 26 件 PASS (lib 7 / project_id 3 / storage 16)、cargo build --release OK (mye_track.exe --version と serve --project を実走し Recording)、check_rules 0 error、git status で target\ は出ない。ミューテーション 2 種 (検証の無効化、入れ直しの位置無視) でテストが落ちることを確認。selftest / replay_verify は src\ を触らないので未実行。CI での bundled ビルドは未確認 (sub-05)。rustc 1.89 での実ビルドは未確認 (手元は 1.98 のみ)。
自己採点: 仕様適合 4 (受け入れ 1〜9 を全て自動テストで確認、追加が数点)、正しさ 4 (突き合わせは単体 + 結合のみ。本物の Editor からは未接続)、コード品質 4、テスト 4。
不安・質問: (1) NormalizePathKey の `towlower` は "C" ロケールで ASCII のみ変換されるのを実測して合わせた。Editor が将来 setlocale を呼ぶと割れる。(2) 壊れた DB の入れ直しで戻るのは events だけ。changes / revisions などの派生表は M104e が events から作り直す前提。(3) 起動時に service_start を events へ書いていない (M104d の欠落区間の計算と一緒に入れる想定)。
触ったファイル: tools\track\ 全体 (Cargo.lock を含む。target\ は除く)、.gitignore、docs\ai-change-tracking\db-schema.md、plans\m104-ai-change-tracking.md


SELF_EVAL: sub-02 (round 2)
実装: #1: project.rs に project_id_source (normalize_path_key の結果から末尾の区切りを全部落とす。c:\ は落とさない) を足し、project_id はそのハッシュにした。normalize_path_key は C++ と同じ結果のまま。project_id_cases.tsv を 4 列 (入力 / NormalizePathKey / もとの文字列 / projectId) にし、末尾の \、/、複数個の行を足した (2 列目は C++ の実測、4 列目は PowerShell の SHA256)。tests\project_id.rs を 4 列対応にし、末尾の区切りで ID が変わらないテストへ置き換えた。project.rs 冒頭と architecture.md §3 の projectId の説明を直した。#2: StartupReport.failed_steps: Vec<StepFailure { step, reason }> に型を付けた (notes は廃止)。ServiceState と同じ StartupReport から取れる。#3: 進捗表の M104c を「完了」にした。
仕様との差分: なし (round 1 の差分は採用済み)。
検証: cargo test 26 件 PASS (警告なし)、check_rules 0 error。selftest / replay_verify は src\ を触らないので未実行。failed_steps に値が入る経路の単体テストは無い (失敗を作りにくい)。
自己採点: 仕様適合 4、正しさ 4、コード品質 4、テスト 4。
不安・質問: なし
触ったファイル: tools	rack\src\project.rs、tools	rack\src\startup.rs、tools	rack	ests\project_id.rs、tools	rack	ests\project_id_cases.tsv、docsi-change-trackingrchitecture.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-02.md (round 1 と合わせて tools	rack 全体、.gitignore、db-schema.md)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-14)。`cargo test` を planner が実走して 26 件 PASS を確認。受け入れ条件 1〜7、9 は対応するテストがある。
  - must 1: projectId が末尾の区切りで変わる (`tests\project_id_cases.tsv` の `C:\Proj\MyGame` = `64489cc1…` と `C:\Proj\MyGame\` = `33ee953a…`)。仕様 (「NormalizePathKey と同じ結果」) の側の穴で、coder の誤りではない。上の「やること 3」を直した。
  - 仕様に無い追加 5 件 (`events(journal_pos)` の一意索引、`journal_unreadable`、退避の条件、degraded 中のジャーナル追記、PID の無い session の扱い) は採る。serve の終了コードも採る。
  - 質問への回答: (1) `towlower` と "C" ロケールの件は sub-09 の C++ 側で同じ表を使って検出する、でよい。(2) 派生表を events から作り直せるようにする件は sub-03 / sub-04 に取り込んだ。(3) `service_start` / `service_stop` は sub-03 で入れる。(4) 検査の 3〜6 の失敗で記録を続けるのは採る。ただし失敗した段と理由を `ServiceState` と一緒に保持し、sub-05 の `status` で返せるようにしておくこと (should)。
- round 2: VERDICT OK (planner、2026-11-14)。planner が `cargo test` を実走して 26 件 PASS。固定表で、末尾の区切りの有無・個数・混在の 6 行が同じ projectId (`64489cc186743810`)、`C:\` と `C:\\` が同じ、UNC も末尾の有無で同じことを確認。`project_id_source` と `normalize_path_key` が分かれている。nit: `failed_steps` に値が入る経路のテストが無い (sub-05 の `status` で、失敗を差し込めるなら足す)。
