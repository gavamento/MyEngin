# sub-08: フック CLI (M104i)

- 依存: sub-07
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-07 の VERDICT で planner が書き直した)。Phase 2 の最後のサブ。
- 質問の運用: 推奨で進め、Notion に後回し。着手前のユーザー確認は無い。**本物の Claude Code の入力の記録 (受け入れ条件 13) はユーザーの手作業なので「未確認」として残し、それ無しでサブを閉じる。ユーザーの `.claude\settings.json` は変えない。リポジトリの `.claude\` の下も変えない。**

`C:\HAL\MyEngin\docs\ai-change-tracking\attribution-rules.md` §6.1 (attr)。入力の形は spec §2 D14 (公式: https://code.claude.com/docs/en/hooks 、2026-11-14 に planner が取得) と `C:\HAL\MyEngin\docs\ai-change-tracking\prototype-results.md` の試作 5。coder は外部のページを読めないので、項目名はこの 2 つを正とする。

## 守ること

- **終了コードは常に 0。標準出力へ 1 バイトも出さない。** 終了コード 2 は PreToolUse で道具を止め、標準出力の JSON は Claude Code が解釈する。サービス不在、時間切れ、入力が読めない、プロジェクトの外、内部の失敗 (panic を含む) のどれでも。失敗の説明は標準エラーへ 1 行だけ。panic は `catch_unwind` と、panic フックの差し替え (標準エラーへ出すだけ) で 0 に落とす。`main` の hook の経路に `std::process::exit` を 0 以外で呼ぶ箇所を作らない。
- **AI を待たせない。** フック全体で 5 秒 (定数) を超えない。超えそうなら途中でやめて 0 で返す。
- **誤った場所にサービスを立てない** (下の A)。

## やること

### A. プロジェクトの決め方 (planner の裁定。spec §2 D36)

フックの入力の `cwd` は、プロジェクトのルートとは限らない (サブディレクトリで作業していることがある)。`cwd` をそのままルートにしてサービスを起動すると、サブディレクトリを別のプロジェクトとして追跡し、そこでベースラインが走る。次の順で決める。

1. `--project <root>` が付いていれば、それ。
2. 無ければ、対象のファイル (`tool_input.file_path`。無ければ `cwd`) から親へたどり、**`.mye\ai_changes\service.json` がある最初のディレクトリ** (= サービスが動いたことのあるプロジェクト)。
3. どちらも無ければ、何もしないで 0 で返す (標準エラーに 1 行)。

サービスを**起動する**のは 1 のときだけ。2 で見つけたプロジェクトは、サービスが動いていれば接続し、動いていなければ何もしない (欠落は次の起動の `service_down` に入る)。設定の手順 (D) は 1 の形を勧める。

### B. `mye_track.exe hook <pre|post|session-start|session-end> [--project <root>]`

`tools\track\src\hook.rs` (新規) と `main.rs`。pipe のクライアントは `tools\track\src\client.rs` の `Client` (`role = hook`)。

| サブコマンド | 入力 (標準入力の JSON) | 送る op |
|---|---|---|
| `pre` | `session_id`、`cwd`、`tool_name`、`tool_input`、`tool_use_id` | `session_begin` (`clientSessionId` = `session_id`、`provider = claude`、`source = terminal`、`workspace` = プロジェクトのルート。既にあればそれが返る) → `op_begin` (`clientSessionId`、`clientOpId` = `tool_use_id`、`operationType`、`targets`、`declaredEdit?`) |
| `post` | 同上 + `tool_response` | `op_end` (`clientSessionId` + `clientOpId`、`result`) |
| `session-start` | `session_id`、`cwd` | `session_begin` |
| `session-end` | `session_id` | `session_end` (`clientSessionId`) |

- 対応する `tool_name` は `Write` (`file_path`、`content`) と `Edit` (`file_path`、`old_string`、`new_string`、`replace_all`)。それ以外は何も送らず 0 で返す (標準エラーにも出さない。`matcher` で絞られていない設定でも静かに動く)。
- `declaredEdit`: Edit → `{"kind":"replace","oldString":..,"newString":..,"replaceAll":bool}`、Write → `{"kind":"write","content":..}`。要求の 1 行が 1 MiB を超える場合は `declaredEdit` を付けずに `op_begin` する (Claimed 止まり)。フックは内容のハッシュを計算しない (サービスが `op_begin` / `op_end` で対象を自分で読む。sub-06)。
- `operationType`: Edit → `edit`、Write → `write`。
- `file_path` がプロジェクトのルートの外なら何も送らない。
- `post` の `result`: `tool_response` から失敗が分かれば `failed`、それ以外は `ok`。本物の `tool_response` の形は未確認 (D14 では Write の例が `{"filePath":..,"type":"create"}`) なので、判定できないものは `ok` にする。
- 入力の `session_id` / `tool_use_id` が無い・文字列でない場合は何も送らない。知らないキーは無視する (公式の入力は項目が増えうる。pipe の `bad_args` とは方針が違う。理由をコメントに書く)。
- 環境変数 `MYE_TRACK_HOOK_DUMP=<ファイル>` があるときだけ、標準入力をそのまま 1 行にして追記する (本物の入力を記録するための口。D)。追記に失敗しても 0。**このファイルはプロジェクトの外でも書く唯一の例外**なので、既定では無効で、設定したときだけ働くことをコードと文書に書く。

### C. サービスの起動 (A の 1 のときだけ)

- exe の探し方: `MYE_TRACK_EXE` が設定されていればそれだけ (無ければ失敗)。無ければ自分自身 (`std::env::current_exe()`)。フックは `mye_track.exe` 自身なので、隣を探す必要は無い。
- 起動の作法は sub-07 の `tools\mcp\src\track_client.rs` (`find_track_exe`、`start_service`、`StdHandlesHidden`) と同じ: `serve --project <root>`、標準入出力は 3 本とも null、`CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS` (アクセス拒否なら BREAKAWAY を外す)、**起動のあいだだけ自分の標準入出力のハンドルを継承不可にする** (sub-07 の実測: `Stdio::null()` を指定しても、継承可能なハンドルが子へ渡り、サービスが親の標準出力を掴んだ。フックの標準出力は Claude Code が読むので、掴まれるとフックが終わっても Claude Code が待ち続ける)。こちらは `windows-sys` を使える。
- 起動した後の接続のやり直しは、全体の 5 秒の内側。間に合わなければ 0 で返す (その操作は欠落し、サービスは立ち上がるので次の操作から記録される)。
- `mye_track client` サブコマンドなど、既存の起動の経路があれば同じ関数を使う (2 通りの実装を持たない)。

### D. 設定の手順の文書

新規 `C:\HAL\MyEngin\docs\ai-change-tracking\hooks-setup.md`。

- Claude Code の設定例 (`.claude\settings.json` か `.claude\settings.local.json` に利用者が書く。**このサブでは書かない**): `hooks.PreToolUse` と `hooks.PostToolUse` に `matcher: "Edit|Write"`、`type: "command"`、`command` は `mye_track.exe` の絶対パス + `hook pre --project <プロジェクトのルート>` (post も同様)、`timeout` を明示 (10 秒)。任意で `SessionStart` / `SessionEnd`。
- `mye_track.exe` の場所: 開発機は `bin\x64\Release\mye_track.exe` (`tools\build_track.bat`)。配布物では Editor の隣 (sub-20 で 1 節足す)。
- フックを設定すると何が変わるか (宣言と一致した編集が Verified になる。設定しなければ Claimed / Inferred / Unknown)。設定しなくても追跡は動く。
- 本物の入力の記録の取り方 (`MYE_TRACK_HOOK_DUMP`)。取れた記録は `tests\track\fixture\` の固定入力にできる。
- 確認済み / 未確認の区別: 入力の項目名は公式文書で確認済み (2026-11-14)。本物の入力での実走、`tool_response` の形、Edit の CRLF や引用符の扱い、MultiEdit / NotebookEdit、Codex CLI / Gemini CLI は未確認。
- 設定例の JSON は、文書の中のコードブロックをそのまま `ConvertFrom-Json` で読めること (受け入れ条件 9)。

### E. sub-06 / sub-07 から持ち越した小さい直し

1. attr §5 に、`verify_gap` の Revision では `overlap` を作らないこと (時刻の窓が使えないので、内容の一致が 1 つの操作にだけ成り立つことで絞る) が書いてあるか確かめ、無ければ足す (sub-06 round 3 の nit)。
2. `tools\track_verify.ps1` が起動時に `cargo build --release --manifest-path tools\mcp\Cargo.toml` を回すと、開発機で動いている MCP サーバ (`tools\mcp\target\release\mye_mcp.exe` を Claude Code が使っている) と exe の置き換えで衝突する (sub-07 で実際に起きた)。**検証用の `mye_mcp.exe` は別の出力先にビルドする** (`--target-dir cache\track_verify\mcp_target` など、`.gitignore` 済みの場所)。`tools\mcp\target\release\mye_mcp.exe` を検証のために置き換えない・名前を変えない。`tools\mcp_verify.ps1` の既存のビルドの場所は変えない (既存の道具の挙動)。
3. `tests\mcp\verify_job.expected.ndjson` が以前から古い (planner が確認: 最後の更新は `bc8f5d4c` = M87c。その後 `tools\mcp\src\verify.rs` の JOBS に `2d` / `feel` / `field` が足されたが撮り直されていない)。このため `tools\mcp_verify.bat` が 1 シナリオ FAIL のまま。**撮り直す。ただし、差分がジョブ名 3 つの追加だけであることを `git diff` で確かめてから** (それ以外の差が出たら撮り直さず、差を「不安・質問」で返す)。`--update` は全シナリオを塗り潰すので、対象を絞るか、撮り直した後に他のファイルの差分が無いことを確かめる。
4. `tools\mcp\target\release\mye_mcp.exe.inuse-15352` (sub-07 で coder が名前を変えて逃がした、ユーザーのセッションが使用中の古い exe) には触らない (消さない・戻さない)。

## やらないこと (このサブでは)

- Codex CLI / Gemini CLI のフック対応 (spec §3 後回し)。MultiEdit / NotebookEdit / Bash による編集の解釈。
- ユーザーの設定ファイルの変更、本物の Claude Code でのフックの実走。
- サービス側の op の変更 (足りないものが見つかったら「不安・質問」で返す)。`tools\mcp\src` の変更。

## 受け入れ条件 (このサブ)

1. **宣言と一致すれば Verified**: D14 の形の `pre` → 実際にファイルを編集 → `post` で、その変更が `ai` / `verified` (`declared_edit_match`) になり、セッションの `list` に出る。Edit (`replace_all` 真・偽)、Write (新規と上書き) の 4 通り → `tools\track_verify.bat` (`# hook pre|post <JSON>` のディレクティブを足す。フックは別プロセスとして起動する) + `cargo test`。
2. `pre` と `post` の間に人が同じファイルへ別の変更を足すと `verified` にならない。`pre` の `old_string` が実際の内容に無い場合も `verified` にならない → 同上。
3. **どの失敗でも終了コード 0・標準出力が空・5 秒以内**: サービス不在 (`--project` なし)、`MYE_TRACK_EXE` が存在しないパス + `--project` あり、壊れた JSON、空の標準入力、`tool_name` が `Bash`、`session_id` 無し、プロジェクトの外の `file_path`、応答しない pipe (偽のサーバ)、1 MiB を超える `content` → `cargo test` (実際に exe を起動して、終了コード・標準出力の長さ・所要時間を測る)。
4. コードの hook の経路に、0 以外の終了コードで終わる箇所が無い (`exit(` と `ExitCode` を grep して 1 つずつ確かめる)。panic を起こす差し込み (試験用の環境変数など) でも 0 → `cargo test` + レビュー。
5. **プロジェクトの決め方**: (a) `--project` なしで、サブディレクトリを `cwd` にしたフックが、親で動いているサービスへつながる (b) `--project` なしで、サービスが動いたことの無い場所では何も起動されず、`.mye` が作られない (c) `--project` ありでサービス不在なら起動され、次の操作から記録される → `cargo test` + `tools\track_verify.bat`。
6. フックが起動したサービスが、フックの標準出力・標準入力を掴んでいない (フックの出力をパイプで読んでいる親が、フックの終了ですぐ読み終わる。時間の上限を置く) → `cargo test`。
7. 1 MiB を超える `content` の Write は、`declaredEdit` なしで記録され `claimed` になる (落ちない・欠落しない) → `cargo test`。
8. `MYE_TRACK_HOOK_DUMP` を設定すると標準入力がそのまま追記され、設定しなければどこにも書かれない。追記先が書けなくても 0 → `cargo test`。
9. `hooks-setup.md` があり、設定例の JSON がそのまま読める。確認済みと未確認が区別して書いてある → `ConvertFrom-Json` での確認と目視。
10. `tools\mcp_verify.bat` が全シナリオ PASS (E の 3 の後)。`tests\mcp\verify_job.expected.ndjson` の差分がジョブ名の追加だけ → 実行結果と `git diff`。
11. `tools\track_verify.bat` が `tools\mcp\target\release\` を書き換えない (実行の前後で `mye_mcp.exe` の更新時刻とハッシュが同じ)。MCP サーバが動いている開発機でも通る → 実行結果。
12. **A20 (Phase 2 の横断)**: `cargo test --manifest-path tools\track\Cargo.toml` (5 回)、`cargo test --manifest-path tools\mcp\Cargo.toml`、`tools\track_verify.bat` (3 回。後に `mye_track.exe` が残らない)、`tools\mcp_verify.bat`、`tools\check_rules.ps1` (0 error) が通る。`git diff --stat ca6a3991 -- src` が空 (空なら selftest / replay_verify は回さない。回していないことを書く)。作業ツリーを書かない検査がこのサブの試験でも成り立つ。
13. **手動確認 (ユーザー。未確認として残す)**: 本物の Claude Code でフックを 1 回動かし、`MYE_TRACK_HOOK_DUMP` で入力を記録して、項目名と `tool_response` の形、Edit の後の変更が `verified` になるかを確かめる。手順が `hooks-setup.md` と `C:\HAL\MyEngin\docs\test_checklists.md` にある。coder は実行しない。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (5 回)、`cargo test --manifest-path tools\mcp\Cargo.toml`
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)、`tools\mcp_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git diff --stat ca6a3991 -- src`、`git diff -- tests\mcp\verify_job.expected.ndjson`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要約)

- 実装: `tools\track\src\hook.rs` (新規。`run` は値を返さず、作業を別スレッドで回して 5 秒で切り上げる。panic は `catch_unwind` + 1 行の panic フック)、`tools\track\src\launch.rs` (新規。`find_track_exe` / `StdHandlesHidden` / `start_service`。windows-sys 版)、`main.rs` (`hook` の枝は `ExitCode::SUCCESS` のみ)、`lib.rs`。
- 試験: `tools\track\tests\hook.rs` (14 件。exe を別プロセスで起動)、`tests\track\18〜22_hook_*.ndjson` (`# hook` / `# hook-noproject` / `# exists` を `track_verify.ps1` に足した)。
- E 節: (1) attribution-rules.md §5 に verify_gap の overlap の記述が無かったので足した (2) `track_verify.ps1` は `cache\track_verify\mcp_target` へ mye_mcp.exe をビルドする (3) `verify_job.expected.ndjson` は `2d, feel, field,` の 6 か所の挿入だけの差だったので撮り直した (4) `.inuse-15352` と `tools\mcp\target\release\` には触っていない (前後でハッシュと更新時刻が同じ)。
- 文書: `docs\ai-change-tracking\hooks-setup.md` (新規)、ipc-contract.md §9.5 にフック CLI の段落、`docs\test_checklists.md` に M104i (受け入れ条件 13 の手順)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、`tools\mcp\target\release\mye_mcp.exe` のハッシュが検証の前後で同じ (475E7A0D…)、`.inuse-15352` も同じ (07ADCD4C…)、`.claude\` と `tools\mcp\src` に差分なし、`git diff --stat ca6a3991 -- src` は空。`tests\mcp\verify_job.expected.ndjson` の差分はジョブ名の追加だけ。planner の実走: `cargo test --manifest-path tools\track\Cargo.toml` 187 件通過、`tools\track_verify.bat` 全シナリオ PASS、`tools\mcp_verify.bat` 全シナリオ PASS (`verify_job` を含む)。
  - 追加の判定: サービスの起動を `pre` / `session-start` に限る (`post` / `session-end` では起動しない)、`MYE_TRACK_HOOK_TEST_PANIC`、`--project` つきでもプロジェクトの外の `file_path` では何もしない、入力の上限 64 MiB、設定例の置換記号、は全部採る。
  - 質問への回答: (1) 起動を `pre` / `session-start` に限るのは了承。終える相手の記録が無いのにベースラインだけ走らせる理由が無い。(2) `tool_response` の失敗の形と (3) 設定例の `/` 区切りのパスは、本物の実走 (受け入れ条件 13) まで未確認として残す。(4) `mcp_verify.ps1` のビルドの場所は変えない。`tools\mcp\src` を変えた後に回すときは、MCP サーバを使っているセッションを閉じてから。
  - nit (持ち越し): `bad_token` の再試行、`hello` の `db_degraded` / `proto_mismatch` の経路の試験が MCP とフックの両方に無い。`tests\hook.rs` の時間のしきい値 (8 秒) は CI で余裕が小さいかもしれない (最初の push で見る)。