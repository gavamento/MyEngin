# sub-07: MCP の 5 ツール (M104h)

- 依存: sub-22 (順序は sub-06 → sub-22 → sub-07)
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-06 の VERDICT round 3 で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。ユーザーの手作業が要る確認 (受け入れ条件 12) は「未確認」として残し、それ無しでサブを閉じる。

要求 §12、`C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md` §8、§9 (ipc)、ADR-039 決定 4。**MCP の道具は引数を検査して pipe へ取り次ぐだけ。判定は全部サービス側。**

## 守ること (変えてはいけないもの)

- `tools\mcp\Cargo.toml` の `[dependencies]` を増やさない (`serde` / `serde_json` だけ。ADR-032 決定 1)。pipe も起動も標準ライブラリで書く。`tools\track` の crate に依存しない (`client.rs` の `Client` は使えない。同じ手順を書く)。
- 既存の 5 ツール (`search` / `describe` / `verify_job` / `note_read` / `note_append`) の挙動と、`tests\mcp\` の既存の期待値 (`tools_list` を除く)。
- MCP は `.mye\ai_changes\` を**書かない**。読むのは `service.json` だけ。
- 承認・復元の道具は足さない (要求 §12.6)。

## やること

### A. `tools\mcp\src\track_client.rs` (新規)

1. **プロジェクトの決め方**: 既存の `Context::notes_base()` と同じ (`--project` があればそれ、無ければ `--root` / 見つけたリポジトリのルート)。どちらも無ければ、5 つの道具は `isError` (起動のしかたを書いた文面)。
2. **projectId**: 正規化 → 末尾の区切りを落とす (ドライブのルートは除く) → UTF-8 の SHA-256 の先頭 16 桁 (spec §2 D25)。`tools\mcp` には SHA-256 が無い (依存を足せない) ので、**SHA-256 を自前で書く** (約 60 行)。既知のベクタ (空文字列、`abc`、長い入力) と、`C:\HAL\MyEngin\tools\track\tests\project_id_cases.tsv` の全行 (4 列) を `cargo test` で読んで照合する。正規化は `tools\track\src\project.rs` と同じ規則 (絶対化、`/` → `\`、ASCII だけ小文字)。
   - 代わりの案 (採らない): `service.json` から pipe 名を読むだけにして projectId を計算しない。サービスが居ないとき (起動の前) に pipe 名が分からず、別名のパスで起動された古いサービスへつなぐ危険があるので、計算する。ただし接続の後、`hello` の結果の `projectId` が自分の計算と一致することを確かめる。
3. **接続**: `\\.\pipe\mye-track-<projectId>` を `OpenOptions` (read + write) で開く。`ERROR_PIPE_BUSY` (`raw_os_error() == Some(231)`) は短く待って開き直す。pipe が無い (`ERROR_FILE_NOT_FOUND` = 2) ならサービスを起動してやり直す。全体で最大 5 秒 (ipc §6)。
4. **起動**: exe の探し方は ipc §9.5 (環境変数 `MYE_TRACK_EXE` → 自分の exe の隣 → 自分の exe から見た `..\..\..\track\target\release\mye_track.exe`)。**`MYE_TRACK_EXE` が設定されているときはそれだけを使い、ファイルが無ければ他を探さずに失敗する** (検証で「サービス不在」を決定的に作るため。ipc §9.5 にこの意味を書き足す。`tools\track` 側の探し方も同じ意味にそろっているか確かめ、違えば合わせる)。
   - `Command::new(exe).args(["serve", "--project", <root>])`、標準入出力は 3 本とも `Stdio::null()` (MCP の stdio を子が掴むと、クライアントから見て接続が閉じない)。
   - `CommandExt::creation_flags(CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS)` (0x01000000 | 0x00000008)。`raw_os_error() == Some(5)` で失敗したら `CREATE_BREAKAWAY_FROM_JOB` を外してやり直す (arch §3)。
   - 起動した子は待たない (`wait` しない。ハンドルを落とすだけ)。終了コード 3 (既に居る) は見えなくてよい (接続のやり直しで分かる)。
5. **`hello`**: `{"proto": <定数>, "role": "mcp", "token": <service.json の値>, "projectRoot": <絶対パス>, "pid": <自分の PID>}`。`bad_token` は `service.json` を読み直して 1 回だけやり直す。`proto_mismatch` / `wrong_project` / `db_degraded` は `isError`。
   - プロトコル版の定数 `pub const PROTO_VERSION: u32 = 1;` を `track_client.rs` に持つ (規則 9 で `tools\track\src\protocol.rs` と突き合わせる)。
6. **要求と応答**: 1 行 1 JSON、LF。`id` は接続ごとに 1 から。応答の読み取りは 30 秒 (読み取りの op) / 5 秒 (記録の op) で時間切れ (ipc §6)。標準ライブラリの同期の pipe には読み取りの時間切れが無いので、**読み取りを別スレッドで行い `recv_timeout` で待つ**か、同等の方法にする (時間切れのとき、MCP サーバ全体が固まらないこと)。1 行の上限 1 MiB を超える応答は `isError`。
7. **接続の持ち方**: 道具の呼び出しごとに接続してよい (サービスは `mcp` のセッションを接続ではなく PID + 開始時刻で見る。sub-06)。接続を持ち回すなら、切れたときに次の呼び出しでつなぎ直す。

### B. 5 つの道具 (`tools.rs` の `list()` と `call()`、`main.rs` の `mod`)

引数は既存の `reject_unknown` で検査し、結果は既存の `wrap()` の形。サービスのエラーは `wrap(…, true)` (`isError`) に、コードと文面をそのまま入れる。

| 道具 | 引数 | 使う op | 備考 |
|---|---|---|---|
| `change_session_begin` | `provider`、`workspace?` (既定はプロジェクトのルート)、`clientSessionId?`、`isolated?` | `session_begin` (`source = mcp`) | 結果の `sessionId` を、以後の呼び出しで AI が渡す |
| `change_session_end` | `sessionId` | `session_end` | |
| `change_report` | `sessionId`、`operationId?`、`paths`、`beforeHashes?`、`afterHashes?` | `report` | ハッシュは無くてもよい (`report_only` になる)。結果はパスごとの `matched` / `mismatch` / `not_observed` と帰属 |
| `change_list` | `sessionId?`、`path?`、`from?`、`to?`、`review?`、`attribution?`、`changeType?`、`cursor?`、`limit?` | `list` | 一覧に Blob も Hunk の本文も入らない (サービスが入れない) |
| `change_describe` | `revisionId` | `describe` | 本文は合計 64 KiB で切り、切ったら `truncated: true` (既存の `notes` の打ち切りと同じ考え方。今の `describe` は Hunk の要約までなので、通常は切れない。根拠の一覧が長い場合に備える) |

- 道具の説明文 (`description`) には、(1) `change_report` だけでは Verified にならないこと (2) セッションを始めてから編集し、終わったら報告する、という使い方を 1〜2 文で書く。英語 (既存の道具に合わせる。着手時に確かめる)。
- `inputSchema` は既存の道具と同じ書き方。

### C. 規則 (`C:\HAL\MyEngin\tools\check_rules.ps1`)

- 規則 15-c: `$mcpSpawnAllowed` に `'track_client.rs'`。
- 規則 15-d: `$mcpWriteAllowed` に `'track_client.rs' = @('OpenOptions')` **だけ** (spec §2 D9)。`create_dir_all` も `write` も許さない。
- 規則 15 の冒頭コメントの「許可リストは、その経路を足すサブがここを編集して広げる (M87c: verify.rs, M87d: notes.rs)」に M104h を足す。
- 規則 9: `$constGroups` に 1 組 (`label = 'PROTO_VERSION (track) / PROTO_VERSION (mcp)'`、site は `tools\track\src\protocol.rs` と `tools\mcp\src\track_client.rs`。spec §2 D7)。C++ の site は sub-09 で足す。正規表現は `pub\s+const\s+PROTO_VERSION\s*:\s*u32\s*=\s*(\d+)`。
- 規則 15-d の禁止パターンが、SHA-256 の実装や pipe のコードに誤って当たらないことを確かめる (当たったら、コードの側を直す。規則は緩めない)。

### D. 検証のシナリオ

- `tests\mcp\tools_list.expected.ndjson` を撮り直す (道具が 10)。差分を読んでからコミット対象にする。
- `tools\mcp_verify.ps1` に足すシナリオ (サービスを起動しないもの): 起動のとき `MYE_TRACK_EXE` を存在しないパスにして、5 つの道具がそれぞれ `isError` を返し、その後 `search` が動く。引数の不正 (知らない引数、型違い、必須の欠落) が、サービスへ届く前に `isError` になる。`mcp_verify.ps1` に、シナリオごとに環境変数を設定するディレクティブを足す (例 `# env MYE_TRACK_EXE=<値>`)。
- サービスを伴うシナリオ (Editor を起動せずに MCP の操作が記録される) は、サービスの起動と後始末を既に持っている **`tools\track_verify.ps1` の側**に置く: シナリオの中で `mye_mcp.exe` を `--project <一時プロジェクト>` で起動し、JSON-RPC の行を流すディレクティブ (例 `# mcp <行>`) を足す。MCP が自分でサービスを起動する経路と、既に動いているサービスへつなぐ経路の両方。終わったら `role = editor` の `shutdown` でサービスを止める (既存の後始末)。
- `mye_mcp.exe` の検証は release ビルドを使う (`mcp_verify.ps1` と同じ)。`track_verify.ps1` が `tools\mcp\target\release\mye_mcp.exe` を要るので、無ければビルドする段を足すか、`track_verify.bat` の冒頭で `cargo build --release --manifest-path tools\mcp\Cargo.toml` を回す。CI の段 (`ci.yml` の track service) でも通る形にする。

### E. 文書

- `C:\HAL\MyEngin\docs\adr\ADR-032-engine-mcp-server.md` と `C:\HAL\MyEngin\docs\mcp-server.md`: 「道具は 5 つ」→ 10、子プロセスの起動とファイルを開く許可表に `track_client.rs`。**それ以外の行は変えない** (spec §2 D19。`verify_job` の「22 ジョブ」の誤記は直さない)。
- `C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md` §8、§9.5: `MYE_TRACK_EXE` の意味、MCP のクライアントの作法。
- `C:\HAL\MyEngin\docs\test_checklists.md`: 受け入れ条件 12 の手順 (本物の Claude Code から MCP 経由でサービスを起動させ、Claude Code を終了した後に `mye_track.exe` が生きているかを `Get-Process` と `service.json` で見る)。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md`: 進捗表、「実装時に直す既存のもの」の結果。25 行目 (M104g の行) で `tools\track` の `\t` がタブに化けているのを直す (sub-22 の VERDICT の nit)。
- sub-22 の結果: `status` の結果に `watches` (`count` / `fallback`) が増えた。`status` の期待値を持つシナリオを足すときは含める。
- `.mcp.json` は変えない (引数なし = リポジトリのルートがプロジェクトになる。A の 1)。

## やらないこと (このサブでは)

- フック CLI (sub-08)。Editor 側 (sub-09)。
- サービス側の op の追加・変更 (足りないものが見つかったら、実装せず「不安・質問」で返す。ただし `MYE_TRACK_EXE` の意味を `tools\track\src\client.rs` 側でそろえるのは可)。
- `subscribe`、通知。

## 受け入れ条件 (このサブ)

1. `tools/list` が 10 個を返す。既存のシナリオの期待値は `tools_list` 以外 1 バイトも変わらない → `tools\mcp_verify.bat`、`git diff --stat -- tests\mcp` (変わるのは `tools_list.expected.ndjson` と新しいシナリオだけ)。
2. `git diff -- tools\mcp\Cargo.toml` が空 (依存を足していない)。`tools\mcp\Cargo.lock` も変わらない。
3. サービス不在 (`MYE_TRACK_EXE` が存在しないパス) で、5 つの道具がそれぞれ `isError` を返し、5 秒程度で戻り、その後も `search` が動く。プロジェクトの中にファイルが増えない (`expect-untouched`) → `tools\mcp_verify.bat`。
4. 引数の不正が、道具ごとに `isError` になる (知らない引数、型違い、必須の欠落) → `tools\mcp_verify.bat`。
5. **Editor を起動せずに記録される**: サービスが居ない状態から、MCP の `change_session_begin` (MCP がサービスを起動する) → ファイルを変える → `change_report` → `change_list` (`sessionId` で絞る) → `change_describe` → `change_session_end` が通り、変更がそのセッションの `claimed` として返る。`changeCount` が 1 → `tools\track_verify.bat` (要求 §16.2 の 3 番目)。
6. `change_report` だけでは `verified` にならない (ハッシュが両方一致しても `claimed`) → 同上 (要求 §4.3)。
7. 既に動いているサービス (別の経路で起動済み) へ、MCP が二重に起動せずにつながる。MCP が起動したサービスは、`mye_mcp.exe` が終わった後も動いている (同じシナリオの中で、MCP を終わらせた後に `status` が返る) → `tools\track_verify.bat`。これは合成の親での確認で、本物のクライアントの Job の中での生存は条件 12。
8. MCP が起動したサービスが MCP の標準入出力を掴んでいない: `mye_mcp.exe` の標準入力を閉じると、サービスが動いたままでも `mye_mcp.exe` が終了し、検証の側が出力の終わりを読める (固まらない) → `tools\track_verify.bat` (シナリオが終わること自体が確認になる。時間の上限を置く)。
9. 応答が来ないサービス (試験用に、`hello` に答えない偽の pipe サーバを `cargo test` の中で立てる) に対して、道具が時間切れで `isError` を返し、MCP の次の要求が処理される → `cargo test --manifest-path tools\mcp\Cargo.toml`。
10. SHA-256 と projectId: 既知のベクタと `project_id_cases.tsv` の全行が通る → `cargo test --manifest-path tools\mcp\Cargo.toml`。
11. 規則: `tools\check_rules.ps1` が 0 error。`-McpSourceDir` を使った一時コピーで、(a) `track_client.rs` に `fs::write` を足すと 15-d で落ちる (b) 別のファイルに `Command::new` を足すと 15-c で落ちる (c) `track_client.rs` の `PROTO_VERSION` を 2 にすると規則 9 で落ちる (規則 9 は `-McpSourceDir` を見ないので、落とし方は coder が決める。確かめられなければ理由を書く) → 実行結果。
12. **手動確認 (ユーザー。未確認として残す)**: 本物の Claude Code を終了した後にサービスが生き残るか。手順が `docs\test_checklists.md` にある。coder は実行しない (自分を動かしているクライアントを終わらせることになる)。結果がどちらでも設計は成り立つ (生き残らなければ `service_down` の欠落)。
13. Phase 1〜2 の検証が全部通る: `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat` (3 回。後にプロセスが残らない)、`tools\mcp_verify.bat`。`git diff --stat ca6a3991 -- src` が空 (空なら selftest / replay_verify は回さない。回していないことを書く)。
14. 文書: ADR-032 と `docs\mcp-server.md` の差分が、道具の数と許可表の行だけ → `git diff`。

## 検証コマンド

- `cargo test --manifest-path tools\mcp\Cargo.toml`
- `tools\mcp_verify.bat`
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)
- `cargo test --manifest-path tools\track\Cargo.toml`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat -- tools\mcp\Cargo.toml tools\mcp\Cargo.lock tests\mcp`、`git diff --stat ca6a3991 -- src`

## 実装メモ (coder が追記)

## フィードバック履歴
