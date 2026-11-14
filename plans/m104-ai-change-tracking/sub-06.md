# sub-06: セッション、操作、帰属 (M104g)

- 依存: sub-05
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。sub-05 の VERDICT で planner が見直してから着手する。

## やること

attr の全体と ipc §4.2 を作る。要求 Phase 2 の完了条件は「複数 AI や人間の編集が混在しても、根拠のない AI 帰属を確定しない」。

1. op: `session_begin` / `session_end` / `op_begin` / `op_end` / `report`。役割は ipc §3.1。
2. `session_begin` でセッションの基準 (その時点の全ファイルの内容ハッシュ) を持つ (attr §4)。`baseCommit` は `.git\HEAD` と参照ファイルを読むだけで決める (git を起動しない。packed-refs と detached HEAD を扱う。git でなければ空)。
3. `op_begin` で対象パスの内容を Blob に保存 (管理された操作の Before)。`declaredEdit` があれば、Before に当てた結果のハッシュを計算して持つ。置換 (`old_string` / `new_string` / `replace_all`) と全文書き込みの 2 形。`old_string` が Before に無い、または `replace_all` でないのに複数ある場合は「当てられない」として根拠を作らない。
4. 根拠 (attr §2 の 12 種のうち、このサブで出せるもの: `observed`、`op_window`、`before_match`、`declared_edit_match`、`report_hash_match`、`report_only`、`worktree`、`time_near`、`overlap`、`preexisting`)。`editor_saved` と `restore_expected` は op の受け口だけ用意するか、sub-09 / sub-15 に回すかを見直しで決める。
5. 区分の決定は attr §3 の表を上から順に。根拠が増えたら計算し直す (`attribution_updated` の通知は `subscribe` を作るサブで)。
6. 複数セッション (attr §5): `overlap`、候補を消さない、PID + 開始時刻。
7. 中断: セッションのプロセスが居なくなったら `interrupted`。再接続 (同じ `clientSessionId` の `session_begin`) の扱いを決めて attr に書く。
8. `list` の `sessionId` / `attribution` フィルタと、`sessions` を実データで返す。

## やらないこと (このサブでは)

- MCP 側 (sub-07)、フック CLI (sub-08)。このサブのテストは pipe の op を直接叩く。
- 作業ツリーの作成 (attr §6.2: サービスは作らない)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `session.rs`、`operation.rs`、`attribution.rs`、`declared_edit.rs`、`tests\attribution.rs`。`C:\HAL\MyEngin\tests\track\` にシナリオ。
- `C:\HAL\MyEngin\docs\ai-change-tracking\attribution-rules.md`、`ipc-contract.md` — 実装で決めた点。

## 受け入れ条件 (このサブ)

1. attr §3 の表の 9 行それぞれに、その行が採られるテストがある。表の順序 (上が優先) を確かめるテストがある → `cargo test`。
2. `session_begin` より前から作業ツリーにあった未コミットの変更は、そのセッションの変更に数えられない (`preexisting`)。git の HEAD との差を判定に使っていない → `cargo test` (要求 §16.1 の 3 番目)。
3. `op_begin` と `op_end` の間に、申告と違う内容を人が書いた場合、Verified にならない → `cargo test`。
4. 同じパスに 2 セッションの窓が重なると `overlap` が付き、Verified にならない。どちらにも `declared_edit_match` が無ければ `ai` / `unknown` で候補が 2 つ残る → `cargo test` (§16.2 の 5 番目)。
5. 同じ PID で開始時刻が違うプロセスは別のセッションになる → `cargo test` (§16.2 の 2 番目)。
6. 申告も窓も無い変更は `unknown` / `unknown`。`time_near` だけなら `inferred` で、幅を広げても上がらない → `cargo test` (§16.2 の 4 番目)。
7. `gap` の Revision は、その間にセッションが動いていても `unknown` → `cargo test`。
8. `report` の応答が、各パスの照合結果 (一致 / 不一致 / 観測なし) を返す → `tools\track_verify.bat`。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
