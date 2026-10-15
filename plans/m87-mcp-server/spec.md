# エンジン MCP サーバ (M87) — 仕様書

- slug: m87-mcp-server
- 状態: 確定 (2026-10-15、planner 裁定。`[ユーザーに聞ける]` の論点は PLAN_RESULT の未決事項に列挙。司会がユーザーに確認し、覆ったら変更履歴に積む)
- 依頼原文: M87の実装をしたい
  (司会の方針確認への回答) 1.全部ハーネスで 2.すべて 3.87bで決める
- 設計の正本: `C:\HAL\MyEngin\plans\エンジンMCPサーバ.md` (以下「計画書」)。§3 の決定と §9 の却下案は覆さない。
  本書は計画書の数値・前提を 2026-10-15 の実コードで更新し、受け入れ条件を機械判定できる形に落としたもの。

## 1. 目的 (なぜ作るか)

AI クライアント (Claude Code ほか) がこのエンジンで作業するとき、次の 3 つを安く済ませる (計画書 §2):

1. **ログの構造化**: `replay_verify.bat` の 1 ジョブのログは 1 本 150 KB 前後 (実測: `cache\replay_logs\demo.log` = 154,291 B)。落ちた段 / 割れた tick / 診断行だけを返す。
2. **索引検索**: component / field / CLI / ABI / ADR / spec 見出し / golden を、`engine_spec.md` (385 KB) を grep せずに 1 呼び出しで引く。
3. **記録の継承**: プロジェクト固有の現在地を、セッションと LLM をまたいで追記・参照できる置き場。

エンジン (sim) の挙動は 1 バイトも変えない。M87 はエンジンの外側の開発ツールである。

## 2. 疑った点と結論

| 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|
| verify_job の対象は「23 個」か | `tools\replay_verify.bat` の `:job_*` ラベルは 21 個 (181〜421 行)、`run_parallel.ps1` へ渡す `-Jobs` 列も同じ 21 個: demo parts flow mp physics joints acoustic ui fracture nav perception bt anim compute jobsab computeab ttdebug ttrelease whatifdebug whatifrelease rules | 「全部」(司会経由) | **21 個**。依頼の「23」は数え違い。全部を対象にする意図はそのまま。名前の表は Rust の固定表に置き、bat の `:job_*` と `-Jobs` 列との三者一致を check_rules 規則 15-e で見張る (司会の推奨「ラベルから作り件数を見張る」を、受け付ける値をコードで読める形にして採用) |
| 計画書 §5 の poll 例にある `firstMismatchTick` / `fields` (フィールド差分) は `--job` 経路で取れるか | `[diag] first mismatch tick:` と `[diag] field-level diff` を出す `:diagnose` は、全体実行の `:failed` (replay_verify.bat 102〜178 行) からしか呼ばれない。`--job` 再入 (`:job` → `:job_<name>`) では走らない。ただしエンジンは照合失敗時に `[replay] HASH MISMATCH at tick N` をログへ出し (`TickRunner.cpp:704`)、`<rep>.mismatch.txt` を残す (`TickRunner.cpp:734`) | なし (planner 裁定) `[ユーザーに聞ける]` | **v1 は tick と段と診断行まで返し、フィールド差分は返さない** (`fieldDiff: null` + 全体実行で得られる旨の `hint`)。却下した案: (a) MCP が自前で Editor.exe を起動して再録画 → 「CI と同じ経路だけを通す」(計画書 §3.1) に反する、(b) bat に `--job diagnose_<scene>` 再入口を足す → CI 経路の bat に手を入れることになり sub-03 が膨らむ。必要になったら (b) を別マイルストーンで |
| MCP の `initialize` / `protocolVersion` の現行値 (計画書 §8-3) | 公式仕様の最新版は **2026-07-28** で、`initialize` ハンドシェイクを廃止し、全リクエストの `params._meta["io.modelcontextprotocol/protocolVersion"]` で版を運ぶ「modern」方式になった。`server/discover` は MUST。`2025-11-25` 以前は「legacy」(initialize あり)。両対応 (dual-era) のサーバは MAY。出典: https://modelcontextprotocol.io/specification/latest/basic/lifecycle (版と互換の頁へ転送される) / https://modelcontextprotocol.io/specification/2026-07-28/server/discover / https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle / https://modelcontextprotocol.io/specification/2026-07-28/server/tools (2026-10-15 取得) | なし (planner 裁定) `[ユーザーに聞ける]` | **dual-era で実装する**。legacy (`initialize`) は必須、modern (`server/discover` + `_meta`) も受ける。理由: 手元の Claude Code がどちらの時代のクライアントかは未確認で、legacy だけだと modern 専用クライアントから繋がらない (互換表で Modern×Legacy = Fails)。handler は版をリクエスト単位で判定するので純関数の設計 (計画書 §3) と矛盾しない。却下した案: legacy のみ (実装は少ないが、外れたら接続不能) |
| 計画書 §4.2 の「CLI の説明は CLAUDE.md から」 | 現在の `CLAUDE.md` は `@AGENTS.md` の 1 行だけで、フラグの説明は載っていない。フラグの実体は 3 か所: `EngineCli.cpp` の `kEngineCliFlags[]` (実行時の表、名前と値の個数)、`ShowcaseScenes.cpp` の `kShowcases[]` (`--*-demo`、実行時の表)、`EditorMain.cpp` の if 連鎖 (Editor 専用、表なし) | なし | 前 2 つは `--dump-index` が**実行時の表から**出す (パース不要)。Editor 専用フラグだけ `gen_engine_index.ps1` が `arg == L"--..."` を静的に拾う。説明文の代わりに「そのフラグに言及する `engine_spec.md` の見出し」を生成時に紐付ける。「文書にあるがパーサに無いフラグ」の突き合わせは情報出力だけ (失敗にしない) |
| 計画書 §4.3「件数の下限を check_rules に」 | check_rules は CI で走り、Editor.exe を起動しない。実行時ダンプ由来の件数 (component / field) は check_rules からは見えない。静かに壊れうるのは静的パース側 (ABI / golden / ADR / spec / l10n / Editor 専用 CLI) | なし | 下限は 2 か所に分ける: (1) 静的パースの下限は `gen_engine_index.ps1 -CheckOnly` (Editor.exe を起動しない) に持たせ、check_rules 規則 15-f がそれを呼ぶ = CI でも見張れる。ABI は固定値ではなく `$apiVersionSlots[MYE_API_VERSION]` (現在 30 → 178) と完全一致。(2) 実行時ダンプの下限 (component / field) は通常の生成時と `EngineIndexDumpSelfTest` で見る |
| 「規則 13」の番号 | `check_rules.ps1` の規則 13 (M81/M93) と 14 (改行) は使用中 | なし | **規則 15** を使う |
| `--dump-index` の SelfTest を `--selftest` に入れてよいか | ユーザーの過去指示は「CI と `--selftest` に **MCP** を入れない」。`--dump-index` は Editor の機能で MCP ではない。AGENTS.md §7 は「回帰テストは `*SelfTest` パターンで対象の近くに」 | なし (planner 裁定) `[ユーザーに聞ける]` | **入れる** (`EngineIndexDumpSelfTest`、ヘッドレスで数十 ms 規模)。MCP の crate・mcp_verify は `--selftest` にも CI にも入れない。逆を選ぶと: 受け入れ条件 A6 が「gen スクリプトの下限チェックのみ」に縮む |
| MCP の子プロセスが MCP の stdout (= JSON-RPC の線) を汚さないか | stdio transport では stdout は JSON-RPC 専用。子が stdout を継承すると Editor.exe のログが線に混ざる | なし | 子は stdin/stdout/stderr とも null。出力は cmd 側の `> "<log>" 2>&1` でファイルへ (run_parallel.ps1 と同じ) |
| Rust の `Command::args` で cmd へ `/c chcp 437 >nul & call ...` を渡せるか | Rust の既定の引数クォート (MSVC 規約) は cmd.exe の解釈と一致しない (`&` `>` `"` の扱い)。stable の `std::os::windows::process::CommandExt::raw_arg` で生の文字列を渡せる | なし | `raw_arg` で run_parallel.ps1:85 と同じ文字列を渡す。受け入れ条件で文字列一致を unit test する |
| 子プロセスの寿命 | 依存は serde / serde_json だけ (計画書 §3) なので Job Object などの API ラッパは使えない。run_parallel.ps1 にもキャンセル・タイムアウトは無い | なし | v1 は start / poll のみ (計画書どおり)。MCP サーバが終了しても走行中のジョブは止めない (ログは残る)。ジョブ表はプロセス内メモリなので、サーバ再起動後は前の jobId を poll できない (エラーで返す)。リスクとして 7. に残す |
| 同じジョブの同時実行 / 全体実行との衝突 | 各ジョブは `cache\golden*.rep` など固有ファイルを書く。同名ジョブを 2 本同時に走らせると同じファイルを奪い合う。全体実行の `replay_verify.bat` は `cache\replay_logs` を毎回消す | なし | 同名ジョブが走行中なら start を拒否。同時実行は 2 本まで [仮置き: Editor 1 本 ≒ 2.2 GB (run_parallel.ps1:53)]。ログは `cache\mcp_logs\<name>.log` (全体実行の `cache\replay_logs` と分ける)。人が全体実行と同時に叩くのは検出しない (docs に注意書き) |
| notes の topic のガード (計画書 §6 は `[A-Za-z0-9_-]{1,64}`) | Windows では `CON` `NUL` `PRN` `AUX` `COM1`〜`COM9` `LPT1`〜`LPT9` は拡張子付きでもデバイス名として開かれる (`con.md` はコンソール)。正規表現だけでは通る | なし | 予約デバイス名 (大文字小文字無視) も拒否する。加えて書き先ディレクトリ / ファイルがシンボリックリンク・ジャンクションなら拒否、正規化後のパスが notes ディレクトリ直下でなければ拒否 |
| notes の gitignore (計画書 §6 は gitignore する) | エンジンリポの `.gitignore` に `.mye/` は無い。`--project` 先の外部プロジェクトの `.gitignore` はこちらで管理できない | なし | (1) エンジンリポの `.gitignore` に `.mye/` を足す。(2) notes ディレクトリを作るときに `<notes>\.gitignore` (中身 `*`) を同じディレクトリに置く — 書き込みは notes 配下だけという規則の内側で、どのプロジェクトでも追跡されない |
| MCP の登録方法 | プロジェクト共有の `.mcp.json` をコミットすると、このリポを開く全員の Claude Code が起動時に接続を試みる (未ビルドなら失敗表示)。エージェントがユーザーの Claude Code 設定を書き換えるのは権限外 | planner は「コミットしない」と裁定したが、**ユーザーが 2026-10-15 に「コミットする」と判断** | **リポジトリのルートに `.mcp.json` をコミットする** (sub-02)。サーバ名は `mye-engine`、コマンドは `cargo build --release` の出力先 `tools\mcp\target\release\mye_mcp.exe` を指す (root は exe の場所から上へ探すので `--root` は付けない)。planner の反対意見 (記録のみ、蒸し返さない): 未ビルドの人の Claude Code にも接続失敗が出る。対処として、未ビルド時の見え方を sub-02 で実測し、`docs\mcp-server.md` にビルドの 1 行と症状を書いて整合させる (条件 30・31)。ユーザー設定の変更 (`claude mcp add`) はしない |
| 実クライアントでの接続確認 | coder はユーザーの Claude Code 設定を変えられない。`claude` CLI の `--mcp-config <file>` は一時ファイルで MCP を読ませられる (永続設定を変えない) | なし (planner 裁定) `[ユーザーに聞ける]` | B9 は「一時 `--mcp-config` で `search` を 1 回呼べた」を coder が試みる。実行できない環境なら「未検証」と書き、ユーザーの手動確認に回す (成功扱いにしない) |
| 日英同義語表の粒度 (計画書 §10、ユーザー回答「87b で決める」) | `LocalizationTable.inl` は 2354 行 / `MYE_STR` 1640 件 (計画書の 1269 行は古い)。`MYE_JP` (field の日本語表示名) は `Components.cpp` に 729 件あり、こちらは `--dump-index` が field の `jp` として既に出す | 「M87b で決める」 | **sub-02 の中で計測して決める**。手順: (1) `MYE_STR` の en/ja 対から機械生成した表だけで固定 10 問 (sub-02 の受け入れ条件) を流し、top-5 命中数を記録する。(2) 外れた問いがあれば、外れた語だけを手書きの小表 `tools\mcp\synonyms_extra.tsv` に足す。手書き表が 0 行で済めばファイルを作らない。結果 (機械のみの命中数 / 手書き行数) を planner が VERDICT 時に変更履歴へ積む |
| ADR を書くか | AGENTS.md §8「アーキテクチャ上の判断を変更する場合は関連 ADR を更新」。notes は記録場所の 4 つ目を増やす判断 (計画書 §6) | なし | sub-04 で ADR-032 を短く書く (計画書へのポインタ + 住み分け表 + Editor 層封じ込め)。設計の正本は計画書のまま |
| 既知の check_rules エラー 3 件 | 2026-10-15 実測: `ERROR [rule 1] src\Editor\App\EditorMain.cpp:729: #ifdef _DEBUG`、`ERROR [rule 14-b] tools/engine_dist/Editor.bat` / `Setup.bat` (LF)。起動スプラッシュ / 配布物のコミット由来で M87 と無関係 | 司会から申し送り | 受け入れ条件の「check_rules」は「**この既知 3 件以外に ERROR が無い**」と定義する。sub-01 が EditorMain.cpp に行を足すと 729 行目はずれるので、行番号ではなく「rule 1 の EditorMain.cpp 1 件 + rule 14-b の engine_dist 2 件」で判定する。直すのは M87 の範囲外 |
| `--dump-index` の出力サイズ (計画書 §8-1) | 未実測。見積もり: component 80 (登録名の実数)、field ≒ 740 (MYE_FIELD 系の出現数)、CLI ≒ 112 + 42 + showcase、ABI 178、l10n 1640 → 数百 KB | なし | sub-01 で実測して実装メモに書く。全体が 2 MB を超えたら sub-02 の着手前に planner が返却形 (limit / 省略フィールド) を見直す |

## 3. スコープ

- やる:
  - `Editor.exe --dump-index <path>` (ヘッドレス、実行時の表を JSON へ)
  - `tools\gen_engine_index.ps1` (ダンプ + 静的パース → `cache\engine_index.json`、`-CheckOnly` モード)
  - `tools\mcp\` 独立 crate `mye_mcp` (stdio JSON-RPC 2.0、dual-era MCP、道具 5 つ: `search` / `describe` / `verify_job` / `note_read` / `note_append`)
  - `tools\mcp_verify.bat` + `tests\mcp\*.ndjson` (固定フィクスチャ索引で回る回帰)
  - check_rules 規則 15 (a〜f、4.4 節)
  - `EngineIndexDumpSelfTest`
  - `.gitignore` に `tools/mcp/target/` と `.mye/`
  - リポジトリのルートの `.mcp.json` (サーバ名 `mye-engine`、ユーザー判断 2026-10-15)
  - `docs\mcp-server.md`、ADR-032、計画書の「現在地」更新、`docs\test_checklists.md` に手動確認項目
- やらない (明示的に外したもの):
  - CI (`.github\workflows`) への組み込み、`--selftest` への MCP の組み込み (ユーザー指示 / 計画書 §3)
  - Streamable HTTP、`rmcp`、workspace 化、本文の BM25、7 本フル、索引の自動再生成、エンジンへの LLM 搭載、notes の上書き・削除、notes の git 追跡 (計画書 §9)
  - `shot_verify.bat` のジョブ化 (計画書 §5、v1 対象外)
  - verify_job のフィールド差分、キャンセル、タイムアウト (2. 参照)
  - 既知の check_rules エラー 3 件の修正
- 後回し:
  - 姉妹メモの S1 (エディタ内検索パレット) / S2 (埋め込みモデル)
  - verify_job のフィールド差分 (bat に診断の再入口を足す案)

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 `Editor.exe --dump-index <out.json>` (sub-01)

- ウィンドウも D3D デバイスも作らず、`<out.json>` に UTF-8 (BOM なし) で書いて終了する。成功 exit 0、書けない等は exit 1 (stderr に理由)。
- 中身 (セクション名は固定):
  - `components`: 組み込みコンポーネント全部 (登録順 = TypeId 順。`kComponentScriptState` の型は含めない)。各要素 `name` / `typeId` / `size` / `align` / `flags` (名前の配列) / `fields`。
  - `fields` の各要素: `name` / `type` (`FieldType` の名前) / `offset` / `flags` (名前の配列) / `jp` (displayName、無ければ省略) / `tooltip` (無ければ省略) / `min` `max` (レンジ有効時のみ)。
  - `cliEngine`: `kEngineCliFlags[]` の `name` と値の個数 (`none` / `one` / `two` / `optional`)。
  - `cliShowcase`: `kShowcases[]` の `flag` と `editorOnly`。
- 並びと書式は決定的 (同じバイナリなら何回走らせてもバイト同一、Debug と Release でもバイト同一)。時刻・パス・PID を出力に含めない。
- `EngineCli.cpp` / `ShowcaseScenes.cpp` の表を外から列挙するための読み取り専用アクセサを足してよい (表の中身・パースの挙動は変えない)。

#### 4.1.2 `tools\gen_engine_index.ps1` (sub-01)

- 既定: `bin\x64\Release\Editor.exe` (無ければ Debug) で `--dump-index` を呼び、静的パースと合わせて `cache\engine_index.json` を書く。GUI サブシステムの exe なので終了を待つ呼び方にする (`Start-Process -Wait -PassThru` か `cmd /c`)。
- 静的パースするセクション:
  - `abi`: `src\Shared\EngineAPI.h` の `MYE_API_VERSION` とスロット列 (順序・名前・引数個数)。
  - `cliEditor`: `src\Editor\App\EditorMain.cpp` の `arg == L"--..."` (Editor 専用フラグ)。
  - `adr`: `docs\adr\*.md` の ID・題名・見出し。
  - `spec`: `engine_spec.md` の見出し (行番号つき)。
  - `golden`: `tools\shot_verify.bat` の `call :shot <name> <args...>` (出現順の番号・名前・引数) と `tests\golden\<name>.png` の有無。
  - `l10n`: `LocalizationTable.inl` の `MYE_STR(id, en, ja)`。
  - `mentions`: component 名と CLI フラグごとに、本文で言及している `engine_spec.md` 見出し / ADR の ID (生成時に計算。本文そのものは索引に入れない)。
  - `sources`: 索引の源にしたファイル (Editor.exe 含む) のパスと最終更新時刻。`generatedAt`。`schema: 1`。
- 生成時にセクションごとのバイト数と件数を表示する。「`engine_spec.md` で言及されているがどのパーサにも無いフラグ」を情報として表示する (失敗にしない)。
- 下限チェック (失敗で exit 1): abi スロット数 == check_rules の `$apiVersionSlots[MYE_API_VERSION]`、他のセクションは sub-01 で実測した件数の 90% (切り捨て) を下限として定数で持つ (根拠の実測値をコメントに書く)。component / field の下限はダンプを読む通常モードだけで見る。
- `-CheckOnly`: Editor.exe を起動せず、静的パースと下限チェックだけ行い、ファイルを書かない。リポジトリのルートを差し替える引数を持つ (違反フィクスチャで下限が効くことを確かめるため)。

#### 4.1.3 MCP サーバ `mye_mcp.exe` (sub-02〜04)

- 起動引数: `--root <repo>` (省略時: exe の場所から上へ `MyEngine.sln` を探す → 無ければカレントから上へ)、`--project <dir>` (notes の書き先の切替、4.1.6)、`--index <path>` (省略時 `<root>\cache\engine_index.json`。mcp_verify がフィクスチャを渡す)。root が見つからなくてもプロセスは起動し、道具は `isError` で理由を返す (縮退。計画書 §3「無ければ利用不可に縮退」)。
- transport (`stdio.rs`): stdin から 1 行 = 1 JSON を読み、応答を stdout に 1 行で書く。stdout には JSON-RPC 以外を一切書かない。診断は stderr。stdin の EOF で終了。
- handler (`handler.rs`): transport を知らない。`fn handle(&mut Context, Request) -> Option<Response>` (通知は `None`)。
- JSON-RPC: 解析不能 → `-32700` (id null)、配列 (バッチ) → `-32600`、未知メソッド → `-32601`、引数不正 → `-32602`。通知 (id なし) には応答しない。
- MCP 時代の判定 (リクエスト単位):
  - legacy: `initialize` の `protocolVersion` が `2025-11-25` / `2025-06-18` / `2025-03-26` ならそれを返し、それ以外なら `2025-11-25` を返す。capabilities は `tools: {}`、`serverInfo` (name `mye-engine`、crate の version)。`notifications/initialized` は無応答。`ping` は `{}`。
  - modern: `params._meta["io.modelcontextprotocol/protocolVersion"]` があるリクエスト。`2026-07-28` なら処理し、結果に `"resultType": "complete"` を付ける。list 系 (`tools/list`) の結果には `ttlMs: 3600000` と `cacheScope: "public"` も付ける (Claude Code 2.1.300 はこれが無い tools/list を拒否した。sub-02 で実測。道具表はプロセスの寿命の間は固定なので 1 時間)。legacy の結果には付けない。それ以外の版は `-32022` (`data.supported` / `data.requested`)。`server/discover` は `supportedVersions` (`["2026-07-28", "2025-11-25", "2025-06-18", "2025-03-26"]`)、`capabilities`、`_meta["io.modelcontextprotocol/serverInfo"]` を返す。
- `tools/list`: 固定順 (search, describe, verify_job, note_read, note_append。そのサブで未実装の道具は載せない)。各道具に `inputSchema` (`additionalProperties: false`)。
- `tools/call`: 結果は `content: [{type: "text", text: <JSON 文字列>}]` + `structuredContent` (同じ JSON)。道具の実行エラー (引数の値が不正、索引なし等) は `isError: true` で返す。未知の道具名は `-32602`。

#### 4.1.4 `search` / `describe` (sub-02)

- `search {q, kind?, limit?}`: `q` は 1〜200 文字。`kind` は `component` / `field` / `cli` / `abi` / `adr` / `spec` / `golden` / `note` の部分集合 (省略 = 全部)。`limit` 既定 20、上限 100。
- スコア: 完全一致 100 > 前方一致 80 > 部分一致 60 > `jp` / `tooltip` / l10n の訳語での一致 40 > 同義語展開での一致 30 (英字は大小無視)。手書きの訳語表 `tools\mcp\synonyms_extra.tsv` での一致は、l10n の訳語と同じ扱いで 40 にする (30 だと field の jp 一致に埋もれて top-5 に入らなかった。sub-02 で実測)。機械展開は l10n の短いラベル (40 文字以下) の英単語を、名前の語の先頭で引く。同点は kind の固定順 → id の辞書順 (決定的)。
- 返却: `{stale, total, hits: [{kind, id, score, jp?, src, ...種別ごとの短い付帯 (typeId 等)}]}`。`src` は必ず付ける (`engine` / `notes`)。
- `stale`: 索引の `sources` のどれかの最終更新時刻が記録値と違えば true (MCP は再生成しない。計画書 §3)。
- 索引が無い / 読めない → `isError` で「`pwsh -File tools\gen_engine_index.ps1` を実行」と返す。
- `describe {id}`: 完全一致で 1 件を引く (同名が複数 kind にあれば全部)。component なら fields 全部 + 言及している spec 見出し / ADR。CLI フラグなら層 (engine / showcase / editor)・値の個数・言及している spec 見出し・そのフラグを引数に含む golden。golden なら引数と、引数に出る CLI フラグ。ABI スロットなら番号・引数個数。見つからなければ `isError` + 上位 5 件の候補。 言及 (spec 見出し / ADR) は、`Name` や `Light` のような一般語の component だと多数の節に当たる。そのため 1 種別につき最大 20 件 (spec は行番号順) に絞り、総数を `mentionsTotal` で返す。
- 同義語: `l10n` の en/ja 対から機械生成 (+ 必要なら `tools\mcp\synonyms_extra.tsv`)。表の決め方は 2. の手順。

#### 4.1.5 `verify_job` (sub-03)

- `verify_job {op: "start", job, ticks?}` → `{jobId}`。`job` は 21 名の enum (4.4 / 2.)。enum 外は `isError` で、シェルには一切届かない。`ticks` は 1〜100000 の整数、省略時 600。子の環境変数 `MYE_RV_TICKS` で渡す (引数には載せない)。
- 前提の確認: `bin\x64\Debug\Editor.exe` と `bin\x64\Release\Editor.exe` が在ること (ビルド済み前提。無ければ `isError`)。同名ジョブが走行中なら `isError`。走行中が 2 本なら `isError` (busy)。
- 子の起動: `cmd.exe` に `raw_arg` で `/c chcp 437 >nul & call tools\replay_verify.bat --job <name> > "<log>" 2>&1` (run_parallel.ps1:85 と同じ形。`<log>` は `cache\mcp_logs\<name>.log` の絶対パス)。カレント = リポジトリのルート、`CREATE_NO_WINDOW`、stdin/stdout/stderr = null。ログのディレクトリは事前に作る。
- `verify_job {op: "poll", jobId}` → `{state: "running", elapsedSec}` または `{state: "done", exitCode, elapsedSec, summary}`。未知の jobId は `isError`。
- `summary` は終了後にログを読んで作る (stdout はパースしない): `result` (`PASS` / `FAIL`、exitCode から)、`failedStages` (`[FAIL] ` で始まる行の配列)、`stage` (最初の `[FAIL]` 行)、`firstMismatchTick` (`HASH MISMATCH at tick N` の最初の N。無ければ null)、`diagnostic` (`[diag] ` で始まる行の配列)、`errors` (`[ERROR]` 系の行、最大 20 行)、`fieldDiff: null` と `hint` (全体実行の `tools\replay_verify.bat` がフィールド差分まで出す旨)、`logPath`、`logBytes`。ログが無ければ `summary.result = "FAIL"` + `noLog: true`。
- ジョブの表はプロセス内メモリ。jobId は起動順の連番 (`jb_01` ...)。

#### 4.1.6 `note_read` / `note_append` (sub-04)

- 書き先: `--project` 指定時 `<project>\.mye\notes\<topic>.md`、無ければ `<root>\.mye\notes\<topic>.md`。ディレクトリを作るとき同じディレクトリに `.gitignore` (中身 `*`) を置く。
- `note_append {topic, title?, text}`: 末尾に `\n## <UTC ISO 8601 秒まで、Z 付き> <title>\n\n<text>\n` を追記する (title 省略時は見出しが時刻だけ)。ファイルは追記モードでのみ開く (切り詰め・上書き・削除・改名の手段を持たない)。`text` は 1〜65536 バイト、`title` は 1 行 120 文字以内。返却 `{path, bytes}` (bytes = 追記後のファイルサイズ)。
- `note_read {topic?, latest?}`: topic 省略 = 一覧 (`[{topic, bytes, lastHeading}]`、topic 名順)。topic 指定 = 最新 `latest` 件 (既定 5、上限 50) の見出しと本文。返却は 64 KB で打ち切り (`truncated: true`)。
- ガード (どれかに当たれば `isError`、ファイルは作らない): topic が `^[A-Za-z0-9_-]{1,64}$` でない / Windows の予約デバイス名 (大小無視) / notes ディレクトリかファイルがシンボリックリンク・ジャンクション / 正規化後の親ディレクトリが notes ディレクトリでない。
- `search` は notes の topic 名とエントリ見出しも引く (`src: "notes"`、kind `note`)。

### 4.2 データ・保存形式・互換性

- `cache\engine_index.json`: `schema: 1`。gitignore 済み (`cache/`)。版を変えたら MCP は読めない旨を `isError` で返す。
- notes: Markdown、追記のみ。gitignore (`.mye/` + notes 内 `.gitignore`)。
- ABI (`src\Shared\`)、シーン / スナップショット / .rep の形式、TypeId は**一切変えない**。
- `tools\mcp\Cargo.lock` はコミットする (collab と同じ)。`tools\mcp\target\` は gitignore。

### 4.3 UI / ビジュアル

なし (エディタの画面は変えない)。`--dump-index` は窓を出さない。verify_job の子が起動する Editor.exe の窓が出るのは全体実行と同じ。

### 4.4 非機能

- 決定性: sim に触れない。`--dump-index` の出力は決定的 (4.1.1)。`search` の並びは決定的 (4.1.4)。
- 層: `--dump-index` の実装は `src\Editor\` に置く。Engine / Runtime / GameLogic / Shared / Server から include しない。
- 依存: crate の依存は `serde` / `serde_json` のみ。edition 2021、`panic = "unwind"`。
- 規則 15 (`tools\check_rules.ps1`):
  - 15-a (sub-01): `src\Engine` / `src\Runtime` / `src\GameLogic` / `src\Shared` / `src\Server` が索引ダンプの実装ヘッダを include しない。
  - 15-b (sub-01): 索引ダンプの実装が sim を動かすヘッダ (World / EngineLoop / TickRunner 等、許可リスト方式) を include しない。
  - 15-c (sub-02 で器を作り、sub-03 で verify.rs を許可): `tools\mcp\src\` で子プロセス起動 (`Command::new` 等) が許可ファイル以外に現れない。
  - 15-d (sub-02 で器を作り、sub-03 で verify.rs の `create_dir_all` のみ、sub-04 で notes.rs を許可): `tools\mcp\src\` でファイル書き込み系 API (`fs::write` / `File::create` / `OpenOptions` / `create_dir` / `create_dir_all` / `rename` / `remove_*` / `copy` / `set_permissions` / `hard_link` 等) が許可ファイル以外に現れない。
  - 15-e (sub-03): replay_verify.bat の `:job_*` 集合 == `-Jobs` 列の集合 == `tools\mcp\src\verify.rs` のジョブ表。
  - 15-f (sub-01): `gen_engine_index.ps1 -CheckOnly` が成功する。
- ローカライズ: エディタ UI は変えないので `Tr()` の追加なし。
- CI / `--selftest`: MCP crate と mcp_verify は入れない。規則 15 は静的検査なので CI の check_rules で走る (MCP のビルドも実行もしない)。

## 5. 受け入れ条件

「check_rules OK」= `pwsh -NoProfile -File tools\check_rules.ps1` の ERROR が既知 3 件 (rule 1 の `src\Editor\App\EditorMain.cpp` 1 件 + rule 14-b の `tools/engine_dist/Editor.bat` / `Setup.bat` 2 件) だけであること。

### M87a (sub-01)
1. `bin\x64\Debug\Editor.exe --dump-index <tmp>\a.json` が exit 0 で JSON を書き、ログに D3D デバイス作成行が出ない。components 件数 = 組み込み登録数 (現在 80 前後、script 型を除く)。— 実行 + `ConvertFrom-Json` で件数確認
2. 同じ exe で 2 回、Debug と Release で 1 回ずつ出した JSON がバイト同一。— `Get-FileHash` 比較
3. `pwsh -File tools\gen_engine_index.ps1` が `cache\engine_index.json` を書き、4.1.2 の全セクションを持つ。`abi` は version 30 / 178 スロット。セクション別サイズと全体サイズ (§8-1 の実測) を実装メモに記録。— 実行出力
4. `gen_engine_index.ps1 -CheckOnly` は Editor.exe を起動せず exit 0。golden 行を消した (または EngineAPI.h のスロットを減らした) 一時フィクスチャでは exit 1。フィクスチャは作業ツリーに残さない。— 実行 2 回
5. check_rules に規則 15-a / 15-b / 15-f が入り、check_rules OK。15-a / 15-b は違反を仕込んだ一時コピー (`-SourceRoot` 等) で ERROR が出ることを 1 回示す。— 実行
6. `Editor.exe --selftest` (Debug) が PASS し、`EngineIndexDumpSelfTest` が含まれる。Release ビルド成功。— 実行ログ
7. `EngineCliSelfTest` が PASS (アクセサ追加でパースが変わっていない)。— 6 に含む

### M87b (sub-02)
8. `tools\mcp\` が edition 2021 / `panic = "unwind"` / 依存 serde・serde_json のみで `cargo build --release` と `cargo test` が通る。`Cargo.lock` を追跡、`tools/mcp/target/` を gitignore。— 実行 + `git check-ignore`
9. JSON-RPC の誤り系 (解析不能 / バッチ / 未知メソッド / 通知) と legacy `initialize` (3 版の echo、未知版 → 2025-11-25) と modern (`server/discover`、`_meta` 付き tools/list、未知版 → -32022) が `tests\mcp\*.ndjson` の期待どおり。— mcp_verify
10. `tools/list` の並びと `inputSchema` が期待どおり (search / describe のみ)。— mcp_verify
11. フィクスチャ索引 (`tests\mcp\fixture\engine_index.json`) に対する search (完全一致 / 前方 / 部分 / jp / 同義語 / kind 絞り / limit / 同点の並び / 索引なし → isError / stale) と describe (component / cli / golden / 未知 → 候補) が期待どおり。— mcp_verify
12. 実索引で固定 10 問 (下記) が top-5 に期待 id を含む: 「音の遮蔽」→ `Acoustic` で始まる component、「剛体」→ Rigidbody、「経路探索」→ `NavMesh` で始まる component、「霧」→ Fog、「破壊」→ Destructible か FracturePiece、「ラグドール」→ Ragdoll、「地形」→ Terrain、「ビヘイビアツリー」→ BehaviorTree、「スクロール」→ UIScrollRect か UIScrollbar、「タイムトラベル」→ `--timetravel-selftest`。機械生成表のみの命中数と、手書き表の行数を実装メモに記録。最終構成で 10/10。— 実索引に対するスクリプト実行 (索引は gen で作り直してから)
13. `tools\mcp_verify.bat` が cargo test + build + 全シナリオを回して exit 0。`--update` で期待を撮り直せる。CI ワークフローは触らない。— 実行 + `git diff .github`
14. 規則 15-c / 15-d の器が入り (許可ファイル 0)、違反を仕込んだ一時コピーで ERROR が出ることを 1 回示す。check_rules OK。— 実行
15. 実クライアント: `claude` CLI に `--mcp-config` で mye_mcp を読ませ、`search` を 1 回呼べたこと。使う設定ファイルは、コミットする `.mcp.json` そのもの (条件 30) を推奨。一時ファイルでもよい。どちらの時代で繋がったかを stderr ログで記録する。実行できなければ「未検証」と書く。— 実行ログ (§8-3 の実地確認)
30. リポジトリのルートに `.mcp.json` があり、サーバ名 `mye-engine`、コマンドは `tools\mcp\target\release\mye_mcp.exe` を指す (JSON として妥当)。コマンドの相対パスが Claude Code のどこを起点に解決されるか (リポジトリのルートで起動した場合 / サブディレクトリで起動した場合) を実測して記録する。サブディレクトリで解決できないなら、その制約を docs に書く (絶対パスは他人の環境で壊れるので使わない)。— 実行ログ
31. 未ビルド時の見え方: exe が無い状態 (`target` を一時的に別名にする等。作業後に戻す) で Claude Code がこのサーバをどう表示するかを実測して記録する (`claude mcp list` の表示、またはセッションの MCP の状態表示)。Claude Code 本体は止まらず、他の機能が使えること。`docs\mcp-server.md` の「未ビルドのとき」の節 (sub-04) が、この実測結果とビルドの 1 行 (`cargo build --release --manifest-path tools\mcp\Cargo.toml`) に一致すること。— 実行ログ + sub-04 で文書と照合

### M87c (sub-03)
16. ジョブ表が 21 名で、規則 15-e が三者一致を見る。bat に架空の `:job_zzz` を足した一時コピーで ERROR。— 実行
17. 子のコマンド文字列が `/c chcp 437 >nul & call tools\replay_verify.bat --job <name> > "<log>" 2>&1` と文字列一致 (unit test)。`CREATE_NO_WINDOW`・stdio null・カレント = root。— cargo test + コードの該当行
18. 引数の拒否: enum 外の job (`demo & calc` / `../x` / 空) と ticks 範囲外 → isError、子を起動しない。同名走行中 / 3 本目 → isError。— mcp_verify (起動しない系) + 実行
19. ログ構造化: PASS / stress 失敗 (plain 失敗・plain 成功) / Release 失敗 / ログなし の固定ログ (`tests\mcp\logs\`) から期待どおりの summary。— cargo test
20. 実ジョブ: MCP 経由で `rules` と `demo` (ticks 60) を start → poll し、`state: done` / `exitCode: 0` / `result: PASS`。その間 MCP の stdout に JSON-RPC 以外の行が無い。— 実行スクリプトの出力
21. §8-2: MCP をコンソールから起動してジョブを 1 本完走させた前後で、親コンソールのコードページが変わらない (`chcp` の出力を前後で比較)。ツール環境にコンソールが無く測れなければ、その旨と代替の観測を書き「未検証」とする。— 実行ログ
22. check_rules OK、mcp_verify exit 0。

### M87d (sub-04)
23. note_append → note_read の往復 (追記が積まれる、latest 件数、一覧の並び、64 KB 打ち切り)。時刻は正規化して比較。— mcp_verify (書き先は一時プロジェクト `--project cache\mcp_verify\...`)
24. パストラバーサル拒否の固定テスト: topic `../x` / `..\x` / `C:\x` / `/x` / `a/b` / `a.md` / `con` / `NUL` / `Com1` / 空 / 65 文字 / 非 ASCII → すべて isError、notes ディレクトリの外にファイルが増えない (前後のファイル一覧比較)。ジャンクションにした notes ディレクトリへの書き込みも拒否。— mcp_verify
25. 上書き・削除の手段が無い: notes.rs のファイルオープンが追記モードだけ (規則 15-d の許可が notes.rs のみ、unit test で既存内容が保たれる)。— cargo test + check_rules
26. `.gitignore` に `.mye/`、notes ディレクトリに `.gitignore` (`*`)。`git status` に notes が出ない。— 実行
27. search が notes を `src: "notes"` で返す。— mcp_verify
28. `docs\mcp-server.md` (ビルド、`.mcp.json` による登録とその初回承認、未ビルドのときの見え方 (条件 31 の実測と一致)、相対パスの起点の制約 (条件 30)、道具、notes の住み分け表、全体実行と同時に叩かない注意)、ADR-032、計画書冒頭の現在地、`docs\test_checklists.md` の手動確認項目。リンク切れなし。— 目視 + パスの存在確認
29. 最終: check_rules OK、mcp_verify exit 0、`Editor.exe --selftest` (Debug) PASS。— 実行

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | `--dump-index` と索引生成スクリプト、規則 15-a/b/f | なし | 1〜7 | `M87a: エンジン索引のダンプ (--dump-index) と gen_engine_index.ps1、規則 15 を追加する` |
| sub-02 | MCP crate 骨格 (stdio JSON-RPC、dual-era) と search / describe、mcp_verify | sub-01 | 8〜15, 30, 31 | `M87b: MCP サーバの骨格と search / describe、mcp_verify、.mcp.json を追加する` |
| sub-03 | verify_job (start / poll、ログの構造化) | sub-02 | 16〜22 | `M87c: MCP の verify_job で replay_verify のジョブを起動しログを構造化する` |
| sub-04 | notes (note_read / note_append) とパストラバーサル拒否、文書 | sub-02 (sub-03 と並列可だが check_rules 規則 15 と handler の道具表を両方が触るので、司会は sub-03 の後に回す) | 23〜29 | `M87d: MCP の notes (追記のみ) とパストラバーサル拒否、ADR-032 を追加する` |

順序の理由: sub-01 は MCP なしで単独で効く (計画書 §7)。最もリスクの高い未知 (MCP の時代の違いで実クライアントに繋がらない) は sub-02 の受け入れ条件 9 / 15 で潰す。§8 の 3 実測は 1 → sub-01 (条件 3)、2 → sub-03 (条件 21)、3 → 本書 2. で机上確認済み + sub-02 (条件 15) で実地確認。

## 7. 未決事項・リスク

- MCP の modern 時代 (2026-07-28) は仕様が新しく、手元の Claude Code の実装と細部 (tools/call の `_meta` の要否、`resultType`) が食い違う可能性。条件 15 で実地確認し、違えば sub-02 内で直す。
- verify_job の子は MCP 終了後も走り続ける。ハングした Editor.exe を止める手段が MCP に無い (タスクマネージャで止める)。docs に書く。
- 全体実行の `replay_verify.bat` と verify_job を同時に叩くと `cache\*.rep` を奪い合う。検出しない。docs に書く。
- `stale` は最終更新時刻の比較なので、git checkout で時刻が戻るケース等は取りこぼしうる (誤って fresh)。v1 は許容。
- `search` の limit 既定 20 は仮置き。sub-01 の実測サイズ次第で sub-02 着手前に見直す。

## 8. 変更履歴

(確定後の変更のみ)

- 2026-10-15 (ユーザー): `.mcp.json` をコミットする (planner の裁定「コミットしない」を覆した)。2. の該当行、3. のスコープ、5. の条件 15 / 28 を直し、条件 30 / 31 (相対パスの起点、未ビルド時の見え方) を足した。担当は sub-02 (サーバを作るサブなので、実測できるのはここ)。docs との整合は sub-04 の条件 28 で見る。その他の `[ユーザーに聞ける]` 5 件はすべて裁定どおりと回答があった (ジョブ数 21 も確認済み)
- 2026-10-15 (sub-01 round 1 の coder の質問): describe の言及を 1 種別 20 件 + `mentionsTotal` に絞ることを 4.1.4 に追記した。一般語の component 名で返却が膨らむため。索引側 (mentions) は全件のまま
- 2026-10-15 (sub-02 round 1 の coder の SELF_EVAL): 4.1.3 の modern の tools/list に `ttlMs` / `cacheScope` を足した (実クライアントの要求)。4.1.4 の手書き訳語表を 40 点とし、機械展開の規則と search の `total` を明記した。同義語表は M87b の計測で決まった (ユーザー回答「87b で決める」の決着): 機械生成のみでは 5/10、手書き 5 行 (剛体 / 経路 / 霧 / 破壊 / タイムトラベル) で 10/10。実クライアントは Claude Code 2.1.300 で、modern で接続した。探りで落ちると legacy で再接続する作りなので、dual-era の裁定は妥当だった。相対パスの起点はクライアントの cwd (条件 30 の実測)
- 2026-10-15 (planner): 固定 10 問の計測スクリプトがコミットされていなかったので、sub-04 のやることに「mcp_verify に実索引モードとして入れる (索引が無ければ SKIP、CI には入れない)」を足した
