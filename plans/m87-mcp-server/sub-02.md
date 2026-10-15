# sub-02: MCP crate 骨格 (stdio JSON-RPC、dual-era) と search / describe、mcp_verify

- 依存: sub-01
- 状態: OK (commit 75d3004)
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

SELF_EVAL: sub-02 (round 1)

実装:
- tools\mcp\ (新規 crate `mye_mcp`): `Cargo.toml` (edition 2021 / panic unwind / 依存 serde・serde_json のみ)、`Cargo.lock`、`src\{main,rpc,stdio,handler,tools,index,search}.rs`、`synonyms_extra.tsv` (5 行)。transport (`stdio.rs`) と handler (`handler.rs::handle`) は分離。JSON-RPC の解析と応答の組み立ては `rpc.rs`。
- handler: legacy `initialize` (3 版 echo、未知は 2025-11-25)、modern (`_meta` の版 2026-07-28 → `resultType: complete`、未知版 → -32022)、`server/discover`、`ping`、`tools/list`、`tools/call`。通知は無応答。時代は stderr に `[mye_mcp] <method> era=...` で残す。
- search / describe: 完全一致 100 / 前方 80 / 部分 60 / 訳語 40 / 同義語 30。同点は kind 固定順 → id。stale は sources の mtime を 100ns 単位で厳密比較 (欠け・root 不明は true)。describe の言及は 1 種別 20 件 + `mentionsTotal`。
- tools\mcp_verify.{bat,ps1} (bat は CRLF)、tests\mcp\*.ndjson 7 本 + 期待、tests\mcp\fixture\ (engine_index / _fresh / _schema2)。
- tools\check_rules.ps1: 規則 15-c / 15-d の器 (許可ファイル 0 件)、`-McpSourceDir` 引数。`.gitignore` に `tools/mcp/target/`。`.mcp.json` をルートに追加。

仕様との差分:
- [追加] modern の `tools/list` 結果に `ttlMs` (3600000) と `cacheScope` ("public") を付ける。実クライアント (Claude Code 2.1.300) が欠けると tools/list を拒否したため (spec 7. のリスクが的中)。legacy には付けない。spec 4.1.3 への追記を推奨。
- [追加] search の返却に `total` (limit 前の該当数) を足した。切り詰められたか分かるように。
- [追加] 引数検査: `limit` が 1〜100 の整数でなければ isError (丸めない)、未知の引数名も isError、`kind` は文字列 1 個でも受ける。`q` が空白のみも isError。
- [追加] 同義語の具体 (spec 2. の手順の結果。下の「同義語」節): 機械展開は l10n の短いラベル (40 文字以下) の英単語を、名前の**語の先頭**で引く (部分文字列ではない。"time" が "Runtime" に当たらないように)。手書き表 `synonyms_extra.tsv` は訳語表として 40 点 (30 ではない) で名前への部分文字列一致。30 点だと CameraPostFx (フィールドの jp 40) や Ticks 系フィールドの 30 に埋もれて top-5 に入らなかった。
- [追加] describe の返却は `{stale, matches:[...]}`、未知 id は isError + `details.candidates` (上位 5 件、kind と id)。CLI の golden 逆引きは args の要素の完全一致。golden の `cliFlags` は各フラグの層 (どの表にも無ければ null)。ADR が索引に無いときの言及の title は空文字。
- [追加] JSON-RPC: `jsonrpc` が "2.0" でない / `id` が null・オブジェクト・真偽 → -32600。解析不能の message は固定文 "Parse error" (serde_json の版で文面が変わるため。詳細は stderr)。-32022 の `data.supported` は discover と同じ 4 版 (modern だけを列挙すると legacy へ落とせない、という判断。クライアントが 2025-11-25 を `_meta` で送る形は想定外)。
- [追加] check_rules に `-McpSourceDir` (違反を仕込んだ一時コピーを見せるため。`-SourceRoot` と同じ考え方)。mcp_verify のシナリオ先頭 `# args ...` で既定の `--index` を差し替える (索引なし / schema 違い / stale でない索引)。
- [追加] フィクスチャの component は Name / Rigidbody / Terrain / Ragdoll / BehaviorTree の 5 (AcousticVolume は外した。jp 一致は Rigidbody.mass で見る)。stale の期待が時刻に依存しないよう、sources は「時刻の違う実在ファイル (.gitattributes) + 存在しないファイル」、stale でない版は sources を空にした別ファイル。`mentions` の Rigidbody は spec 25 件に水増し (20 件上限と mentionsTotal の確認用の合成データ)。
- [未実装] なし (verify_job / note_* は対象外。tools/list にも載せていない。`kind: note` は enum に受けるが該当データは無い)。

同義語 (条件 12 の記録):
- 機械生成のみ (l10n の en/ja 対から。手書き表なし): 固定 10 問のうち top-5 命中 5/10 (音の遮蔽 / ラグドール / 地形 / ビヘイビアツリー / スクロール)。外れた 5 問: 剛体 (l10n にも jp にも無い)、経路探索 (複合語。l10n に 経路探索 は無い)、霧 (CameraPostFx のフィールド jp が 40 で先に並び、Fog は名前しか持たない)、破壊 (l10n は長い説明文だけで短いラベルが無い)、タイムトラベル (展開語 time / ticks が雑音で、`--timetravel-selftest` に届かない)。
- `tools\mcp\synonyms_extra.tsv` = 5 行: 剛体→rigidbody、経路→navmesh、霧→fog、破壊→destructible,fracture、タイムトラベル→timetravel。外れた 5 問 1 行ずつ。最終 10/10 (実索引を gen で作り直してから測定。scratch の ten.py)。
- 途中経過: 展開語を「文章を含む全 l10n」かつ部分文字列一致で引く初版は 6/10 だったが、`Runtime` に "time" が当たる等の雑音が多く、短いラベル限定 + 語の先頭一致にして 5/10 に下がった (雑音を取って精度を選んだ)。

実クライアント (条件 15 / 30 / 31。Claude Code 2.1.300、`--mcp-config` + `--strict-mcp-config`、永続設定は変更していない。`--allowedTools mcp__mye-engine__search` はその 1 回の起動だけの指定):
- 15: **modern で繋がった**。デバッグログ `Connection established ... "protocolEra":"modern","negotiatedProtocolVersion":"2026-07-28"`、サーバ stderr `[mye_mcp] server/discover era=modern`。初回は `tools/list` が `ttlMs` / `cacheScope` 欠落で拒否された (上の [追加] で修正後に成功)。修正後: `Tool 'search' completed successfully in 21ms`、モデルの返答は "Rigidbody" (search q=Rigidbody の先頭 hit)。legacy 時代で実クライアントが繋がる経路は実測していない (単体は ndjson で確認)。ログに `version negotiation probe closed the stdio server (rmcp-class pre-init hard close); respawning pinned legacy` が出る = クライアントは modern の探りで落ちると legacy で再接続する作りで、legacy も実クライアントに使われうる。
- 30: `.mcp.json` はルート (`{"mcpServers":{"mye-engine":{"command":"tools/mcp/target/release/mye_mcp.exe","args":[]}}}`、スラッシュ区切りで JSON として妥当)。相対パスの起点は**クライアントのカレントディレクトリ**: ルートから `--mcp-config .mcp.json` なら接続成功。サブディレクトリ `tools\` から `--mcp-config C:\HAL\MyEngin\.mcp.json` (絶対パス指定) だと失敗 (`CONNECTION_CLOSED`、サーバ stderr = cmd の「指定されたパスが見つかりません」)。`claude mcp list` はルートでもサブディレクトリ (`tools\`) でも `.mcp.json` を上へ辿って見つけ、どちらも同じ表示 (下記)。→ docs には「Claude Code はリポジトリのルートで起動する」制約を書く。承認前なので、プロジェクトスコープ (承認済み) でサブディレクトリから起動した場合の実測は未実施 (承認操作をしないため。`--mcp-config` 経由の起点は上記)。
- 31: 承認前: `claude mcp list` は `mye-engine: tools/mcp/target/release/mye_mcp.exe  - ⏸ Pending approval (run \`claude\` to approve)` (exe の有無に関係なく、承認前は起動しない)。exe を一時退避して `--mcp-config` で起動 (作業後に戻した): stream-json の init に `{"name":"mye-engine","status":"failed","source":"dynamic"}`、デバッグログ `Connection failed after 404ms (CONNECTION_CLOSED): Connection closed`、サーバ側 stderr は cmd の「'tools\mcp\target\release\mye_mcp.exe' は、内部コマンドまたは外部コマンド、操作可能なプログラムまたはバッチ ファイルとして認識されていません。」。Claude Code 本体は止まらず、同じセッションで通常どおり応答 (`-p "Say hello"` → exit 0、返答あり)。ビルドの 1 行は `cargo build --release --manifest-path tools\mcp\Cargo.toml`。

検証:
- `cargo test --manifest-path tools\mcp\Cargo.toml` → 21 passed (rpc 3 / stdio 2 / index 4 / handler 5 / search 7)。warning なし。
- `cargo build --release --manifest-path tools\mcp\Cargo.toml` → 成功、warning なし。
- `tools\mcp_verify.bat` → exit 0 (cargo test + build + 7 シナリオ全 PASS: describe 17 行 / protocol 24 / search 30 / search_fresh 1 / search_noindex 2 / search_schema2 1 / tools_list 2)。`--update` で撮り直せた。途中で tools_list の期待が古く FAIL したのを確認済み (差分検出の実地確認)。
- `pwsh -NoProfile -File tools\gen_engine_index.ps1` → 成功 (351,942 B、81 components)。作り直した実索引で固定 10 問 10/10、stale=False (mtime 比較が実ファイルで一致)。
- `pwsh -NoProfile -File tools\check_rules.ps1` → ERROR は既知 3 件 (rule 1 EditorMain / rule 14-b engine_dist 2 件) のみ。違反を仕込んだ一時ディレクトリ (`-McpSourceDir`、scratch) では 15-c 2 件 + 15-d 4 件の ERROR (use std::process::Command / Command::new・spawn / use std::fs::{.., create_dir_all} / fs::write / OpenOptions / File::create を検出、`//` コメント内は許す)。
- `git check-ignore -v tools/mcp/target/x` → `.gitignore:29:tools/mcp/target/`。`git diff --stat .github` → 空。`git ls-files --eol` (add -N 後) → mcp_verify.bat だけ w/crlf、他は w/lf。確認後 `git reset` で intent-to-add を戻した。
- `claude` CLI 実接続 → 上記 (15 / 30 / 31)。

自己採点 (1-5):
  仕様適合: 4 — 条件 8〜15・30・31 を満たした。[追加] が複数 (ttlMs / total / 引数検査 / 同義語の具体) あり、うち ttlMs は spec への反映が要る。
  正しさ: 4 — ndjson 7 本 + unit 21 本 + 実索引 10 問 + 実クライアントの modern 接続で確認。legacy の実クライアント接続と、承認済みプロジェクトスコープでのサブディレクトリ起動は未確認。
  コード品質: 4 — 責務は rpc / stdio / handler / tools / index / search に分離、依存は serde・serde_json のみ。search.rs が長め (entities・クエリ準備・採点・describe が同居) なので sub-03/04 前に分けるのも手。
  テスト: 4 — 回帰は固定フィクスチャで時刻非依存。実索引の 10 問は手動スクリプトで、mcp_verify には入っていない (索引が cache\ にあり gitignore のため)。

不安・質問:
- ttlMs / cacheScope を spec 4.1.3 に追記してよいか (planner の判断)。値は 3600000 / "public" (道具表はプロセス寿命の間固定のため)。sub-03/04 で道具が増えても同じ値でよい想定。
- 手書き訳語表を 40 点にしたのは spec 4.1.4 の「同義語展開 30」とずれる (表は訳語表の扱い、機械展開は 30 のまま)。spec 側を直すか、30 に戻して別の手当てをするか。
- 固定 10 問を mcp_verify に入れていない (実索引依存)。入れるなら「索引が無ければ SKIP」の形になるが、CI に入れない方針なので現状のままにした。

触ったファイル:
  .gitignore、.mcp.json、tools\check_rules.ps1、tools\mcp_verify.bat、tools\mcp_verify.ps1、tools\mcp\Cargo.toml、tools\mcp\Cargo.lock、tools\mcp\synonyms_extra.tsv、tools\mcp\src\{main,rpc,stdio,handler,tools,index,search}.rs、tests\mcp\{describe,protocol,search,search_fresh,search_noindex,search_schema2,tools_list}.ndjson と各 .expected.ndjson、tests\mcp\fixture\{engine_index,engine_index_fresh,engine_index_schema2}.json

申し送り:
- sub-03: verify_job の許可は `check_rules.ps1` の `$mcpSpawnAllowed` / `$mcpWriteAllowed` (どちらも空配列) に `verify.rs` を足す。`tools::list()` の末尾に道具を足し、`tools::call` の match に枝を足す。tools_list の期待は撮り直しになる。
- sub-04: docs\mcp-server.md に 30 / 31 の実測 (上記) を写す。要点: Claude Code はリポジトリのルートで起動する (相対パスはクライアントの cwd 起点)、承認前は `Pending approval`、未ビルドは `failed` + CONNECTION_CLOSED + cmd の「認識されていません」、本体は止まらない。`.gitignore` のコメントが docs\mcp-server.md を指している (sub-04 で作る)。
- Claude Code 2.1.300 は modern を先に試し、探りで落ちると legacy へ落とす。modern の list 系結果は `ttlMs` + `cacheScope` 必須。他の modern 結果 (tools/call、server/discover) は現状の形で通った。
- 実索引の固定 10 問の scratch スクリプトは scratchpad の ten.py (コミットしていない)。
- 作業ツリーの無関係な変更 (StartupSplash.cpp、docs\deepmodal\training-cleanup-20261015.md) と plans\ の未コミット変更には触っていない。

## フィードバック履歴
- round 1: VERDICT OK (planner)。mcp_verify exit 0、Cargo.toml (edition 2021 / panic unwind / serde のみ)、`.mcp.json`、check_rules 既知 3 件のみを planner が再実行して確認した。追加 (ttlMs / 訳語表 40 点 / total) は spec に取り込んだ。固定 10 問の再現スクリプトは sub-04 で mcp_verify に入れる
