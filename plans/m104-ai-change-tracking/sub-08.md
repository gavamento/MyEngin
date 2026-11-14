# sub-08: フック CLI (M104i)

- 依存: sub-07
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子 + Phase 1 の結果** (2026-11-15、sub-05 の VERDICT で見直し)。Phase 2 の最後のサブ。
- 質問の運用 (2026-11-15 から): 推奨で進め、Notion に後回し。本物の Claude Code の入力の記録 (やること 8) はユーザーの手作業なので「未確認」として残し、それ無しでサブを閉じる。**ユーザーの `.claude\settings.json` は変えない。**
- sub-06 の決定 (spec §2 D31、D32): フックは pre と post が別のプロセス。pre は `session_begin` (`clientSessionId` = `session_id`。既にあればそれが返る) → `op_begin` (`clientOpId` = `tool_use_id`、`declaredEdit`)。post は `op_end` を `clientSessionId` + `clientOpId` で送る。サービスは `op_begin` と `op_end` で対象を自分で読むので、フックは内容のハッシュを計算しない。1 行の上限 (1 MiB) を超える宣言 (大きい `content`) は送らず、`declaredEdit` なしで `op_begin` する (Claimed 止まり)。pipe のクライアントは `tools\track\src\client.rs` の `Client` を使う。

## やること

attr §6.1。入力の形は spec §2 D14 (公式: https://code.claude.com/docs/en/hooks 、2026-11-14 取得) と sub-01 の結果の文書。着手時に公式ページをもう一度確かめ、項目名が変わっていたら結果の文書を直してから実装する。

1. `mye_track.exe hook pre` / `hook post`: 標準入力の JSON を読み、pipe (`role = hook`) で `op_begin` / `op_end` を送って終わる。
2. 対応する道具は Write (`file_path`、`content`) と Edit (`file_path`、`old_string`、`new_string`、`replace_all`)。それ以外の `tool_name` は何もせず 0 で返る。pre と post は `tool_use_id` で対にする。
3. セッションは `session_id` を `clientSessionId` として結び付ける。対応する `session_begin` が無ければフックが登録する (`provider = claude`、`source = terminal`、`workspace = cwd`)。
4. **終了コードは常に 0。標準出力へ何も出さない** (spec §4.1)。サービス不在、時間切れ (5 秒)、入力が JSON でない、プロジェクトの外のパス、のどれでも。失敗は標準エラーへ 1 行だけ。
5. フックはサービスを起動するか: 推奨は「pipe が無ければ起動を試みるが、待つのは全体の 5 秒の内側」。sub-01 の試作 6 の結果 (フックの子として起動したサービスが生き残るか) で決める。
6. 文書: Claude Code の設定の手順 (`.claude\settings.json` の `hooks.PreToolUse` / `PostToolUse`、`matcher = "Edit|Write"`、`timeout` を明示)。置き場所は `C:\HAL\MyEngin\docs\ai-change-tracking\` に 1 本足すか `docs\mcp-server.md` の節にするかを見直しで決める。**リポジトリの `.claude\settings.json` は変えない** (利用者の設定。手順を書くだけ)。

7. sub-01 の結果 (2026-11-14 の見直し): 宣言の適用は試作 5 の規則をそのまま使う (`old_string` の出現が 0 件、または 2 件以上で `replace_all` が偽なら「当てられない」。`replace_all` が真なら全置換。行末は変換しない)。フック CLI は `mye_track.exe` の中なので `windows-sys` を使える (`WaitNamedPipeW` で `ERROR_PIPE_BUSY` を再試行)。
8. **本物の入力の実測**: 試作の入力は文書どおりに作ったもので、本物の Claude Code が渡すキー名・パスの形・`tool_response`・CRLF や引用符の扱いは未確認。`mye_track.exe hook pre|post` に、環境変数 (例 `MYE_TRACK_HOOK_DUMP=<ファイル>`) があるときだけ標準入力をそのまま追記する口を足し、利用者が自分の設定でフックを 1 回動かして記録を取れるようにする。**coder はユーザーの `.claude\settings.json` を変えない。** 記録の取り方を手順の文書に書き、実測は「未確認」として報告する。司会がユーザーに記録を取ってもらえた場合は、それを `tests\track\fixture\` の固定入力にする。

## やらないこと (このサブでは)

- Codex CLI / Gemini CLI のフック対応 (spec §3 後回し)。
- MultiEdit / NotebookEdit / Bash による編集の解釈 (Claimed / Inferred でよい)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\hook.rs` (新規)、`main.rs`、`tests\hook.rs`、`C:\HAL\MyEngin\tests\track\`
- 文書と計画の進捗表

## 受け入れ条件 (このサブ)

1. D14 の形の pre → 実際にファイルを編集 → post で、その変更が `ai` / `verified` (`declared_edit_match`) になる。Write と Edit、`replace_all` の有無 → `cargo test` + `tools\track_verify.bat`。
2. pre と post の間に人が同じファイルへ別の変更を足すと Verified にならない → `cargo test`。
3. サービス不在、壊れた JSON、空の標準入力、知らない `tool_name`、プロジェクトの外のパス、の全部で終了コード 0、標準出力が空、5 秒以内 → `cargo test` (実際に exe を起動して測る)。
4. どの経路でも終了コード 2 を返さない (コードの全 `exit` を確かめる) → レビュー + テスト。
5. 設定の手順の文書があり、書いてある設定例の JSON が構文として正しい → 文書の確認。
6. A20 (Phase 2 の横断): `tools\check_rules.ps1` が 0 error、`tools\mcp_verify.bat` と `tools\track_verify.bat` が通る。`git diff --stat ca6a3991 -- src` が空 (空なら selftest と replay_verify は回さなくてよい。回していないことを書く)。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `tools\track_verify.bat` / `tools\mcp_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
