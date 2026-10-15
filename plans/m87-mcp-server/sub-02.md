# sub-02: MCP crate 骨格 (stdio JSON-RPC、dual-era) と search / describe、mcp_verify

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること

spec 4.1.3 / 4.1.4 / 4.4 (規則 15-c・15-d の器) を実装する。

1. 新規 crate `tools\mcp\` (`mye_mcp`、bin のみ)。`Cargo.toml` は `tools\collab\Cargo.toml` の判断 (edition 2021 / `panic = "unwind"` / 依存を絞る理由) をコメントごと踏襲し、依存は `serde` (derive) と `serde_json` のみ。
2. `src\stdio.rs` (行の読み書きだけ)、`src\handler.rs` (`fn handle(&mut Context, Request) -> Option<Response>`)、`src\index.rs` (索引の読み込みと stale 判定)、`src\search.rs` (スコア・同義語・describe)。ファイル分けは目安で、coder が決めてよい (transport と handler の分離は必須)。
3. dual-era: legacy `initialize` (3 版) と modern (`server/discover`、`_meta` の版、-32022) を spec 4.1.3 のとおり。
4. `search` / `describe` を spec 4.1.4 のとおり。
5. 同義語表の決定 (spec 2. の手順): 機械生成のみで固定 10 問を流して命中数を記録 → 外れた語だけ `tools\mcp\synonyms_extra.tsv` に足す (0 行ならファイルを作らない)。
6. `tools\mcp_verify.bat` (+ 必要なら `.ps1`): `cargo test` → `cargo build --release` → `tests\mcp\*.ndjson` を `tools\mcp\target\release\mye_mcp.exe --root <repo> --index tests\mcp\fixture\engine_index.json` の stdin に流し、出力を `*.expected.ndjson` と比較。`--update` で撮り直し。`collab_verify.ps1` の形 (シナリオ単位で独立、正規化は環境依存のものだけ) を踏襲する。
7. `tests\mcp\fixture\engine_index.json`: 実索引から数件を抜いた小さな固定フィクスチャ (component 3〜5、cli・golden・adr・spec・l10n・abi 各数件)。`sources` は stale のテスト用に、存在するファイル / 時刻が違うファイルを用意する方法を coder が決める (時刻依存で期待が揺れないこと)。
8. 規則 15-c / 15-d の器 (許可ファイル 0 件)。
9. `.gitignore` に `tools/mcp/target/`。
10. 実クライアント確認 (spec 条件 15)。
11. リポジトリのルートに `.mcp.json` を置く (ユーザー判断 2026-10-15)。中身は `{"mcpServers": {"mye-engine": {"command": "tools/mcp/target/release/mye_mcp.exe", "args": []}}}` を出発点にする。相対パスの起点と、未ビルド時の見え方を実測する (spec 条件 30 / 31)。実測の結果は実装メモに書く。sub-04 が docs に写す。

## やらないこと (このサブでは)

- `verify_job` / `note_*` (tools/list にも載せない)
- CI ワークフロー・`--selftest` への組み込み
- `tools\` の workspace 化、`build_collab.bat` / `collab_verify.bat` への変更
- ユーザーの Claude Code 設定の変更 (`claude mcp add` を実行しない。登録は `.mcp.json` のコミットで行う)

## 触る場所 (planner の見立て)

- 新規 `tools\mcp\Cargo.toml` / `Cargo.lock` / `src\*.rs`
- 新規 `tools\mcp_verify.bat` (CRLF。`.gitattributes` が `*.bat eol=crlf`。規則 14-b が見る)。他の新規テキストは LF
- 新規 `tests\mcp\*.ndjson` / `*.expected.ndjson` / `fixture\engine_index.json`
- `tools\check_rules.ps1` (規則 15-c / 15-d)
- `.gitignore`
- 新規 `.mcp.json` (リポジトリのルート、LF)。Claude Code は project スコープの `.mcp.json` を初回に承認させる。承認状態はユーザー側の設定にあるので、coder は承認操作をしない。検証は `claude --mcp-config .mcp.json --strict-mcp-config -p ...` の形で行う (永続設定を変えない)。`${VAR}` 展開に頼る書き方は、実測で確かめるまで使わない
- 参考: `tools\collab\src\protocol.rs` / `main.rs` (NDJSON の読み書きの流儀)、`tools\collab_verify.ps1` (回帰の回し方)
- 罠: Windows の stdout はテキストモードの変換は無いが、`println!` は行バッファでないことがある → 応答ごとに flush。stdin の行末 `\r\n` を許す。

## 受け入れ条件 (このサブ)

spec 5. の 8〜15 と 30・31。

8. edition 2021 / `panic = "unwind"` / 依存 serde・serde_json のみ。`cargo build --release` と `cargo test` が通る。`Cargo.lock` 追跡、`tools/mcp/target/` は `git check-ignore` で無視される。
9. JSON-RPC 誤り系と legacy / modern の時代判定が ndjson の期待どおり。
10. `tools/list` の並びと `inputSchema` が期待どおり。
11. フィクスチャ索引で search / describe の各ケースが期待どおり (spec 5. の 11 の列挙を全部)。
12. 実索引 (gen で作り直したもの) で固定 10 問が top-5 命中 (spec 5. の 12 の表)。機械のみの命中数と手書き表の行数を実装メモに記録。最終 10/10。
13. `tools\mcp_verify.bat` exit 0。`--update` が動く。`.github` は無変更。
14. 規則 15-c / 15-d の器が入り、違反を仕込んだ一時コピーで ERROR を 1 回示す。check_rules OK。
15. `claude` CLI の `--mcp-config` で `search` を 1 回呼べた記録。設定ファイルはコミットする `.mcp.json` を推奨し、scratch の一時ファイルでもよい。どちらの時代で繋がったか (stderr ログ) を書く。実行できなければ「未検証」と書き、理由を添える。
30. `.mcp.json` がルートにあり、JSON として妥当で、`mye-engine` → `tools\mcp\target\release\mye_mcp.exe` を指す。リポジトリのルートで起動した場合とサブディレクトリで起動した場合について、相対パスがどこを起点に解決されるかを実測して記録する (絶対パスは使わない)。
31. 未ビルド時 (exe を一時的に退避し、作業後に戻す) に Claude Code がこのサーバをどう表示するかを実測して記録する。Claude Code 本体が止まらないこと。記録は sub-04 の docs が写せる具体さで書く (表示文言、終了コード等)。

## 検証コマンド

- `cargo build --release --manifest-path tools\mcp\Cargo.toml` / `cargo test --manifest-path tools\mcp\Cargo.toml`
- `tools\mcp_verify.bat` (PowerShell ツールから `cmd /c` で)
- `pwsh -NoProfile -File tools\gen_engine_index.ps1` (実索引の作り直し) + 固定 10 問のスクリプト (scratch に置く)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `git check-ignore -v tools/mcp/target/x`

## 実装メモ (coder が追記)

## フィードバック履歴
