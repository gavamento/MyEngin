# sub-03: verify_job (start / poll、ログの構造化)

- 依存: sub-02
- 状態: OK (commit bc8f5d4)
- 往復: 0

## やること

spec 4.1.5 / 4.4 (規則 15-c の verify.rs 許可、15-d の verify.rs `create_dir_all` 許可、15-e) を実装する。

1. `tools\mcp\src\verify.rs`: 21 名のジョブ表 (固定の const 配列、replay_verify.bat の `-Jobs` 列と同じ順)、start / poll、ジョブ表 (プロセス内、連番 jobId)、同名走行中・同時 2 本の拒否、前提 exe の確認。
2. 子の起動: `std::process::Command::new("cmd.exe")` + `CommandExt::raw_arg("/c chcp 437 >nul & call tools\\replay_verify.bat --job <name> > \"<log>\" 2>&1")` + `creation_flags(CREATE_NO_WINDOW = 0x08000000)` + `current_dir(root)` + `env("MYE_RV_TICKS", ticks)` + stdin/stdout/stderr = `Stdio::null()`。ログは `<root>\cache\mcp_logs\<name>.log` (ディレクトリは `create_dir_all`)。終了の検知は `Child::try_wait` を poll 時に呼ぶ形でよい (待機スレッドは不要。collab の worker.rs の型を使うかは coder 判断)。
3. ログの構造化 (spec 4.1.5 の summary)。固定ログ `tests\mcp\logs\*.log` で unit test。固定ログは実ログの形 (`=== ... ===` / `[INFO ]` / `[ERROR]` / `[FAIL] ...` / `[diag] ...` / `[replay] HASH MISMATCH at tick N`) に合わせる — 実ログの行形式は `cache\replay_logs\*.log` と `replay_verify.bat` の `:chain` / `:chain_stress_fail` / `:jobsab_one` / `:computeab_one` で確認すること。
4. 規則 15-e (三者一致) と 15-c / 15-d の許可リスト更新。
5. 実ジョブ確認と §8-2 の chcp 実測。

## やらないこと (このサブでは)

- `replay_verify.bat` / `run_parallel.ps1` の変更 (CI 経路。1 文字も変えない)
- フィールド差分 (`:diagnose` 相当) の実行、キャンセル、タイムアウト
- ビルドの実行 (ビルド済み前提)
- `shot_verify.bat` のジョブ化

## 触る場所 (planner の見立て)

- `tools\mcp\src\verify.rs` (新規)、handler の道具表 (`tools/list` に verify_job を追加)
- `tests\mcp\logs\*.log`、`tests\mcp\*.ndjson` (起動しない系の拒否ケース。実ジョブを起動するケースは mcp_verify に入れない — 時間と環境依存のため。実ジョブは検証コマンドとして別に回す)
- `tools\check_rules.ps1` (15-e、15-c / 15-d の許可)
- 罠: 固定ログに日本語は不要 (ジョブ経路の echo は ASCII 限定)。ログはエンジンの出力も混ざるので UTF-8 として不正なバイトがありうる → lossy で読む。ログは数百 KB あるので全部を返さない。
- 罠: ジョブ名に `rules` があるが、これは check_rules.ps1 を pwsh で回すジョブ (Editor.exe 不要)。前提 exe の確認を `rules` にも課すかは coder 判断 (課さないなら理由を書く)。

## 受け入れ条件 (このサブ)

spec 5. の 16〜22。

16. ジョブ表 21 名、規則 15-e が三者一致を見る。架空の `:job_zzz` を足した一時コピーで ERROR。
17. 子のコマンド文字列の一致 (unit test)、`CREATE_NO_WINDOW`・stdio null・カレント = root。
18. enum 外の job (`demo & calc` / `../x` / 空) と ticks 範囲外 → isError、子を起動しない。同名走行中 / 3 本目 → isError。
19. 固定ログ (PASS / stress 失敗で plain も失敗 / stress 失敗で plain は成功 / Release 失敗 / ログなし) から期待どおりの summary (cargo test)。
20. MCP 経由で `rules` と `demo` (ticks 60) を start → poll し done / exit 0 / PASS。その間 MCP の stdout に JSON-RPC 以外の行が無い。
21. §8-2: ジョブ完走の前後で親コンソールのコードページ不変。測れなければ「未検証」+ 理由 + 代替の観測。
22. check_rules OK、mcp_verify exit 0。

## 検証コマンド

- ビルド済みの `bin\x64\Debug\Editor.exe` / `bin\x64\Release\Editor.exe` があることを確認 (無ければ MSBuild でビルド)
- `cargo test --manifest-path tools\mcp\Cargo.toml`
- `tools\mcp_verify.bat`
- 実ジョブ: scratch に置いたスクリプトで mye_mcp に `initialize` → `tools/call verify_job start` → `poll` を流す
- `chcp` 前後比較 (PowerShell ツールから。コンソールが無ければその旨)
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-03 (round 1)
- 実装: verify.rs (21 名 JOBS、start/poll、child_command_line/build_command、check_admission、summarize)、tools.rs (verify_job 追加)、handler.rs/main.rs (Context.jobs)、check_rules.ps1 (15-c/15-d に verify.rs、15-e、-ReplayVerifyBat)、tests/mcp/logs/*.log 4 本、tests/mcp/verify_job.ndjson、.gitignore に `!tests/mcp/logs/*.log`
- 差分: [追加] .gitignore 例外 (`*.log` で固定ログが無視されるため) / [追加] errors に `ERROR ` 始まり (rules ジョブ) も含める / [追加] start の返却に job/ticks/logPath、op ごとに許可引数を絞る / [判断] rules は Editor.exe 前提を課さない (check_rules を pwsh で回すだけ) / 条件 20 の rules は exit 1 (既知 3 件の ERROR のため。FAIL を正しく返した) / Release\Server.exe は前提に含めない (仕様どおり Editor 2 本のみ)
- 検証: cargo test 35 pass、mcp_verify 全 PASS、check_rules は既知 3 件のみ、15-e は :job_zzz 入り bat で ERROR、実ジョブ (demo/physics PASS exit 0、同名拒否・busy 拒否、stdout に非 JSON-RPC 0 行)、chcp は 65001 のまま前後不変 (コンソールあり)

## フィードバック履歴
- round 1: VERDICT OK (planner)。cargo test 35 passed、固定ログ 4 本が追跡に載ること、raw_arg / CREATE_NO_WINDOW / create_dir_all の箇所を planner が確認した。条件 20 は spec を直して達成扱い。15-d の許可の粒度 (should) は sub-04 で絞る
