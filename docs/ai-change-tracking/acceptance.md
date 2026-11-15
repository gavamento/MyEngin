# AI Change Tracking: 受け入れの対応表 (M104t)

要求 ([spec-v1.1.md](spec-v1.1.md)) §16 の受け入れテストと §18 の完成条件の全項目が、何で確かめられているかを、**自動の試験 / 手動の確認 / 未確認** に分けて書く。**未確認は成功の扱いにしない。** 書いた時点 (2026-11-16、M104t) の実装に対する対応で、試験の名前は実在するもの (`cargo test` は関数名、`changetracking` スイートは `src\Editor\ChangeTracking\` の `*SelfTest.cpp` のファイル)。

## 凡例と、全項目に共通する「未確認」

- **自動**: `cargo test --manifest-path tools\track\Cargo.toml` (表では `cargo:ファイル名::関数名`)、`tools\track_verify.bat` (サービス + 実 pipe の NDJSON 照合)、`tools\mcp_verify.bat`、`Editor.exe --selftest-full` のスイート (表では `ct:` の後にファイル名。`changetracking` スイート)、`tools\replay_verify.bat`、`tools\check_rules.ps1`。
- **手動**: [test_checklists.md](../test_checklists.md) の「AI Change Tracking (M104)」の「通しの確認」(以下「通し」)。実 Editor を使い、利用者が行う。
- **共通の未確認** (どの項目にも言える。個別の欄には再掲しない):
  - **見た目の目視**: 各サブのスクリーンショット (`cache\m104*_shots\`) は撮ったが、見た目の合否は利用者の目視 (AI は画像を開いて文字が切れていないことだけ確かめた)。
  - **実 Editor での手の操作**: 窓のクリック、復元のボタン、Ctrl+Z など。自動の試験は画面の関数・モデルを直接呼ぶ。
  - **CI での実走** (`ci.yml`): 未 push のため。Rust 1.89 (採用した最低版) での実ビルドも未確認 (開発機は rustc 1.98)。
  - **本物の Claude Code / 本物のフック入力**: 終了後のサービスの生存、本物のフックの入力 (D14 の形は仕様と試作で確認し、フックのコマンドは模擬の入力で試験している)。

## §16 受け入れテスト

### 16.1 変更検出

| 項目 | 自動 | 手動 | 未確認 |
|---|---|---|---|
| AI が C++ ファイルを変更すると履歴が記録される | cargo:watch::create_modify_rename_delete_each_make_one_revision、cargo:revision::three_distant_edits_make_three_hunks_that_rebuild_the_after_content、track_verify (観測と `list`) | 通し 2〜3 | — |
| 同じファイルの複数 Hunk を検出できる | cargo:diff::three_distant_changes_make_three_hunks_with_exact_ranges_and_context、cargo:diff::property_hunks_rebuild_both_sides_byte_for_byte | 通し 3 | — |
| 変更前から存在した人間の未コミット変更を AI 変更として混入させない | cargo:attribution::row_3_preexisting_is_unknown_even_with_a_session_running、ベースライン (cargo:reconcile::interrupted_baseline_restarts_and_covers_every_file) | — | 本物の人間の未コミット変更を持つ作業ツリーでの AI 編集の通し |
| 短時間に連続編集された変更の観測範囲を正しく記録する | cargo:watch::writes_within_debounce_make_one_coalesced_revision、cargo:watch::three_quick_writes_chain_from_the_baseline_to_the_final_content、cargo:watch::file_held_open_for_a_second_ends_with_a_revision_of_the_final_content | — | — |
| 取得できなかった中間状態を完全な履歴として扱わない | cargo:watch::changes_while_service_was_down_are_gap_unknown、cargo:reconcile::verification_scan_catches_missed_create_modify_delete_as_gap、cargo:reconcile::real_overflow_is_fully_covered_after_verification (`--ignored`)、cargo:session::a_gap_revision_is_unknown_even_while_a_session_runs | — | `notify` の本物のバッファあふれは 1 回の `--ignored` の計測で確認 (performance.md §6)。機械・負荷によって起きたり起きなかったりする。B (一括 2500 件) の取りこぼしは計測の道具で再現できていない |

### 16.2 セッション・帰属

| 項目 | 自動 | 手動 | 未確認 |
|---|---|---|---|
| 複数 AI セッションを区別できる | cargo:session::sessions_are_distinguished_and_the_same_client_session_id_resumes_one、cargo:session::overlapping_operations_on_one_path_are_never_verified | — | 本物の複数の Claude Code セッションでの実走 |
| PID の再利用によってセッションを混同しない | cargo:session::the_same_pid_with_another_start_time_is_another_process、cargo:session::an_mcp_session_takes_the_pid_from_the_pipe_not_from_the_claim | — | — |
| Editor 停止中の MCP 操作を記録できる | cargo:hook::a_hook_with_project_starts_the_service_and_the_next_operations_are_recorded、track_verify、mcp_verify | — | **本物の Claude Code の終了後にサービスが生きていること** (Job オブジェクトの外へ出る手順は試作で確認したが、本物のクライアントでは未確認) |
| 編集元が不明な変更は Unknown になる | cargo:attribution::row_9_nothing_is_unknown、cargo:session::a_change_with_no_session_near_is_unknown、cargo:session::a_startup_gap_stays_unknown_even_when_it_matches_the_declared_edit | 通し 4 (Unknown が別の節に出る) | — |
| **終わりの来ない操作の窓で、人の編集を AI のものにしない** (M104v、spec D46。`op_end` の ok で閉じた操作の窓の根拠だけを数える) | cargo:session::a_human_edit_in_the_window_of_an_operation_that_never_ends_is_not_the_ais、cargo:session::a_human_edit_after_session_end_is_not_the_ais_even_when_another_operation_finished_ok (DB の作り直しでも同じ)、cargo:session::an_op_end_that_arrives_after_the_observation_raises_the_change_and_notifies、cargo:session::parallel_operations_are_both_verified_when_their_ends_arrive_in_order、cargo:session::a_failed_or_denied_end_and_a_turn_end_close_the_window_without_counting_it、cargo:hook::failure_denied_and_turn_end_hooks_close_operations_and_never_hand_a_human_edit_to_the_ai、track_verify | — | 本物の Claude Code で拒否したときに終わりのイベントが来ないこと (公式文書の記述の確認だけ。実機では未確認) |
| 同一ファイルを複数 AI が編集しても誤った確定帰属を行わない | cargo:attribution::overlapping_windows_never_verify_and_keep_both_candidates、cargo:attribution::several_candidates_without_overlap_are_not_claimed_for_either、cargo:session::overlapping_operations_on_one_path_are_never_verified | — | — |

### 16.3 Scene・Prefab

| 項目 | 自動 | 手動 | 未確認 |
|---|---|---|---|
| Transform 変更対象を fileId で特定できる | cargo:scene_diff::a_one_field_edit_in_a_real_scene_is_one_field_modified_with_exact_lexemes、ct:SceneChangeOverlaySelfTest (fileId で O(1) に引く、選択の同期) | 通し 5 | — |
| 別 Scene に存在する同じ fileId を混同しない | cargo:scene_diff::the_same_field_of_the_same_fileid_in_two_scenes_stays_in_two_documents、cargo:pipe::scene_overlay_folds_per_document_honours_after_revision_and_document_for_path_agrees | — | — |
| Entity 削除時に Before を表示できる | ct:SceneChangeOverlaySelfTest (ゴーストの解析・描画・World 不変・Before の読み取り専用の表示)、cargo:pipe::scene_overlay_is_paged_flags_oversize_values_and_lists_document_changes_once (`before` の返し方) | 通し 6 | 見た目 (ゴーストの描画) の目視 |
| Prefab Override を区別できる | cargo:scene_diff::actor_and_prefab_files_report_the_asset_change_plus_inner_changes、`scene_diff.rs` の単体試験 (`PrefabOverrideChanged` は `FieldModified` と二重に出ない)、ct:SceneRestoreSelfTest (override の印の行の扱い) | 通し 5 | — |
| **Prefab 伝播変更を直接編集と区別できる** (M104t) | **cargo:propagation::a_base_change_reaches_only_the_instances_that_did_not_override_it_and_survives_a_rebuild** (override していないものだけ・原因 `caused_by`・DB を作り直しても同じ・`scene_overlay` の `propagated` / `causePath`)、cargo:propagation::legacy_members_and_unreadable_scenes_are_reported_in_a_summary_row_not_hidden、cargo:propagation::deleted_scenes_and_scenes_without_the_prefab_are_left_out、ct:SceneChangeOverlaySelfTest::TestPropagation (印・文面・`causedBy`)、ct:SceneRestoreSelfTest (伝播の行は戻さず原因へ案内) | 通し 7 | 見た目の目視 (`cache\m104t_shots\` の 6 枚)。**実際のプレハブ + 実シーンでの通し** (実 Editor + 実サービスで、AI がプレハブを編集して Hierarchy に輪の印が出ること。スクショは偽のサービスの返事で撮った)。入れ子のプレハブの連鎖は追わない (限界) |
| 未保存 Scene を外部変更で上書きしない | ct:ReloadHubSelfTest (保留の口)、`changetracking` の外部変更の確認 (M104n)、`tools\replay_verify.bat` (口が未設定のとき従来どおり) | 通し 8 | 実 Editor でのモーダルの操作と目視 |

### 16.4 Review・Restore

| 項目 | 自動 | 手動 | 未確認 |
|---|---|---|---|
| Hunk 単位で復元できる | ct:RestoreApplySelfTest (一時プロジェクトで適用、`HunkRestore`)、cargo:diff::the_shared_hunk_table_matches_unapply_hunks | 通し 9 | 実 Editor でのボタン操作 |
| Field 単位で復元できる | ct:SceneRestoreSelfTest (Field / Component / Entity / Override、Ctrl+Z で戻る、プレハブの override 記録が保たれる) | 通し 9 | 実 Editor でのボタン操作・実際の Ctrl+Z |
| 再編集された対象は競合として検出する | ct:RestoreSelfTest (競合の判定)、ct:SceneRestoreSelfTest (`ValueChanged` など。何も当てない) | — | — |
| 承認済み Revision の次の Revision は Pending になる | cargo:review::an_approval_stays_with_its_revision_and_a_later_revision_starts_pending | — | — |
| Rejected によって変更が自動削除されない | cargo:review::reviewing_changes_nothing_but_the_reviews_table、cargo:review::reviews_are_append_only_and_reopen_returns_to_pending | — | — |
| 復元操作を履歴として記録できる | cargo:session::an_expected_restore_makes_the_change_the_restores_whether_it_is_observed_first_or_second、cargo:session::a_committed_restore_marks_the_targets_and_leaves_the_review_alone、cargo:session::a_committed_scene_restore_marks_only_the_restored_scene_rows_and_survives_a_rebuild | — | — |
| 複数ファイル復元の途中失敗から回復できる | ct:RestoreApplySelfTest (途中失敗の巻き戻し、`rolled_back`、`recovery_required`) | — | 実際のディスク障害 (容量不足・権限) での巻き戻し |
| 復元中の強制終了を検出できる | ct:RestoreApplySelfTest (ジャーナルの `applying` を起動時に見つけ、回復の画面を出す) | — | **本物のプロセスの強制終了** (ジャーナルを残した状態の再現で確認。実際に Editor を kill して再起動する通しは未実施) |

### 16.5 既存機能との共存

| 項目 | 自動 | 手動 | 未確認 |
|---|---|---|---|
| Git Index を勝手に変更しない | ct:RestoreApplySelfTest ("git: the bytes of .git\index are the same before and after the restore")。追跡と復元のコードは `.git` の下を読みも書きもしない (`SourceControl\` の `GitTransaction` はゲートの型だけ使う) | — | — |
| Play 中の危険な変更適用を停止する | ct:RestoreSelfTest (ゲート `Playing` / `NetActive` / `BuildRunning` などは必ず止める)、ct:SceneRestoreSelfTest (Play 中は止まる)、ct:ReloadHubSelfTest | — | 実 Editor での Play 中の操作 |
| MCP 切断時も Editor が動作する | ct:ChangeTrackingSelfTest (切断・再接続・`proto_mismatch` で Editor の他機能に影響しない)、cargo:pipe::an_unread_subscriber_does_not_stop_recording_and_gets_one_resync | — | 本物の MCP 切断の通し |
| Tracking DB 破損時も Editor が起動する | cargo:storage::corrupt_db_is_quarantined_and_rebuilt_from_journal_with_gap、cargo:storage::newer_db_is_not_written_and_degrades、ct:ChangeTrackingSelfTest (`db_degraded` で通常動作) | — | — |
| Scene View の追跡表示が WorldHash に影響しない | ct:SceneChangeOverlaySelfTest (`TestPlayModeInvariance`、ゴーストの `HashWorld` 一致、`FindByFileId` 以外で fileId を採番しない)、ct:ScenePreviewSelfTest (プレビューは専用の Scene) | — | — |
| Replay Verify の結果が追跡機能の有効・無効で変化しない | `tools\replay_verify.bat` (M104n、o、q、r、s、t の各サブで PASS。非対話起動はサービスへ接続も起動もしないので、設定が有効でも追跡のコードが I/O をしない。ct:ChangeTrackingSelfTest の `disabled`) | — | 設定を有効にした通常起動での長時間の Replay の一致 (追跡は World を書かない構造で保証している) |
| AI Change Tracking を無効化しても通常の Editor 操作が変わらない | ct:ChangeTrackingSelfTest (`disabled`: 既定は無効、無効では I/O もサービスの起動もしない)、`tools\check_rules.ps1` の規則 19 | 通し 1 | — |

## §18 完成条件

### 機能面

| # | 条件 | 自動 | 手動 | 未確認 |
|---|---|---|---|---|
| 1 | AI セッションごとに変更を記録できる | cargo:session、cargo:hook、track_verify、mcp_verify | 通し 2 | 本物の Claude Code |
| 2 | 変更元の確実性を表示できる | cargo:attribution (§3 の 9 行)、ct:ChangeViewSelfTest (帰属と根拠の整形・英日)、ct:SceneChangeOverlaySelfTest (「未確認」の印) | 通し 4 | 見た目の目視 |
| 3 | コード差分を Hunk 単位で確認できる | cargo:diff、cargo:revision、ct:ChangeViewSelfTest (Diff 表示部品) | 通し 3 | 見た目の目視 |
| 4 | Scene 変更を Entity・Component・Field 単位で確認できる | cargo:scene_diff、cargo:pipe (`scene_diff` / `scene_overlay`)、ct:SceneChangeOverlaySelfTest | 通し 5 | 見た目の目視 |
| 5 | Before / After 比較を提供できる | ct:ScenePreviewSelfTest (専用の Scene と RT、アセットを再現できないときの表示、`HashWorld` 不変) | 通し 10 | 見た目 (3D 比較の描画) の目視 |
| 6 | Revision 単位で承認・却下できる | cargo:review (7 つの単位)、ct:ReviewSelfTest | 通し 9 | — |
| 7 | 競合検証を通して変更を復元できる | ct:RestoreSelfTest、ct:RestoreApplySelfTest、ct:SceneRestoreSelfTest | 通し 9 | 実 Editor での復元の通し |
| 8 | Editor 再起動後も履歴を保持できる | cargo:pipe::history_survives_a_restart_and_the_down_time_is_a_gap、cargo:storage::journal_lines_not_yet_in_db_are_replayed_exactly_once | 通し 11 | — |

### 安全性

| # | 条件 | 自動 | 手動 | 未確認 |
|---|---|---|---|---|
| 1 | 既存の未コミット変更を破壊しない | ct:RestoreApplySelfTest (復元は競合のとき何も書かない、復元前の内容を `pre_restore` に残す)、cargo:watch (追跡は作業ツリーを書かない。各結合テストの後処理が `.mye` 以外の作業ツリーの不変を検査する) | — | — |
| 2 | Git Index を勝手に変更しない | ct:RestoreApplySelfTest (`.git\index` のバイト列) | — | — |
| 3 | 未保存 Scene を上書きしない | ct:ReloadHubSelfTest、`changetracking` の外部変更の確認 | 通し 8 | 実 Editor での操作 |
| 4 | 変更元を根拠なく確定しない | cargo:attribution (`Verified` の条件は宣言 + バイト一致 + 他の候補なし)、cargo:session (人の編集を挟むと確定しない) | — | — |
| 5 | 異常終了後の復元処理を回復可能にする | ct:RestoreApplySelfTest (ジャーナル、回復の画面) | — | 本物のプロセスの強制終了 |
| 6 | Play モード・ネットワークセッションの安全性を維持する | ct:RestoreSelfTest (ゲート `Playing` / `NetActive` は必ず止める)、ct:SceneRestoreSelfTest (実行の直前のゲート) | — | 実際の Play 中・ネットワークセッション中の操作 (ゲートの入力としての試験まで) |
| 7 | WorldHash と決定論 Replay に影響を与えない | ct:SceneChangeOverlaySelfTest、ct:ScenePreviewSelfTest (`HashWorld` 一致)、`tools\replay_verify.bat`、ABI・SimSnapshot・TypeId の不変 (`git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` が空) | — | — |

### 配布 (A21、M104u)

| 条件 | 自動 | 手動 | 未確認 |
|---|---|---|---|
| エンジンの配布物に `mye_track.exe` が入り、欠けていれば作成が失敗する | `make_dist.ps1 -SkipBuild` の実行 (配布物の `bin\x64\Release\mye_track.exe`)、`mye_track.exe` が無い入力での `missing in …: mye_track.exe` | — | — |
| `release.yml` がサービスをビルドしてから詰める | 差分の確認 (`track service (Release)` の段が `package` より前) | — | **ワークフローの実走** (push 後) |
| 展開先の Editor が隣の `mye_track.exe` を起動して接続できる | 展開先で `mye_track.exe --version`、`serve --project`、`client` の hello / status / shutdown | `test_checklists.md` の M104u | **展開先の Editor からの接続** (対話起動が要る) |
| ゲームの配布物に `mye_track.exe` も `.mye` も入らない | `Editor.exe --package` の出力の確認、`ci.yml` の `$forbidden` | — | CI での実走 |

### 検証面 (それぞれ独立して自動テストできる)

| 領域 | 独立した試験 |
|---|---|
| 変更検出 | cargo:watch、cargo:reconcile |
| 変更帰属 | cargo:attribution、cargo:session、cargo:hook |
| 差分生成 | cargo:diff、cargo:revision、cargo:scene_diff、cargo:propagation |
| 承認状態 | cargo:review、ct:ReviewSelfTest |
| 復元処理 | ct:RestoreSelfTest、ct:RestoreApplySelfTest、ct:SceneRestoreSelfTest (一時プロジェクト) |
| 永続化 | cargo:storage、cargo:revision (DB を作り直して同じ)、cargo:propagation |
| 障害回復 | cargo:storage、cargo:reconcile、ct:RestoreApplySelfTest (ジャーナルと回復) |

## 未確認の一覧 (成功扱いにしていないもの)

1. 見た目の目視 (各サブのスクリーンショット)。
2. 実 Editor での手の操作の通し (`test_checklists.md` の「通しの確認」)。
3. 本物の Claude Code の終了後のサービスの生存、本物のフックの入力での実走。
4. CI での実走、Rust 1.89 での実ビルド。
5. 実際のプロセスの強制終了での復元の回復、ディスク障害での復元の巻き戻し。
6. 実際のプレハブ + 実シーンでの伝播の通し。入れ子のプレハブの連鎖と、プレハブ側のエンティティの追加・削除・親子の変更は伝播の行にしない (限界)。
7. 変更 2 万件の文書の取り直し (performance.md §10 の「外れるもの」)。
8. 配布 (M104u): `release.yml` の実走、展開先の Editor からの接続、Launcher 経由の導入・削除の実機、Defender (ダウンロードを通した経路)。
