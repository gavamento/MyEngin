# sub-28: 閉じられない操作の窓で人の編集を AI のものにしない (M104v)

- 依存: sub-20 (最終レビュー round 1 の指摘への対応。M104 の全サブの後)
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: 詳細 (2026-11-16、REVIEW round 1 を受けて planner が書いた)
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。
- 出所: `C:\HAL\MyEngin\plans\m104-ai-change-tracking\review-1.md` の指摘 1 (major、planner 宛) と 2〜4 (minor、coder 宛)。

## 問題 (reviewer の再現。spec §2 D46)

フックの `pre` (`op_begin`) が届いて `post` (`op_end`) が来ない操作は、打ち切り (10 分) まで窓が開いたままになる。その間に人がそのファイルを変えると、根拠 `op_window` + `before_match` で `ai` / `claimed` になり、AI の操作とセッションに結び付く (`session_end` の後でも)。reviewer が実サービスで再現した: (a) Edit の `pre` だけ → 4 秒後に人が別の内容を書く → `ai` / `claimed`、`restorable: true` (b) 同じセッションで別ファイルの pre + post と `session-end` の後に人が書く → 同じく `ai` / `claimed`、終わったセッションの `changeCount` に数えられる。

**Claude Code の公式文書 (https://code.claude.com/docs/en/hooks 、2026-11-16 に planner が取得) で確かめたこと**:
- `PostToolUse` は道具の呼び出しが**成功した後**だけ。失敗の後は `PostToolUseFailure` (入力は `tool_name`、`tool_input`、`tool_use_id`、`error`、`is_interrupt?`、`duration_ms?`)。
- **利用者が許可の確認で拒否したとき (既定の権限のモード) は、`PreToolUse` は来るが、`PostToolUse` も `PostToolUseFailure` も `PermissionDenied` も来ない** (「Permission denials fire PreToolUse but not this event」「PermissionDenied … doesn't run when you manually deny a permission dialog」)。`PermissionDenied` は自動のモードの拒否だけ (入力に `tool_use_id` と `reason`)。
- `Stop` は Claude の応答が終わったとき。`SessionEnd` はセッションが終わったとき (1.5 秒の予算を共有)。
- つまり、**拒否された操作の終わりを知らせるイベントは無い**。イベントを足すだけでは塞げない。

## 方針 (planner の裁定。D46)

1. **帰属の規則を変える (本体)**: 操作の窓に基づく根拠 (`op_window`、`before_match`、`declared_edit_match`) は、**その操作が `op_end` の `result = ok` で閉じたときだけ**結論に数える。
   - 操作が開いている間 (`op_end` 待ち): 根拠の行は記録する (監査のため) が、結論には数えない。結論は他の根拠 (`time_near` → `inferred`、無ければ `unknown`) で決まる。`op_end` (ok) が届いたら、その操作の窓に入る Revision を計算し直して `verified` / `claimed` に上げる (sub-06 の「後から届く根拠」の仕組みを使う。`attribution_updated` の通知も出る)。
   - 操作が `ok` 以外で閉じた (失敗・拒否・打ち切り・ターンの終わり・セッションの終わり) とき: その操作の窓の根拠は結論に数えない。対象の Revision を計算し直す。
   - これで (a) は `inferred` か `unknown` に、(b) も同じになる。拒否の終わりが分からなくても、人の編集は AI に確定しない。
   - `report` (MCP の `change_report`、`report_hash_match` / `report_only`) は操作の窓と別の根拠で、この変更の対象外。
2. **開いた窓を閉じる契機を足す** (表示と寿命のため。帰属の正しさは 1 で保たれる):
   - フック CLI に `post-failure` (`PostToolUseFailure` → `op_end` の `result = failed`)、`denied` (`PermissionDenied` → `op_end` の `result = denied`)、`turn-end` (`Stop` → そのセッションの開いている操作を全部 `abandoned` にする) を足す。サービス側は `op_end` の `result` に `denied` を足し、ターンの終わりは新しい op (例 `session_turn_end`、`role = hook`、`clientSessionId`) か、既存の op の組み合わせで表す。
   - `session_end` で、そのセッションの開いている操作を全部閉じる (`abandoned`)。
   - 10 分の打ち切りは最後の守りとして残す。
   - **「同じセッションの次の `op_begin` で前の操作を閉じる」は採らない** (Claude は 1 つの応答の中で道具を並べて呼ぶことがあり、`pre` が 2 つ続いてから `post` が来うる)。
3. `docs\ai-change-tracking\attribution-rules.md` (§2.1 の `op_window` / `before_match` / `declared_edit_match` の定義、§3 の表の注記、§6.1)、`ipc-contract.md` (`op_end` の `result`、ターンの終わり、`session_end` の動き)、`hooks-setup.md` (設定例に `PostToolUseFailure`、`PermissionDenied`、`Stop` を足す。拒否のときはイベントが来ないこと、それでも AI のものにならないこと)、`acceptance.md` (要求 §16.2 の該当行) を直す。

## やること

### A. サービス (`tools\track`)

1. `attribution.rs`: 結論の計算で、操作の窓の根拠を、その操作の `result` が `ok` のときだけ数える。開いている・`ok` 以外の操作の根拠は数えない。`verify_gap` の内容だけの根拠 (D35) も同じ条件 (`op_end` が `ok`)。`overlap` の判定は、開いている操作も含めて今までどおり (確定を弱める側なので残す)。
2. `op_end` / 操作を閉じるすべての経路 (`op_end`、打ち切り、`session_end`、ターンの終わり) で、その操作の窓に入る Revision を計算し直す。根拠の行は消さない。
3. `op_end` の `result` に `denied` を足す。ターンの終わりの op を足す (`protocol.rs` の `IMPLEMENTED_OPS` と `role_allows`。`hook` と `mcp`)。`session_end` が開いている操作を閉じる。
4. イベント (`events`) を通し、作り直しで同じ結論になること (spec §2 D26)。スキーマは変えない (版 3)。足すなら v4。
5. **minor 2**: `serve --project <パス>` で、パスが存在しない・ディレクトリでないときは、何も作らずに 0 以外の終了コードで終わる (エラーの 1 行を標準エラーへ)。今は `.mye\ai_changes\` を作って動き続ける。

### B. フック CLI (`tools\track\src\hook.rs`)

- `hook post-failure` (入力は `PostToolUseFailure`)、`hook denied` (`PermissionDenied`)、`hook turn-end` (`Stop`) を足す。今までどおり: 終了コードは常に 0、標準出力は空、5 秒以内、サービスを起動しない (起動するのは `pre` / `session-start` だけ。sub-08)。
- `post` に `PostToolUseFailure` の入力が来た場合 (設定の誤り) も、`hook_event_name` を見て失敗として扱う。

### C. Editor と文書

- Editor 側の変更は要らない見込み (帰属の値の表示は今までどおり)。`attribution_updated` で一覧が更新されることは既存。
- **minor 3**: `C:\HAL\MyEngin\src\Editor\App\EditorApp.cpp` 153 行目付近の `rebuildDockLayout_ =!std::filesystem::exists(...)` を、元の書き方 (M104n より前の `= !`) に戻す。変更と関係の無い空白の差分を残さない。
- **minor 4**: `C:\HAL\MyEngin\docs\ai-change-tracking\performance.md` 111 行目付近の、機械に固有の絶対パスを、リポジトリからの相対に書き換える (規則 17 の WARN を基点の数に戻す)。
- 上の「方針」の 3 の文書。

## やらないこと (このサブでは)

- 拒否を知るための別の仕組み (トランスクリプトの読み取りなど)。Codex / Gemini のフック。
- 「次の `op_begin` で前を閉じる」(並列の呼び出しと矛盾する)。
- 下位の層 (`src\Engine` ほか) の変更。スキーマの変更 (要るなら v4)。
- reviewer の minor 5 (ゴーストのラベルの重なり、大きいファイル) — 台帳の申し送りへ回した。

## 受け入れ条件 (このサブ)

1. **reviewer の再現 (a)**: Edit の `pre` だけを送り、人が別の内容を書く → `ai` / `claimed` にならない (`inferred` か `unknown`)。根拠の行に `op_window` / `before_match` は残る (監査) が、結論に使われない。その Revision はセッションの一覧と `changeCount` に入らない → `cargo test` + `tools\track_verify.bat` (シナリオ)。
2. **reviewer の再現 (b)**: 同じセッションで別ファイルの pre + post と `session-end` を送った後に人が書く → (a) と同じ。`session_end` で開いている操作が閉じている → `cargo test` + シナリオ。
3. **正常な経路が変わらない**: pre → 宣言どおりに書く → post で `ai` / `verified` (sub-08 の受け入れ条件 1 と同じ)。post が観測より後に届いても、届いた時点で `verified` に上がり `attribution_updated` が出る → `cargo test`。
4. `post-failure` / `denied` / `turn-end` で操作が閉じ、その窓の人の編集が AI に確定しない。`turn-end` はそのセッションの開いている操作だけを閉じ、別のセッションの操作に触れない。どの失敗でも終了コード 0・標準出力が空 → `cargo test` (実際に exe を起動)。
5. 並列の呼び出し: pre (A) → pre (B) → 両方を書く → post (A) → post (B) で、両方が `verified` (次の `op_begin` で前を閉じていないこと) → `cargo test`。
6. 作り直し: 上の (a)(b) と 3 の後に DB を壊して起動し直すと、結論が同じ → `cargo test`。
7. minor 2: 存在しないパス・ファイルのパスで `serve` → 0 以外で終わり、何も作られない → `cargo test`。
8. minor 3: `git diff ca6a3991 -- src\Editor\App\EditorApp.cpp` に、その行の空白の差分が無い。minor 4: `tools\check_rules.ps1` の規則 17 の WARN が `performance.md` に無い。
9. 文書: attribution-rules / ipc-contract / hooks-setup / acceptance が新しい規則に合っている。hooks-setup の設定例の JSON がそのまま読める (`ConvertFrom-Json`)。
10. 既存の検証が通る: `cargo test --manifest-path tools\track\Cargo.toml` (既定。3 回)、`tools\build_track.bat Release` → `tools\track_verify.bat`、`cargo test --manifest-path tools\mcp\Cargo.toml --target-dir cache\track_verify\mcp_target`、`tools\check_rules.ps1` 0 error。`src` を触るのは minor 3 の 1 行だけなので、Editor のビルドと `MYE_SELFTEST_ONLY=changetracking` (Debug / Release) を回す (作業ディレクトリはリポジトリ)。期待値が変わるシナリオは、差分が新しい規則による変化だけであることを確かめてから撮り直す。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (3 回)
- `tools\build_track.bat Release` (`mye_track.exe` が動いていないことを確かめてから)、`tools\track_verify.bat`
- `cargo test --manifest-path tools\mcp\Cargo.toml --target-dir cache\track_verify\mcp_target`
- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)、`MYE_SELFTEST_ONLY=changetracking` で `--selftest` (Debug / Release)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。窓を出す起動はしない見込み (するなら作業ディレクトリをリポジトリの外に)。

## 実装メモ (coder が追記)

### round 1 (FIX_REQUEST: REVIEW round 1 の #1〜#4)

- #1: `attribution.rs` の `load_evidence` が、窓の根拠 (`op_window` / `before_match` / `declared_edit_match`) を、操作が `result = 'ok'` のときだけ結論に渡す。行は消さない。`session_evidence` は結論に数える窓 (`counted_window`) がある変更だけ `time_near` を抑える。`operation.rs` の `op_finish` は ok で閉じたとき前後の結論を比べ、変わった Revision に `kind = op_closed` の印の行を足す (通知 `attribution_updated` は根拠の行の追加から作るため。結論には使わない)。`close_open_ops` / `open_op_ids` を足し、`session_end` の反映と新しい `session_turn_end` イベントが開いている操作を `abandoned` で閉じる。`op_end` の `result` に `denied`。`session.rs` の `session_candidate_sql` も同じ条件 (一覧・`changeCount`)。フック CLI に `post-failure` / `denied` / `turn-end`。`post` は `hook_event_name = PostToolUseFailure` なら失敗。
- #2: `main.rs` の `serve` が `--project` のディレクトリでないとき、何も作らず終了コード 2。
- #3: `EditorApp.cpp` の `= !` に戻した (`git diff ca6a3991` に該当行の差分なし)。
- #4: `performance.md` の絶対パスを `<リポジトリのルート>` に。check_rules の規則 17 から消えた (0 error、WARN は 65)。
- 文書: attribution-rules (§2.1、新設 §2.2、§3 の表、§6.1)、ipc-contract (4.2 の表、9.7)、hooks-setup (設定例 + 終わりの来ない操作)、acceptance (16.2)、db-schema (events に `session_turn_end`、`op_end` の `denied`)。
- 検証: cargo test (tools\track) 3 回全通過、mcp 58 通過、build_track Release → track_verify 全 30 シナリオ通過 (12 の期待値は `time_near` と `op_closed` の行が増えるだけで撮り直し、新規 30)、Debug / Release ビルド (MyeWarnAsError) 通過、`changetracking` Debug / Release 通過、check_rules 0 error。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-16)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、`git status` は sub-28 の対象と review-1.md・台帳・spec だけ、`src` の差分は `EditorApp.cpp` の空白 1 か所で、基点との差分に該当行の変更は残っていない (#3 解消)、スキーマと `tools\mcp` に差分なし。planner の実走: `cargo test` (track) 303 件通過、`tools\build_track.bat Release` → `tools\track_verify.bat` 全シナリオ PASS (新規 30_hook_open_op を含む)、`tools\check_rules.ps1` 0 error で `performance.md` の規則 17 の WARN は消えた (#4 解消)。Editor のビルドと `changetracking` (Debug / Release) は coder の結果による。
  - 追加の判定: `op_closed` の印の行 (ok で閉じて結論が変わった Revision に足し、`attribution_updated` を出すため。結論・一覧には効かない) は採る。通知の仕組みとスキーマを変えない最小の形。開いている窓がある間の `time_near` (verified の根拠の一覧に `time_near` が増える) も採る。`session_turn_end` を開いている操作が無いときに記録しないのも採る。
  - nit (申し送り): Editor の根拠の一覧で `op_closed` が表示の名前を持たず生の文字列で出る (`ChangeFormat.cpp` の表に無い)。次に Editor を触るときに英日の名前を足すか、表示から外す。
  - 質問への回答: (1) `op_closed` の方式でよい。(2) `time_near` が増えるのはそのままでよい (結論は変わらない)。(3) 実機の Claude Code での拒否・失敗・Stop は未確認として残す。`session_interrupted` で開いている操作を閉じないのは、10 分の打ち切りで閉じるのでよい。