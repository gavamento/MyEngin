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

## フィードバック履歴
