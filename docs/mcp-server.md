# エンジン MCP サーバ (mye_mcp)

AI クライアント (Claude Code など) から、エンジンの索引検索・検証ジョブの実行・プロジェクトのメモを呼ぶ開発ツール。
エンジン本体 (sim / Runtime / Editor) には触らない。Rust の独立 crate で、依存は `serde` / `serde_json` だけ。
設計の正本は [plans\エンジンMCPサーバ.md](../plans/エンジンMCPサーバ.md)、判断の記録は [ADR-032](adr/ADR-032-engine-mcp-server.md)。

CI にも `Editor.exe --selftest` にも入れていない。MCP を触ったときに `tools\mcp_verify.bat` を手で回す。

## ビルド

```
cargo build --release --manifest-path tools\mcp\Cargo.toml
```

出力は `tools\mcp\target\release\mye_mcp.exe` (gitignore)。`cargo` (Rust stable) が要る。
検索には索引 `cache\engine_index.json` も要る。無ければ `pwsh -NoProfile -File tools\gen_engine_index.ps1`
(Release か Debug の `Editor.exe` が先にビルドされている必要がある。両方あれば Release を使う)。索引は自動では作り直さない。
ソースより古いと検索結果に `stale: true` が付くので、そのときに作り直す。

## Claude Code への登録

リポジトリ直下の [.mcp.json](../.mcp.json) がコミット済みで、サーバ名は `mye-engine`、
コマンドは `tools/mcp/target/release/mye_mcp.exe`。

- 初回は承認が要る。リポジトリのルートで `claude` を起動すると承認を求められる。承認前の `claude mcp list` は
  `mye-engine: tools/mcp/target/release/mye_mcp.exe  - ⏸ Pending approval (run claude to approve)` と出し、サーバは起動しない。
- **Claude Code はリポジトリのルートで起動する。** `.mcp.json` のコマンドの相対パスはクライアントのカレントディレクトリが起点。
  `tools\` から `--mcp-config` で別の場所の `.mcp.json` を絶対パス指定して起動すると、接続が失敗する
  (`CONNECTION_CLOSED`、サーバ側 stderr は cmd の「指定されたパスが見つかりません」)。
  絶対パスを `.mcp.json` に書くと他人の環境で壊れるので書かない。
  なお `claude mcp list` は上のディレクトリへ辿って `.mcp.json` を見つけるので、サブディレクトリからでも同じ表示になる
  (承認済みのプロジェクトスコープでサブディレクトリから起動した場合は未確認)。
- 接続の時代: Claude Code 2.1.300 は MCP 2026-07-28 (modern) で繋がる。modern の探りで落ちると legacy
  (2025-11-25 など) で繋ぎ直す作りで、サーバは両方に答える。どちらで繋がったかはサーバの stderr
  (`[mye_mcp] <method> era=modern|legacy`) に残る。

### 未ビルドのとき

exe が無いと、Claude Code は起動時にこのサーバだけを失敗扱いにする。Claude Code 本体は止まらず、他の機能は使える。

- 見え方: セッションの MCP の状態は `mye-engine` が `failed`。デバッグログに
  `Connection failed after ...ms (CONNECTION_CLOSED): Connection closed`。
- サーバ側 stderr は cmd の「'tools\mcp\target\release\mye_mcp.exe' は、内部コマンドまたは外部コマンド、操作可能なプログラムまたはバッチ ファイルとして認識されていません。」。
- 直し方は上のビルドの 1 行を実行して、Claude Code を起動し直す。

### notes の書き先を別プロジェクトにしたいとき (個人設定)

エンジンのリポジトリで作業するときは、メモは `<リポジトリ>\.mye\notes` に溜まる。外部プロジェクト (三校など) の現在地を
そのプロジェクトのフォルダに置きたいときは、`--project` を付けた個人設定を足す。共有の `.mcp.json` は変えない。

```
claude mcp add --scope local mye-engine -- C:\HAL\MyEngin\tools\mcp\target\release\mye_mcp.exe --root C:\HAL\MyEngin --project D:\games\sanko
```

`--project` に渡すフォルダは存在している必要がある (無いと `note_*` は `isError`。フォルダは作らない)。
同名のサーバがプロジェクトスコープにもあるときの優先は Claude Code の仕様に従う (local が優先のはずだが、この設定では未確認)。

## 道具

結果は `content[0].text` (JSON 文字列) と `structuredContent` の両方に同じ JSON が入る。
引数の誤りや道具の中の失敗は、JSON-RPC エラーではなく `isError: true` の結果で返る。

| 道具 | 引数 | 返るもの |
|---|---|---|
| `search` | `q` (1〜200 文字)、`kind` (配列。省略 = 全部)、`limit` (1〜100、既定 20) | `{stale, total, hits:[{kind, id, score, src, ...}]}`。日本語でも英語でも引ける |
| `describe` | `id` (search が返した id と完全一致) | `{stale, matches:[...]}`。component のフィールド、言及する spec 見出しと ADR (各 20 件まで + `mentionsTotal`)、関連 golden。未知 id は `isError` で候補 5 件 |
| `verify_job` | `op` = `start` / `poll`。start は `job` と `ticks` (1〜100000、既定 600)、poll は `jobId` | start: `{jobId, job, ticks, logPath}`。poll: `{state:"running"}` か `{state:"done", exitCode, summary}` |
| `note_read` | `topic` (省略 = 一覧)、`latest` (1〜50、既定 5。topic が要る) | 一覧: `{topics:[{topic, bytes, lastHeading}]}` (topic 名順)。topic 指定: `{topic, total, truncated, entries:[{heading, text}]}` (古い順。見出しと本文を JSON 文字列に直した長さの合計が 64 KB を超えたら古い側を落として `truncated: true`。応答は同じ内容を `content` の text (JSON をさらに文字列にしたもの) と `structuredContent` の 2 か所に載せるので、応答 1 行は 64 KB の 2〜3 倍になる。`"` だけの 65536 バイトの本文で 196,842 B を実測) |
| `note_append` | `topic`、`text` (1〜65536 バイト)、`title` (任意、1 行 120 文字以内) | `{path, bytes}` (path はプロジェクトからの相対、bytes は追記後のファイルサイズ) |

`search` の `kind` は `component` / `field` / `cli` / `abi` / `adr` / `spec` / `golden` / `note`。`src` は `engine` か `notes` で、
メモ (notes) はエンジンの権威情報と混ぜずに `src: "notes"` で区別する。

### verify_job

`tools\replay_verify.bat --job <名前>` をバックグラウンドで 1 本走らせ、終わったら `cache\mcp_logs\<名前>.log` を読んで
落ちた段・最初に割れた tick・診断行だけを `summary` に返す (ログ全体は返さない。1 本 150 KB 前後ある)。
**ビルドはしない**。Debug と Release の `Editor.exe` が先にビルドされている必要がある (無ければ `isError`)。

- 名前は固定の 22 個: `demo` `parts` `flow` `mp` `physics` `joints` `acoustic` `ui` `fracture` `nav` `perception` `bt` `anim` `compute` `2d` `jobsab` `computeab` `ttdebug` `ttrelease` `whatifdebug` `whatifrelease` `rules`。
  `replay_verify.bat` の `:job_*` と `verify.rs` の表が食い違うと `tools\check_rules.ps1` の規則 15-e が止める。
- 同時に走るのは 2 本まで。同じ名前は同時に 2 本走らせられない。
- `summary` は `result` (`PASS` / `FAIL`)、`stage`、`firstMismatchTick`、`diagnostic`、ログが無いとき `noLog: true`。

注意:

- **走行中のジョブは MCP が終わっても止まらない。** 子の `cmd` は切り離して動いている。Claude Code を閉じても `replay_verify` は最後まで走る。
- **ジョブ表は MCP のプロセスの中だけにある。** MCP を再起動すると、前の `jobId` は `poll` できない
  (ログは `cache\mcp_logs\` に残るので、直接読む)。
- **同名ジョブの二重起動を止めるのは、同じ MCP プロセスの中だけ。** MCP を再起動した直後 (前の子がまだ走っている) や、
  Claude Code のセッションを 2 つ開いて両方から同じ名前のジョブを起動すると、2 本目も通る。2 本は
  `cache\mcp_logs\<name>.log` と `cache\*.rep` を奪い合って結果が壊れるので、前のジョブが終わったことを確かめてから起動する。
- **全体実行の `tools\replay_verify.bat` と同時に叩かない。** どちらも `cache\*.rep` を書くので奪い合って割れる。
  `verify_job` を使う間は全体実行を走らせない (逆も同じ)。
- `rules` は `tools\check_rules.ps1` を回すジョブ。作業ツリーに既知の ERROR が残っている間は FAIL が正しい結果。

## notes (記録の住み分け)

`.mye\notes\<topic>.md` に追記専用で溜まる、プロジェクト固有の現在地・実測値のメモ。
書き先は `--project` を付けたときは `<project>\.mye\notes`、無ければ `<リポジトリ>\.mye\notes`。
リポジトリの `.gitignore` に `.mye/` があり、notes ディレクトリを作るときに中身 `*` の `.gitignore` も置くので、
どのプロジェクトでも git に載らない。残す価値があるものは人が `docs\` へ昇格させる。

| 場所 | 書くもの | 読む主体 |
|---|---|---|
| `docs\adr\` | 覆らない設計判断 | 人 + AI |
| `plans\` | これからやること | 人 + AI |
| Claude Code のメモリ | 作業のクセ・踏んだ罠 | Claude Code のみ |
| `.mye\notes\` | プロジェクト固有の実測値・現在地 (コミットできない / したくない状態) | 全 LLM |

書き方:

- 1 回の `note_append` が `## <UTC 時刻 (秒まで、Z 付き)> <title>` の見出しと本文になる。**上書き・削除・改名はできない。**
  間違いは次のエントリで訂正する。
- `topic` は `[A-Za-z0-9_-]` の 1〜64 文字。`con` `nul` `com1` のような Windows の予約デバイス名 (大小無視) は使えない。
  パス区切り・`.`・日本語など、それ以外の文字を含むと拒否される。
- `.mye` と `.mye\notes` とトピックのファイルがシンボリックリンク・ジャンクションなら、読み書きとも拒否する。
- 本文に `## <時刻>` で始まる行を書くと、見出しとして読まれる。

## 検証

```
tools\mcp_verify.bat                # cargo test + build + tests\mcp の全シナリオ。exit 0 が合格
tools\mcp_verify.bat --update       # 期待を撮り直す (差分を読んでからコミット)
pwsh -NoProfile -File tools\mcp_verify.ps1 -RealIndex   # 実索引に対する固定 10 問 (索引が無ければ SKIP)
```

シナリオの書き方 (`# args` / `# junction` / `# expect-untouched` / `@@REP:x:n@@`) は `tools\mcp_verify.ps1` の冒頭にある。
notes のシナリオは `cache\mcp_verify\proj_<名前>` を書き先にする (ジャンクションの試験もここで作って片付ける)。
