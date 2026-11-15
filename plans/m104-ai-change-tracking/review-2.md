# review-2: m104-ai-change-tracking

- 対象コミット範囲: `ca6a3991..9aad35e7` (round 1 からの差分は `9aad35e7` M104v の 1 本。台帳 harness.md のハッシュの記入は未コミット)
- 日付: 2026-11-16
- reviewer: harness-reviewer (round 2)

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 の再現手順 (a)(b) を、実サービスと実フックでそのまま流し直した。どちらも unknown / unknown になり、AI の操作にもセッションにも結び付かない。変形も実サービスで確かめた: 並列の 2 操作 (post が逆順に届く)、post-failure、denied、turn-end (Stop)、pre の 300 ms 後の人の編集。存在しないパスで serve を起動すると、何も作らずに終了コード 2。新しい minor が 2 件 (指摘 6、7)。どちらも M104v で生じた挙動。
  機能性: 4 — cargo test (track) は全部 ok。新しく足された d46_* などの試験を含む。track_verify は 30 シナリオ全部 PASS (新しい 30_hook_open_op と、期待値が 2 行増えた 12_managed_edit を含む)。check_rules は 0 error / 65 warning (round 1 の rule 17 の 1 件が消えた)。check_rules_selftest も通った。正常な経路 (pre → 宣言どおり → post) は verified のまま。
  ビジュアルデザイン: 4 — round 1 から変わっていない (src の差分は空白 1 か所)。新しいスクショは無い。op_closed の表示名が無い件は既知の nit。
  コード品質: 4 — 変更は小さく、規則がコメントと文書 (attribution-rules.md §2.2、ipc-contract.md、hooks-setup.md、spec D46) にそろっている。「次の op_begin で前を閉じる」を採らなかった理由 (並列の道具呼び出し) も文書にある。minor は指摘 6、7。
指摘:
  6. [minor] 宛先: coder — `hook pre --project <存在しないパス>` が、5 秒の上限まで待ってから 0 で返る。serve が終了コード 2 で終わるようになった (#2 の修正) ので、フックはサービスの pipe を待ち続けて時間切れになる。設定の `--project` を綴り間違えると、Claude Code の Edit / Write のたびに約 5 秒待たされ、出る文は「5 秒を超えたので途中でやめる」で原因が分からない (何も作らないのは正しい) — 根拠: `'{"session_id":"s","tool_name":"Edit","tool_use_id":"t","tool_input":{...}}' | mye_track.exe hook pre --project %TEMP%\<存在しないパス>` → 標準エラー `mye_track hook: 5 秒を超えたので途中でやめる`、終了コード 0、パスは作られていない — 期待: フックが起動の前に `--project` がディレクトリかを確かめ、無ければ「--project がディレクトリではない」の 1 行で即座に 0 で返る (hooks-setup.md §3 / §4 に一言)。round 1 で書かなかった理由: #2 の修正 (serve が何も作らずに終わる) で新しく生じた挙動。
  7. [minor] 宛先: planner — 結論に数えない窓の根拠 (操作が開いたまま・failed・denied・abandoned の op_window / before_match) が、describe に印なしで並ぶ。Editor の詳細にも「AI の道具がこのパスを操作している間に起きた」「変更前の内容が、道具が操作の開始時に読んだ内容と一致した」(ChangeFormat の文面。round 1 の cache\m104k_shots\1_running_inline.png) のまま出る。一方で区分は unknown になる。利用者からは、根拠が AI を指しているのに不明と出ているように見え、なぜ数えないかの説明が無い (attribution-rules.md §8「区分と一緒に根拠の一覧を出す」) — 根拠: round 1 の手順 (a) を流し直した結果の `describe` (revisionId 1)。`"actor":"unknown","attribution":"unknown"` で、evidence に `op_window` と `before_match` (operationId = 閉じていない操作) がそのまま入っている。数えたかどうかを表す項目は無い — 期待: describe の evidence に「結論に数えていない (操作が ok で終わっていない)」を表す印を足すか、Editor が操作の結果を引いてその旨を添えるか、を仕様で決める。round 1 で書かなかった理由: D46 で新しく生じた「記録はするが数えない根拠」。
検証した手段:
  - 消込のための再現: round 1 と同じ手順で、一時プロジェクトに `tools\track\target\release\mye_track.exe serve` (06:12 のビルド。`cargo build --release` が何もしないことで最新と確認) を起動し、実フックを流した。
    (a) h.txt の pre だけ → 4 秒後に人の編集 → list の最初の Revision が `unknown/unknown`、operationId = null、describe の candidateSessions = []。同じ流れの中の k.txt (pre → 宣言どおり → post) は `ai/verified`。
    (b) h.txt の pre → 同じセッションで k.txt の pre + post → `hook session-end` → 3 秒後に h.txt を初めて変える → `unknown/unknown`、そのセッションの changeCount = 1 (k.txt だけ)。
  - 変形 (同じ駆動): a.txt と b.txt の pre を並べて書き、post を b → a の順に送る → 両方 `ai/verified`。c.txt は post-failure、d.txt は denied、e.txt は turn-end の後に人が書く → 3 件とも `unknown/unknown` (別のセッションの活動から 4 秒離したとき)。別のセッションの pre の直後 (0 から 2 秒) に人が書いた場合は `ai/inferred` (time_near。不明 / 推定の節に分かれる設計どおり)。
  - `mye_track.exe serve --project <存在しないパス>` と `--project <ファイル>` → どちらも「--project がディレクトリではない」、終了コード 2、`.mye` は作られない。
  - `cargo test --manifest-path tools\track\Cargo.toml` → 全部 ok。新しい試験の名前を diff で確かめた: a_human_edit_in_the_window_of_an_operation_that_never_ends_is_not_the_ais、a_human_edit_after_session_end_is_not_the_ais_even_when_another_operation_finished_ok、an_op_end_that_arrives_after_the_observation_raises_the_change_and_notifies、parallel_operations_are_both_verified_when_their_ends_arrive_in_order、a_failed_or_denied_end_and_a_turn_end_close_the_window_without_counting_it、failure_denied_and_turn_end_hooks_close_operations_and_never_hand_a_human_edit_to_the_ai、serve_with_a_missing_or_non_directory_project_creates_nothing_and_fails ほか。DB の作り直しで同じ根拠になることは、既存の a_broken_db_is_rebuilt_to_the_same_sessions_operations_and_evidence (ok で閉じた操作と、session_end で閉じた操作を含む。attribution_evidence を行ごとに比べる) が通ることで確かめた。
  - `tools\track_verify.bat` → 30 シナリオ全部 PASS。`tools\check_rules.ps1` → 0 error / 65 warning (performance.md:111 の WARN が消えた)。`tools\check_rules_selftest.ps1` → exit 0。
  - 読んだ diff: 9aad35e7 の tools\track\src 全部 (attribution.rs、operation.rs、ops_record.rs、session.rs、hook.rs、main.rs、protocol.rs、server.rs、revision.rs)、src\Editor\App\EditorApp.cpp (空白を戻しただけ)、performance.md、12_managed_edit の期待値、attribution-rules.md と ipc-contract.md の差分。
  - 決定性: `git diff ca6a3991..HEAD --stat -- src/Engine src/Shared src/Scripting/Interop.cs src/Runtime src/Server src/GameLogic` は round 1 と同じ (src\Engine の 4 ファイルだけ)。9aad35e7 の src の差分は EditorApp.cpp の空白 1 か所だけ。
  - 回し直さなかったもの (理由): Editor のビルド、`--selftest-full`、`replay_verify` (src の差分が空白 1 か所だけで、round 1 で両構成の --selftest-full と replay_verify が PASS している)。mcp_verify / make_dist / 対話起動 / CI / 本物の Claude Code は round 1 と同じ理由で未実行 (本物の Claude Code での拒否・失敗・Stop のフックの実走は未確認のまま)。
  - 片付け: 自分が作った %TEMP%\mye_review1_e2e_* (4 個)、%TEMP%\mye_review2_*、%TEMP%\svc_err_review1.txt を削除した。起動したサービスは全部 shutdown で終わったことを確かめた (mye_track.exe の残りなし)。リポジトリに .mye\ai_changes は無い。
前回指摘の消込:
  1. [major] 開いたままの窓で人の編集が ai / claimed になる — 解消 (round 1 の (a)(b) を実サービスで流し直して unknown/unknown。並列・failure・denied・turn-end の変形も確かめた。正常な経路の verified は保たれている)
  2. [minor] serve が存在しないパスに .mye を作る — 解消 (終了コード 2、何も作らない。ただし修正で生じたフック側の待ちを指摘 6 として新しく出した)
  3. [minor] EditorApp.cpp:153 の空白 — 解消 (9aad35e7 の diff で `= !std::` に戻っている)
  4. [minor] performance.md:111 の絶対パス (rule 17) — 解消 (`<リポジトリのルート>` に変わり、check_rules の WARN が 66 → 65)
  5. [minor] 既知の nit の持ち越し — 未解消 (planner の判断で台帳の申し送りへ回した。M104 の合否の外)
```
