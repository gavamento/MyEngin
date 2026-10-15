# sub-04: notes (note_read / note_append) とパストラバーサル拒否、文書

- 依存: sub-02 (check_rules 規則 15 と handler の道具表を sub-03 と両方が触るので、司会は sub-03 の後に回す)
- 状態: 未着手
- 往復: 0

## やること

spec 4.1.6 / 4.4 (規則 15-d の notes.rs 許可) と文書を仕上げる。

1. `tools\mcp\src\notes.rs`: 書き先の決定 (`--project` → `<project>\.mye\notes`、無ければ `<root>\.mye\notes`)、topic のガード、追記のみのオープン、`.gitignore` (`*`) の設置、note_read の一覧 / 最新 N 件 / 64 KB 打ち切り、UTC 時刻 (std の `SystemTime` から暦計算。依存を足さない)。
2. `search` に notes (topic 名とエントリ見出し、`src: "notes"`、kind `note`) を足す。
3. `tests\mcp\` に notes のシナリオとパストラバーサル拒否の固定テスト (spec 条件 24 の列挙を全部)。書き先は一時プロジェクト (`cache\mcp_verify\...`)。時刻は正規化して比較。ジャンクションのケースは mcp_verify の ps1 側で `New-Item -ItemType Junction` で作り、終わったら片付ける。
4. 規則 15-d の許可に notes.rs を足す。あわせて、verify.rs の許可を spec 4.4 のとおり `create_dir_all` だけに絞る (sub-03 ではファイル単位で全 API を許していた)。verify.rs に `fs::write` を仕込んだ一時コピーで ERROR が出ることを 1 回示す。
5. `.gitignore` に `.mye/`。
7. 固定 10 問 (spec 条件 12) の計測を `tools\mcp_verify.ps1` の実索引モード (例: `-RealIndex`) として入れる。`cache\engine_index.json` が無ければ SKIP と表示して exit 0 にする。CI には入れない。sub-02 の scratch のスクリプト (未コミット) の代わり。reviewer が再現できるようにするため。
6. 文書: `docs\mcp-server.md` (ビルド、`.mcp.json` による登録と Claude Code の初回承認、「未ビルドのとき」の節 (sub-02 の実装メモにある条件 31 の実測をそのまま写し、ビルドの 1 行 `cargo build --release --manifest-path tools\mcp\Cargo.toml` を添える)、相対パスの起点の制約 (条件 30 の実測)、`--project` を付けたいときの個人設定の例 (`claude mcp add --scope local ...`)、道具の一覧と引数、notes の住み分け表 (計画書 §6)、全体実行の replay_verify と verify_job を同時に叩かない注意、走行中ジョブは MCP 終了後も止まらない注意)、`docs\adr\ADR-032-engine-mcp-server.md` (短く。正本は計画書へリンク)、計画書 `plans\エンジンMCPサーバ.md` 冒頭の「現在地」と §10 の未決の消込、`docs\test_checklists.md` に手動確認 (実クライアントからの接続・verify_job 1 本・note の往復)。

## やらないこと (このサブでは)

- notes の上書き・削除・改名、git 追跡
- ユーザーの Claude Code 設定の変更 (`.mcp.json` は sub-02 でコミット済み)
- `AGENTS.md` / `CLAUDE.md` の変更 (住み分け表は docs と ADR に置く)

## 触る場所 (planner の見立て)

- `tools\mcp\src\notes.rs` (新規)、handler の道具表、search への notes 追加
- `tests\mcp\*.ndjson`、`tools\mcp_verify.bat` (または ps1)
- `tools\check_rules.ps1` (15-d)、`.gitignore`
- `docs\mcp-server.md` / `docs\adr\ADR-032-engine-mcp-server.md` (新規)、`plans\エンジンMCPサーバ.md`、`docs\test_checklists.md`
- 罠: Rust の `Path::canonicalize` は Windows で `\\?\` 付きを返す → notes ディレクトリ側も canonicalize してから比較する。`symlink_metadata().file_type().is_symlink()` がジャンクションで true になるかは実測で確かめる (ならなければ別の判定を足す)。予約デバイス名は `CON` `PRN` `AUX` `NUL` `COM1`〜`COM9` `LPT1`〜`LPT9` (大小無視)。

## 受け入れ条件 (このサブ)

spec 5. の 23〜29。

23. note_append → note_read の往復 (追記が積まれる、latest、一覧の並び、64 KB 打ち切り)。
24. パストラバーサル拒否の固定テスト (`../x` / `..\x` / `C:\x` / `/x` / `a/b` / `a.md` / `con` / `NUL` / `Com1` / 空 / 65 文字 / 非 ASCII / ジャンクション) がすべて isError、notes の外にファイルが増えない。
25. ファイルオープンが追記モードのみ (unit test で既存内容が保たれる)、規則 15-d の許可が notes.rs のみ (+ sub-03 の verify.rs `create_dir_all`)。
26. `.gitignore` に `.mye/`、notes ディレクトリの `.gitignore` (`*`)、`git status` に notes が出ない。
27. search が notes を `src: "notes"` で返す。
28. 文書 4 点が揃い、リンク先のパスが存在する。`docs\mcp-server.md` の exe のパス・サーバ名・未ビルド時の症状が、`.mcp.json` の中身と sub-02 の実測 (条件 30 / 31) に一致する。
29. 最終: check_rules OK、mcp_verify exit 0、`Editor.exe --selftest` (Debug) PASS。

## 検証コマンド

- `cargo test --manifest-path tools\mcp\Cargo.toml`
- `tools\mcp_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `bin\x64\Debug\Editor.exe --selftest`
- `git status --short` (notes が出ないこと)

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)

実装:
- tools\mcp\src\notes.rs (新規): 書き先決定 (`--project` → `<project>\.mye\notes`、無ければ root)、topic ガード (`[A-Za-z0-9_-]{1,64}` + 予約デバイス名)、`.mye` / `notes` / topic ファイルのリンク拒否 (`is_symlink` + Windows の再解析ポイント属性)、canonicalize での親ディレクトリ検査、`open_append` (ファイルオープンはここだけ。append(true)、`.gitignore` は append + create_new)、UTC 時刻 (SystemTime から Hinnant の暦計算)、note_read (一覧 / 最新 N 件 / 64 KB で古い側を落とす。1 件目だけは本文を切る)、search 用 `headings`。unit 12 本。
- tools.rs: `note_read` / `note_append` を tools/list の末尾に追加し call に枝。search が notes を引く (kind が空か note を含むときだけ読む)。handler.rs: `Context.project` と `notes_base()`。main.rs: `--project`。
- search.rs: `search()` に `notes: &[NoteHeading]` を足し、kind `note` / `src: "notes"` を返す。id は topic、エントリは `<topic>: <見出し>`。unit 1 本追加。
- tools\check_rules.ps1: 15-d の許可を (ファイル x API) に変更。verify.rs = create_dir_all のみ、notes.rs = create_dir_all + OpenOptions。
- tools\mcp_verify.ps1: `{PROJ}` / `# junction` / `# expect-untouched` / `@@REP:c:n@@` / 出力の正規化 (UTC 時刻 → `<TS>`、100 回以上の同一文字 → `文字*回数`) / `-RealIndex` (固定 10 問、索引が無ければ SKIP exit 0) / notes_test の掃除。
- tests\mcp: notes / notes_truncate / notes_traversal / notes_noproject / notes_junction_{notes,mye,file} の 7 シナリオ + 期待。tools_list の期待を撮り直し (note_* 追加 + 下記の verify_job 説明の修正)。他の既存期待は無変更。
- .gitignore に `.mye/`。docs\mcp-server.md、docs\adr\ADR-032-engine-mcp-server.md、plans\エンジンMCPサーバ.md (現在地と §10 消込)、docs\test_checklists.md (M87 節)。

仕様との差分:
- [追加] verify_job の description の `tools\replay_verify.bat` が Rust 文字列の `\r` (CR) になっていた sub-03 の不具合を `\\` に直した (tools_list の期待に出ていた)。
- [追加] note_append の返す `path` はプロジェクトからの相対 (`.mye\notes\<topic>.md`)。環境依存の絶対パスを出さないため。
- [追加] `--project` が存在しないディレクトリなら note_* は isError (ディレクトリは作らない。typo で任意の場所に作らないため)。`note_read` の `latest` は topic 指定時のみ (topic 無しは isError)。title が空文字、text が空は isError。
- [追加] エントリの区切りは `## <UTC 時刻>` の形の行だけ (本文中の `## foo` は見出しにしない)。search は索引が読めないときは従来どおり isError (notes だけの検索はしない)。describe は notes 未対応 (engine のみ)。
- [追加] notes.rs の unit test の一時プロジェクトは `tools\mcp\target\notes_test\` に作り、削除は mcp_verify.ps1 が先頭で行う (15-d で notes.rs に削除 API を許さないため)。
- [追加] ジャンクションは `is_symlink()` 単独で true になることを実測 (Rust std)。念のため Windows では再解析ポイント属性も見る。
- [追加] 15-d の書式を「許可ファイルの一覧」から「ファイル x 許可 API の表」に変えた (verify.rs は create_dir_all のみ、notes.rs は create_dir_all + OpenOptions)。

検証:
- `cargo test --manifest-path tools\mcp\Cargo.toml` → 47 passed、warning なし。
- `tools\mcp_verify.bat` → exit 0 (15 シナリオ全 PASS)。`--update` は tools_list 以外の既存期待を変えなかった。
- 負の確認: `# expect-untouched` を notes シナリオ (書き込む) に付けた一時シナリオは FAIL になった。
- `pwsh -File tools\mcp_verify.ps1 -RealIndex` → 10/10。索引を一時退避すると SKIP、exit 0。
- `pwsh -NoProfile -File tools\check_rules.ps1` → ERROR は既知 3 件のみ。違反を仕込んだ一時コピー (`-McpSourceDir`) で verify.rs の `std::fs::write`、notes.rs の `fs::remove_file`、search.rs の `OpenOptions` がそれぞれ 15-d の ERROR。
- `Editor.exe --selftest` (Debug) → exit 0。
- 実 repo ルートで note_append → `git status` に `.mye` が出ない (`git check-ignore` が `.gitignore:59:.mye/` で `.mye/notes/*` と notes 内 `.gitignore` の両方を ignore)。確認後 `.mye` は削除。
- 未実施: 実クライアント (claude CLI) からの note / verify_job (test_checklists に記載)、`claude mcp add --scope local` の実行 (優先順位は未確認と文書に明記)。

自己採点 (1-5):
  仕様適合: 4 — 23〜29 を満たした。[追加] あり (path 相対、project 必須存在など)。
  正しさ: 4 — unit + 7 シナリオ + 実 repo の git 確認 + 負の確認。ハードリンク、検査後の差し替え (TOCTOU) は未対応・未検証。
  コード品質: 4 — notes.rs は責務が閉じている。search.rs は長いまま (分割は任意として見送り)。
  テスト: 4 — 15-d は違反コピーで確認、拒否は固定シナリオ。実クライアントは手動項目。

不安・質問:
- 15-d を (ファイル x API) の表にした。planner の意図 (API 単位に絞る) の解釈として問題ないか。
- search.rs の分割は見送った。

触ったファイル:
  .gitignore、docs\mcp-server.md、docs\adr\ADR-032-engine-mcp-server.md、docs\test_checklists.md、plans\エンジンMCPサーバ.md、tools\check_rules.ps1、tools\mcp_verify.ps1、tools\mcp\src\{notes,handler,main,search,tools}.rs、tests\mcp\tools_list.expected.ndjson、tests\mcp\notes*.ndjson と各 .expected.ndjson (14 ファイル)

申し送り:
- 本文中の `## <時刻>` 行は見出しとして読まれる (docs に記載)。検査後にリンクへ差し替える競合は対象外。
- plans\m87-mcp-server\harness.md / sub-03.md の未コミット変更、StartupSplash.cpp、docs\deepmodal\training-cleanup-20261015.md には触っていない。

## フィードバック履歴
- round 1: VERDICT OK (planner)。planner が再実行して確認: mcp_verify exit 0 (全シナリオ)、check_rules は既知 3 件のみ、docs 内のリンク先が実在、docs の条件 30 / 31 の記述が sub-02 の実測と一致。15-d の「ファイル x API」の表は指示の解釈として妥当
