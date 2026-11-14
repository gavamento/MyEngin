# sub-07: MCP の 5 ツール (M104h)

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子 + Phase 1 の結果** (2026-11-15、sub-05 の VERDICT で見直し。sub-06 の VERDICT でもう一度見直す)。
- 質問の運用 (2026-11-15 から): 推奨で進め、Notion に後回し。このサブのユーザーの手作業 (受け入れ条件 9) は「未確認」として残し、それ無しでサブを閉じる。

## やること

要求 §12 と ipc §8。MCP の道具は引数を検査して pipe へ取り次ぐだけで、判定はサービス側 (ADR-039 決定 4)。

1. `C:\HAL\MyEngin\tools\mcp\src\track_client.rs` (新規): pipe クライアント (接続 → 無ければサービスを起動 → 最大 5 秒待ってやり直す。ipc §6)、`hello` (`role = mcp`)、5 ツールの実装。**`tools\mcp` の依存は増やさない** (`serde` / `serde_json` だけ。pipe は標準ライブラリで開く。ADR-032 決定 1)。
   - sub-01 の結果 (2026-11-14 の見直し): `tools\mcp` は `windows-sys` を足さないので、`ERROR_PIPE_BUSY` は `raw_os_error() == Some(231)` を見て、短く待って開き直す (`WaitNamedPipeW` は使わない。全体の 5 秒の内側)。
   - サービスの起動は `CommandExt::creation_flags` で `CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS` (0x01000000 | 0x00000008)。`raw_os_error() == Some(5)` (`ERROR_ACCESS_DENIED`) で失敗したら `CREATE_BREAKAWAY_FROM_JOB` を外して起動し直す (arch §3)。標準入出力は継がせない (`Stdio::null()`。MCP の stdio を子が掴むと、クライアントから見て接続が閉じない)。
   - `mye_track.exe` の探し方は sub-05 で確定したもの。
   - sub-05 の結果 (2026-11-15): pipe の名前、`hello` の形、トークン (`<project>\.mye\ai_changes\service.json`)、exe の探し方 (環境変数 `MYE_TRACK_EXE` → 自分の exe の隣 → `..\..\..\track\target\release\mye_track.exe`) は `C:\HAL\MyEngin\docs\ai-change-tracking\ipc-contract.md` §9 が正。`tools\track\src\client.rs` の `Client` は別の crate なので使えない (`tools\mcp` は依存を足さない)。同じ手順を標準ライブラリだけで書く。
   - サービスの起動引数は `serve --project <root>`。二重起動は終了コード 3 で分かる (既に居る = 接続をやり直す)。
   - sub-06 の決定: MCP の接続は `role = mcp`。`change_session_begin` は `source = mcp`。MCP は要求ごとに接続してよい (サービスはセッションを接続ではなく PID + 開始時刻で見る)。`change_report` はハッシュが無くても受ける (`report_only` になる)。`isolated` は道具の引数に出す (既定は偽)。
   - シナリオは `tools\track_verify.ps1` のディレクティブ (`# write` / `# wait-recorded` など) を使える形にするか、`mcp_verify.ps1` に同じ仕組みを足すかを coder が選ぶ。
2. `tools.rs` の `list()` と `call()` に 5 つ足す。`main.rs` に `mod` 宣言。引数の検査は既存の `reject_unknown` の方針、結果は既存の `wrap()` の形。
3. サービス不在・時間切れ・`proto_mismatch` などは `isError` の結果で返す (MCP サーバ自体は落ちない、他の 5 ツールは動く)。
4. `change_describe` の本文は合計 64 KiB で切り `truncated` を示す (ipc §4.3)。`change_list` はページング。
5. `check_rules.ps1`: 規則 15-c の `$mcpSpawnAllowed` に `track_client.rs`、規則 15-d の `$mcpWriteAllowed` に `'track_client.rs' = @('OpenOptions')` **だけ** (spec §2 D9)。規則 9 の `$constGroups` に `tools\track\src\protocol.rs` と `tools\mcp\src\track_client.rs` の 2 site の組 (spec §2 D7。C++ は sub-09)。規則 15 の冒頭コメントの許可の経緯にも 1 行足す。
6. `tests\mcp\tools_list.expected.ndjson` を撮り直す。新しい道具のシナリオ (サービス不在で `isError`、引数の不正) を `tests\mcp\` に足す。
7. 文書: `C:\HAL\MyEngin\docs\adr\ADR-032-engine-mcp-server.md` と `C:\HAL\MyEngin\docs\mcp-server.md` の「道具は 5 つ」と許可表の記述。**それ以外の行は変えない** (spec §2 D19)。

## やらないこと (このサブでは)

- 承認・復元の道具 (要求 §12.6。足さない)。
- 既存 5 ツールの挙動変更。`verify_job` の「22 ジョブ」の誤記の修正 (別件)。
- MCP が `.mye\ai_changes\` を読み書きすること。`service.json` のトークンを**読む**のは許す (書かない)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\mcp\src\track_client.rs` (新規)、`tools.rs`、`main.rs`
- `C:\HAL\MyEngin\tools\check_rules.ps1`
- `C:\HAL\MyEngin\tests\mcp\`、`C:\HAL\MyEngin\tools\mcp_verify.ps1` (サービスを伴うシナリオを足すなら。`track_verify` 側に置く方が単純なら、そちらへ)
- 上の文書 2 本、計画の進捗表

## 受け入れ条件 (このサブ)

1. `tools/list` が 10 個を返す。既存のシナリオの期待値は `tools_list` 以外、1 バイトも変わらない → `tools\mcp_verify.bat`、`git diff --stat -- tests\mcp`。
2. サービスが居ないプロジェクトで 5 つの道具がそれぞれ `isError` を返し、その後も `search` が動く → `tools\mcp_verify.bat`。
3. Editor を起動せずに、MCP の `change_session_begin` → (ファイルを変える) → `change_report` → `change_list` → `change_describe` → `change_session_end` が通り、変更がそのセッションの Claimed として返る → シナリオ (`mcp_verify` か `track_verify`) (要求 §16.2 の 3 番目)。
4. `change_report` だけでは Verified にならない → 同上 (要求 §4.3)。
5. `Cargo.toml` の `[dependencies]` が変わっていない → `git diff -- tools\mcp\Cargo.toml` が空。
6. 規則 15-c / 15-d が通る。`track_client.rs` の一時コピーに `fs::write` を仕込むと 15-d で落ち、別のファイルに `Command::new` を仕込むと 15-c で落ちる (`-McpSourceDir` を使う) → `tools\check_rules.ps1`。
7. 規則 9 の新しい組が通り、片方の定数を変えた一時コピーで落ちる → 同上。

8. MCP が起動したサービスが、MCP の標準入出力のハンドルを持っていない (MCP のプロセスを終わらせると、親から見てパイプが閉じる) → シナリオか `cargo test`。
9. **手動確認 (ユーザー。coder は自分を動かしている Claude Code を終わらせられない)**: 本物の Claude Code から MCP 経由でサービスを起動させ、Claude Code を終了した後に `mye_track.exe` が生きているか、`service.json` と `Get-Process` で見る手順を `C:\HAL\MyEngin\docs\test_checklists.md` に書く。結果は「未確認」として報告し、ユーザーが確かめたら arch §3 の「未検証」を置き換える。どちらの結果でも設計は成り立つ (生き残らなければ `service_down` の欠落になる)。

## 検証コマンド

- `tools\mcp_verify.bat`
- `tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
