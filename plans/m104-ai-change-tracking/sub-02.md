# sub-02: `tools\track` の骨組み、DB、Blob、ジャーナル (M104c)

- 依存: sub-01
- 状態: 未着手
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
3. projectId: 正規化したプロジェクトルートの SHA-256 の先頭 16 桁 (arch §3)。正規化は `C:\HAL\MyEngin\src\Engine\Platform\PathUtil.h` の `NormalizePathKey` と**同じ結果**になること。C++ と Rust で同じ入力から同じ projectId が出ることを、固定の入出力の表でテストする (sub-09 が同じ表を C++ 側で使う)。
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
8. projectId の固定表 (大文字小文字、末尾の区切り、`/` と `\`、日本語を含むパス) が通る → `cargo test`。
9. 既存の検証が変わらない → `pwsh -NoProfile -File tools\check_rules.ps1` が 0 error。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `cargo build --release --manifest-path tools\track\Cargo.toml`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git status --short` (`tools\track\target\` が出ないこと)

## 実装メモ (coder が追記)

## フィードバック履歴
