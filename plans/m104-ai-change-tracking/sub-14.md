# sub-14: レビュー (M104o)

- 依存: sub-24、sub-13 (順序は sub-24 → sub-25 → sub-13 → sub-14)
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-13 の VERDICT で planner が書き直した)。Phase 4 はこの 1 本で、Phase 3〜4 の横断の検証 (A20) もここで回す。
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

要求 §10、`C:\HAL\MyEngin\docs\ai-change-tracking\db-schema.md` §3 の `reviews` と §3.1 (db)、ADR-039 決定 11。完了条件は「承認情報が別 Revision へ誤って引き継がれない」。

## 原則 (変えてはいけないもの)

- **承認は何も起こさない。** git の操作 (stage / commit / push)、ビルド、テスト、ファイルの書き込みを一切しない。**却下は変更を消さない。** 取り消すのは復元 (sub-15) で、レビューとは別の操作 (要求 §10.4)。
- **記録は追記だけ。** 過去の行を書き換えない・消さない。Reopen は「再開した」という行を足すこと (要求 §10.1)。
- **新しい Revision は必ず Pending から。** 前の Revision の承認は前の Revision に残り、動かない (要求 §10.3)。Hunk の ID は Revision ごとに別 (sub-04) なので、古い承認が新しい Hunk を指すことは構造上ない。
- **MCP とフックからは呼べない** (`role = editor` だけ。要求 §12.6)。
- 親の状態は保存しない。子から問い合わせのたびに計算する (db §3.1)。
- レビューの状態と、復元の状態 (`changes.state` / `change_hunks.state` / `scene_changes.state`) は別の軸 (要求 §10.5)。このサブは復元の状態に触れない。

## `--selftest-full` の扱い (spec §2 D38)

- Debug の `Editor.exe --selftest-full` は、`physics` スイートの開始直後にスタックオーバーフロー (終了コード -1073741571) で落ちる (M104 の外の別件。planner が `MYE_SELFTEST_ONLY=physics` の Debug で再現)。**M104 では直さない。**
- A20 の「`--selftest-full` が通る」は次の形で確かめる: Release は `bin\x64\Release\Editor.exe --selftest-full` を全部。Debug は `--selftest-list` の**スイートの名前だけ** (別名の行 `m94 = …` などを混ぜない。sub-13 で、別名を混ぜて physics が走った) から `physics` を除いた全部を `MYE_SELFTEST_ONLY=<名前,...>` で回す。除いたことを結果に明記する。

## やること

### A. サービス側 (`tools\track`)

1. **op `review_set`** (`role = editor` だけ)。引数:
   - `targets`: 配列。要素は `{revisionId, scope: {kind, ref?}}`。`kind` は `revision` / `hunk` / `entity` / `component` / `field`。`ref` は、`hunk` なら `hunkId`、`entity` なら `fileId`、`component` なら `"fileId/Component"`、`field` なら `"fileId/Component/field"` (コンポーネントに属さないフィールドは `"fileId//field"`)。`revision` は `ref` なし。
   - `state`: `approved` / `rejected` / `needs_review` / `reopened`。
   - `note?` (上限 4000 文字)。
   - 複数の対象を 1 回で受けるのは、セッションや操作をまとめて承認する (下の B) のを 1 つのイベントとして記録するため。対象は 1 回 1000 件まで (超えたら `bad_args`。Editor が分けて送る)。
   - 検査: Revision が存在する、`hunkId` がその Revision のもの、`fileId` などがその Revision の `scene_changes` に存在する。合わなければ `not_found` / `bad_args`。**別の Revision の Hunk ID や、存在しない範囲への記録を作らない。**
2. 記録: `Store::record_event` (kind `review_set`) を通し、`reviews` へ対象ごとに 1 行ずつ足す。`reviews` も events から作り直せる派生の表 (spec §2 D26。`rebuild_derived` を延ばす)。
3. **今の状態の決め方** (純粋な関数にして単体で試験する):
   - 葉 = テキストの Revision なら各 Hunk、シーンの Revision なら各構造の変更 (`scene_changes` の行)、どちらも無い Revision (バイナリ、除外、解析不可、削除だけ、など) は Revision 自身。
   - ある葉の状態 = その葉を含む範囲 (葉自身 → フィールド → コンポーネント → エンティティ → Revision) に付いた行のうち、**いちばん新しい行**の状態。行が無ければ `pending`。`reopened` の行がいちばん新しければ `pending`。
   - 範囲の広さではなく新しさで決まる (Revision 全体を承認した後で 1 つの Hunk を却下したら、その Hunk は却下、他は承認。その後もう一度 Revision 全体を承認したら、全部承認)。
4. **集約** (Revision、ファイル = Change、操作、セッション):
   - 葉が全部同じ → その状態 (`pending` / `approved` / `rejected` / `needs_review`)。
   - そうでなければ、上から順に: `needs_review` が 1 つでもある → `needs_review`。`approved` が 1 つでもある → `partially_approved`。それ以外 (`rejected` と `pending` の混在) → `partially_rejected`。
   - Change / 操作 / セッションは、属する Revision の葉を全部集めて同じ規則。件数 (状態ごとの葉の数) も返す。
   - **集約の対象は、行為者が `ai` か `unknown` の Revision だけ** (`editor_user` と `restore` は数えない。レビューの対象ではない)。
   - この表を db §3.1 に書く。
5. 取得への反映: `describe` に、Revision の集約の状態・葉ごとの状態・**履歴** (その Revision の `reviews` の行を古い順に。状態、範囲、メモ、時刻) を足す。`hunks` と `scene_diff` の各要素に今の状態。`list` の各行に Revision の集約の状態と、`review` フィルタ (集約の状態で絞る。sub-05 で引数だけ受けていた)。`sessions` の各行にセッションの集約と件数。集約の計算が重くならないよう、1 回の問い合わせで引く行の数に上限を置き、索引が要れば移行 (今の版は 3。足すなら v4。`v1.sql`〜`v3.sql` は変えない)。
6. 通知 `review_changed` (`revisionIds`)。`subscribe` の対象に足す。
7. `scene_overlay` (sub-12): **今の状態が `approved` の構造の変更は強調から外す** (確認が済んだものにバッジを出し続けない)。`rejected` と `needs_review` は強調したまま。引数で切り替えられるようにしてよい。sub-12 の「ここまで確認済みにする」(表示だけの印) は残す (レビューせずに強調だけ消したい場合のため)。
8. MCP: `change_list` の `review` フィルタと、`change_describe` の結果のレビューの状態は、そのまま通る (`tools\mcp\src` は変えない。MCP の期待値が変わるシナリオがあれば撮り直し、差分を説明する)。MCP から `review_set` を呼ぶ道具は足さない。
9. `protocol.rs` (`IMPLEMENTED_OPS`、`role_allows`、通知の名前)、ipc、試験 (`tests\review.rs` 新規、`tests\pipe.rs`、`tests\track\` のシナリオ)。`tests\pipe.rs` の `unused_mut` の警告 1 件を直す (持ち越し)。

### B. Editor 側

1. `src\Editor\ChangeTracking\ReviewManager.h/.cpp` (新規): `review_set` を送る薄い層。対象の組み立て (セッション全体 = そのセッションが候補の Revision の `revision` 範囲を全部。操作全体も同様。1000 件ごとに分けて送る)、送信中の状態、失敗の扱い (切断・時間切れ・`not_found` は利用者に見える形で知らせ、画面の状態を先に変えない。**サービスの応答で確定してから表示を変える**)。
2. `AiChangesWindow`:
   - 詳細の列に、Revision の状態 (集約) と操作: 承認 / 却下 / 要確認 / 再開、メモの入力。
   - Hunk 単位: `DiffViewer` の Hunk の選択 (sub-10) に対して同じ操作。各 Hunk の見出しに状態。
   - シーンの変更の一覧 (sub-12) の行に対して同じ操作 (Entity / Component / Field の範囲)。
   - セッションの行と、一覧の複数選択に対して「まとめて承認 / 却下」。**まとめての操作は件数を出して 1 回確認する。**
   - 状態の表示: 一覧の行、セッションの行に集約の状態 (Pending / Approved / Rejected / NeedsReview / Partially Approved / 一部却下) と件数。`review` のフィルタ。
   - 履歴: 選んだ Revision のレビューの履歴 (古い順。誰が、は無い。1 人の利用者の記録)。
   - **文面で明示する**: 承認は「確認した」という記録で、git にもファイルにも何もしない。却下は変更を消さない (取り消すには復元。復元は sub-15 で、ここでは「復元は別の操作です」の 1 行まで)。
   - 行為者が `editor_user` / `restore` の変更には操作を出さない。
3. 整形 (状態 6 種の文面、件数の表示、履歴の行) は `ChangeFormat` に置き、スイートから呼べるようにする。英日。色は既存のテーマ色 (sub-24 の強調の 4 色とは別)。
4. `SceneChangeOverlay`: `review_changed` で取り直す。承認済みが強調から外れる。

### C. sub-13 から持ち越した直し

1. **自分の保存で確認が出る場合を無くす** (sub-13 の VERDICT の should): 保存の直後 (監視のイベントが届くまでの約 150 ms 以上) に利用者が編集を続けると、自分の保存の監視イベントが「未保存」の状態で届き、「エディタの外で変更されました」の確認が出る。Editor は自分が今保存したことを知っているので、**保存の直後に控えたファイルの大きさと最終更新時刻 (sub-12 で `NoteSaved` が控えているものと同じ手段) が、保留になった時点のファイルと同じなら、確認を出さずに保留を捨てる** (ライブのシーンは、その保存の内容 + その後の編集なので、読み込む必要が無い)。違えば今までどおり確認。`ReloadHub` の API は変えない (Editor 側で `DiscardPendingScene` を呼ぶ)。試験は Editor 側の判定の関数を切り出して行う。
2. ADR-039 の「影響」の節の「Engine 層の変更は `ReloadHub` の口 1 つ」を、実装に合わせて直す (口 1 つ + 保留の操作。公開関数は `SetSceneApplyGate` / `HasPendingScene` / `PendingScenePath` / `RequestApplyPendingScene` / `DiscardPendingScene` / `TakePendingSceneResult`)。arch §8 も合わせる。
3. Debug の `Editor.exe` を、最終のソースからビルドし直す (sub-13 の終わりに、Release だけがスクショ用のプローブを外した後にビルドし直されていた)。

### D. 文書

- db (`reviews` の今の状態の決め方、集約の表、移行を足した場合の版)、ipc (`review_set`、`review_changed`、各取得の追加の項目)、`C:\HAL\MyEngin\engine_spec.md` §9.2、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表)。

## やらないこと (このサブでは)

- 復元 (sub-15、16)。却下から復元への導線は「復元は別の操作です」の案内まで。
- 自動の承認、MCP からの承認 (要求 §1.4、§12.6)。レビューの担当者・複数人の承認。
- 下位の層の変更 (変わってよいのは `LocalizationTable.inl` だけ)。`tools\mcp\src` の変更。
- git の操作。

## 受け入れ条件 (このサブ)

1. **別の Revision へ引き継がれない**: Revision 1 を承認した後に同じファイルが変わると、Revision 2 は `pending` で、Revision 1 の承認の行と状態は変わらない。Revision 1 の Hunk ID で Revision 2 に記録しようとすると `not_found` / `bad_args` → `cargo test --manifest-path tools\track\Cargo.toml` (要求 §16.4 の 4 番目、Phase 4 の完了条件)。
2. **追記だけ**: 承認 → 再開 → 却下で `reviews` に 3 行あり、前の行のどの列も変わっていない (行の内容を 1 行ずつ比べる)。再開の直後の状態は `pending` → `cargo test`。
3. 今の状態の決め方: Revision 全体を承認 → 1 つの Hunk を却下 → その Hunk は `rejected`、他は `approved`、Revision は `partially_approved`。その後 Revision 全体を承認 → 全部 `approved`。シーンでも同じ (Entity を承認 → その中の 1 フィールドを要確認)。新しさで決まり、範囲の広さで決まらないこと → `cargo test` (純粋な関数の単体)。
4. 集約の表の全行: 全部同じ (4 通り)、`needs_review` が混ざる、`approved` が一部、`rejected` と `pending` の混在。葉が無い Revision (バイナリ) は Revision 自身が葉。`editor_user` / `restore` の Revision は集約に数えない → `cargo test`。
5. **承認・却下は何も変えない**: 承認・却下・要確認・再開のそれぞれの前後で、作業ツリーの全ファイルのパスとハッシュ、`.git\index` のバイト列、`change_revisions` / `change_hunks` / `scene_changes` / `changes` の `state` が同じ。git のプロセスが起動されない → `cargo test` (一時の git リポジトリ) (要求 §16.4 の 5 番目、§16.5 の 1 番目)。
6. 役割: `mcp` と `hook` の `review_set` が `forbidden` → `tools\track_verify.bat`。
7. 範囲: Hunk / Entity / Component / Field の記録が、Revision と組で区別される。別のシーンの同じ `fileId` への記録が混ざらない (Revision が文書を決める) → `cargo test`。
8. 取得: `describe` に状態・葉ごとの状態・履歴、`list` の `review` フィルタ、`sessions` の集約と件数、`review_changed` の通知 → `cargo test` + `tools\track_verify.bat`。
9. 作り直し: レビューを記録した後に `tracking.db` を壊して起動し直すと、`reviews` と各状態が壊す前と同じ → `cargo test`。
10. `scene_overlay` が、承認済みの構造の変更を返さず、却下・要確認のものは返す → `cargo test`。
11. Editor: 整形 (状態 6 種、件数、履歴) が英日で期待どおり。`ReviewManager` が、まとめての対象を正しく組み立て、1000 件ごとに分け、失敗のとき表示を変えない。行為者が `editor_user` / `restore` の変更に操作を出さない。追跡が無効・非対話起動のとき I/O の口が呼ばれない → `changetracking` スイート (ヘッドレスの ImGui フレームで窓を描く)。
12. C の 1: 自分の保存の直後の監視イベントで確認が出ない (控えた大きさと更新時刻が同じ → 保留を捨てる)。外から書き換えられていれば確認が出る → スイート。
13. スクショ (一時のプローブ。コミットしない): (1) 詳細の列のレビューの操作と履歴 (2) Hunk ごとの状態 (3) 一覧とセッションの集約の表示 (Partially Approved を含む) (4) まとめて承認の確認 (5) 英語。Release で撮る。**見た目の合否はユーザーの目視。**
14. **A20 (Phase 3〜4 の横断)**:
    - `tools\check_rules.ps1` が 0 error。`tools\check_rules_selftest.ps1` が PASS。
    - `bin\x64\Release\Editor.exe --selftest-full` が全部通る。Debug は `physics` を除いた全スイートが通る (上の「`--selftest-full` の扱い」)。実行中と後に `mye_track.exe` が無い。
    - `tools\replay_verify.bat` が通る。**追跡の有効・無効で結果が変わらないこと**は、構造で成り立つ (replay_verify は非対話起動で、`allowChangeTracking` が偽なので、設定が有効でも追跡のコードが I/O をしない。sub-09 の受け入れ条件 2、3)。この根拠を結果に書き、加えて、`changeTrackingEnabled` を true にした一時プロジェクトで非対話起動を 1 回回し、`mye_track.exe` が起動せず `.mye\ai_changes\` ができないことを確かめる。
    - `cargo test` (track)、`tools\track_verify.bat`、`cargo test` (mcp) が通る。`tools\mcp_verify.bat` は、`tools\mcp\src` を変えていなければ回さなくてよい (回していないことを書く)。
    - ABI・SimSnapshot・TypeId が変わっていない: `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` が空。`git diff --stat ca6a3991 -- src\Engine` に出るのは `LocalizationTable.inl` と `HotReload\ReloadHub.*` (sub-13) だけ。
15. Debug と Release の `Editor.exe` が、最終のソースより新しい (C の 3)。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\build_track.bat Release`、`tools\track_verify.bat`、`cargo test --manifest-path tools\mcp\Cargo.toml`
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`
- `bin\x64\Release\Editor.exe --selftest-full`、Debug は `physics` を除いた全スイート
- `tools\replay_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`、`pwsh -File tools\check_rules_selftest.ps1`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`、`git diff --stat ca6a3991 -- src\Engine`
- exe の実行は PowerShell から。`tools\mcp\target\release\` の exe に触らない。このリポジトリのルートで追跡を有効にしない。ユーザーのプロセスを止めない。

## 実装メモ (coder が追記)

SELF_EVAL: sub-14 (round 1)

実装:
  - tools\track\src\review.rs (新規) — 葉の今の状態 (`LatestIndex`。範囲の広さでなく新しさ、`reopened` = pending)、集約 (`Counts::aggregate`)、葉の読み出し (構造の変更 → Hunk → Revision 自身)、`describe_fields`、`group_counts` (変更 / 操作 / セッション)、`apply_event` (`reviews` へ追記)。単体試験 5 件。
  - tools\track\src\ops_record.rs:review_set — `role = editor` だけ。targets 1〜1000 件、scope の ref を正規形だけ受ける、Revision・Hunk・fileId・コンポーネント・フィールドの実在を DB で確かめてから、1 つの `review_set` イベントとして `record_and_apply`。1 件でも不正なら何も記録しない。
  - tools\track\src\{session.rs,revision.rs,protocol.rs,server.rs,lib.rs} — `review_set` を `apply_event` / 作り直し (`reviews` を先に消して入れ直す) / `IMPLEMENTED_OPS` / `role_allows` / 配線 / 通知名 `review_changed` に足した。
  - tools\track\src\ops.rs — `describe` (reviewable / review / reviewCounts / reviewLeaves / reviewHistory / changeReview / operationReview、hunks に hunkId と review)、`hunks` と `scene_diff` の各要素に review、`list` の各行の review (集約。対象外は null) と `review` フィルタ (集約の 6 状態。1 回に 2000 Revision まで読み、上限なら読んだ所までと続きの cursor)、`sessions` の各行に review (集約と件数)。
  - tools\track\src\notify.rs — `review_changed` (1 回の引きで 1 件、`revisionIds`)。
  - tools\track\src\scene_overlay.rs — 承認済みの構造の変更を強調から外す (`excludeApproved` 既定 true、`approvedHidden`)。
  - tools\track\tests\review.rs (新規 14 件)、tools\track\tests\pipe.rs (`unused_mut` を直した)、tests\track\27_review.ndjson (新規シナリオ。mcp / hook の forbidden を含む)、tests\track\*.expected.ndjson (11 本を撮り直し。追加の項目だけ)。
  - src\Editor\ChangeTracking\ReviewManager.h/.cpp (新規) — `Submit` (1000 件ごとに分けて 1 件ずつ送る、応答を待って次へ。失敗で止め、記録済みの件数を持つ)、`PrepareRevisions` / `PrepareSession` (list を最後まで引く。集めきれなければ全体を失敗にして何も記録しない) / `CommitPrepared`、対象の組み立て (`ForHunk` / `ForEntity` / `ForComponent` / `ForField` / `ForSceneChange` / `RevisionsOf`)。切断・無効なら I/O を呼ばず失敗にする。
  - src\Editor\ChangeTracking\{ChangeModel,ChangeFormat,ChangeTrackingSession,TrackProtocol,SceneChangeOverlay}.* — 応答の解釈 (review 項目)、`Refresh`、`review_changed` で一覧・セッション・(選択中なら) 詳細・オーバーレイを取り直す、購読に `review_changed`、整形 (状態 6 種・件数・範囲・履歴の行・確認文・失敗文、英日)。
  - src\Editor\Windows\Project\AiChangesWindow.* — 詳細のレビュー欄 (状態・内訳・4 操作・メモ・履歴・「承認は何もしない / 却下は消さない」の文面)、Hunk の選択と操作 (見出しに状態、取り直しても選択を ID で保つ)、シーンの変更の行の操作 (範囲を フィールド / コンポーネント / エンティティ / 変更全体 から選ぶ)、一覧の行・セッションの行に集約、`review` の絞り込み、Ctrl / Shift クリックの複数選択、セッション全体 / 選択のまとめては件数つきの確認 1 回、送信中・失敗の 1 行 (editor_user / restore の変更には操作を出さない)。
  - src\Editor\Widgets\DiffView.* — `DiffHunkInput` に `badge` / `badgeColor` (見出しの右に状態を出す。追跡には依存しない)。
  - src\Editor\App\OwnSaveRecord.h/.cpp (新規)、EditorApp.h/.cpp — C の 1: 保存の直後にファイルの大きさと更新時刻を控え、保留のシーンが同じ印なら確認を出さず `DiscardPendingScene`。外から書き換えられていれば印が違うので確認が出る。
  - src\Engine\Core\Localization\LocalizationTable.inl — `AiChg_Rv*` 48 件 (英日)。
  - src\Editor\ChangeTracking\ReviewSelfTest.cpp (新規。`changetracking` スイートへ) — 対象の組み立て / 分割 / 1 件ずつ送る / 失敗で表示を変えない / 時間切れ / 無効で I/O ゼロ / セッションの収集 / 文面 (英日) / 通知 / 窓 (ヘッドレスの ImGui フレーム) / 自分の保存の印。
  - docs: db-schema.md §3.4、ipc-contract.md §4.6 と通知、ADR-039 の「影響」と architecture.md §8・§9 (口 1 つ + 保留の操作。自分の保存の確認の回避)、engine_spec.md §9.2、test_checklists.md、plans\m104-ai-change-tracking.md の進捗表。build\Editor.vcxproj(.filters) は `gen_project_files.ps1` の再生成 (新規 4 ファイル)。

仕様との差分:
  - [追加] `review_set` は行為者が `editor_user` / `restore` の Revision を `bad_args` で断る (仕様は集約に数えないとだけ。Editor は操作を出さないが、サービス側でも対象外を弾く)。
  - [追加] 構造の変更を持つ Revision への `hunk` 範囲は `bad_args` (葉は構造の変更で、Hunk への記録は状態に効かず黙って無意味な行になるため)。構造の変更が無い Revision の `entity` などは `not_found`。
  - [追加] `list` の `review` フィルタは集約の 6 状態だけ (旧 `reopened` は受けない)。集約の読み出しの上限: list は 2000 Revision / 1 回、変更・操作・セッションの集約は 5000 Revision (超えたら `truncated`)。
  - [追加] `scene_overlay` に `excludeApproved` (既定 true) と `approvedHidden`。`describe` の hunks に `hunkId`。
  - [追加] `ChangeModel::Refresh`、`DiffHunkInput.badge / badgeColor` (DiffView を触った)、`OwnSaveRecord` (新規ファイル)。EditorApp の `NoteFileSaved` が全ての保存で印を控える (stat だけ)。
  - [未実装] 「操作全体」を窓から選んでまとめて承認する導線 (操作 ID を選ぶ画面が無い)。サービスは `operationReview` を返し、`ReviewManager::PrepareRevisions` は任意の Revision 集合を受けるので、足すのは導線だけ。
  - [未実装] 変更 (ファイル) 単位・操作単位の集約を窓に出すこと (サービスは `changeReview` / `operationReview` を返すが、窓は Revision・セッションの集約だけを出す)。
  - [未実装] MCP からの `review_set` の道具 (仕様どおり足さない)。`tools\mcp\src` は変えていない (`mcp_verify` は回していない)。

検証:
  - cargo test --manifest-path tools\track\Cargo.toml → 13 本 265 件 OK、警告 0 (`unused_mut` も解消。新規 `tests\review.rs` は 14 件 + `review.rs` の単体 5 件)。
  - tools\build_track.bat Release → tools\track_verify.bat → 27 シナリオ全部 PASS (27_review 新規。11 本の期待値は追加項目だけの差で、削除・値の変更は 0 を機械で確認)。実行後 mye_track.exe なし。
  - cargo test --manifest-path tools\mcp\Cargo.toml (--target-dir cache\track_verify\mcp_target) → 58 件 OK。tools\mcp_verify.bat は未実行 (tools\mcp\src を変えていない。MCP が絡む track_verify の 16 / 17 は撮り直し済みで PASS)。
  - MyEngine.sln を Debug|x64 と Release|x64 で `/p:MyeWarnAsError=true` ビルド → 成功 (警告 0)。スクショ用のプローブ (ReviewProbe.h と EditorApp の差し込み) を外したあとにもう一度。Editor.exe は Debug (16:00) / Release (16:08) とも最後のソースの変更 (15:29) より新しい (C の 3)。
  - MYE_SELFTEST_ONLY=changetracking の Debug --selftest → ALL PASS (新規の試験を含む、FAIL 0)。
  - Release の `Editor.exe --selftest-full` → 125 スイート 0 失敗 (227 秒)。Debug は `--selftest-list` のスイート名 124 個 (`physics` だけ除く。D38) を `MYE_SELFTEST_ONLY` に並べて `--selftest` → 124 スイート 0 失敗 (668 秒)。どちらも実行後に mye_track.exe なし。**Debug の physics は除いた (既知の別件。調べていない)**。
  - tools\replay_verify.bat → [PASS] (24 ジョブ、17 シーン)。追跡の有効・無効で結果が変わらない根拠: replay_verify は非対話起動で `allowChangeTracking` が偽 → 設定が有効でも追跡のコードは I/O をしない (sub-09 の条件 2、3。`disabled` の試験)。加えて changeTrackingEnabled = true の一時プロジェクト (`%TEMP%\mye_m104o_proj`、`LOCALAPPDATA` を一時の場所へ向けてプロジェクト一覧を汚さない) を Release の `Editor.exe --project <dir> --frames 60` で非対話起動 → 終了コード 0、実行中 (12 秒間 100 ms おきに監視) も後も mye_track.exe なし、`.mye\ai_changes\` なし。
  - pwsh tools\check_rules.ps1 → 0 error (警告 66 は既存のみ)。pwsh tools\check_rules_selftest.ps1 → FAIL なし。
  - git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs → 空。git diff --stat ca6a3991 -- src\Engine → LocalizationTable.inl と HotReload\ReloadHub.{h,cpp} / ReloadHubSelfTest.cpp だけ。Runtime / Server / GameLogic に差分なし。ABI・SimSnapshot・TypeId は変えていない。
  - スクショ (一時のプローブ + Release の `Editor.exe --frames 40 --shot-frame 36 --screenshot`。プローブは外した): C:\HAL\MyEngin\cache\m104o_shots\ の 1_detail_ja.png (詳細のレビュー欄・履歴・Hunk の操作)、2_hunks_ja.png (Hunk ごとの状態・選択)、3_list_ja.png (一覧と「一部承認 / 未確認 2 / 承認済み 3 / 却下 1」のセッション)、4_bulk_ja.png と 4_bulk_en.png (まとめての確認)、5_detail_en.png / 5_hunks_en.png (英語)。撮れた・文字が切れていない・化けていない、まで確認した。**見た目の合否はユーザーの目視 (未確認)**。画面は偽のサービスの応答 (FakeIo) で描いたもので、実サービスと実 Editor の操作は未確認。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜12・14・15 は機械で確認。「操作全体」の一括の導線と、変更・操作の集約の窓表示は入れていない ([未実装] に明記)。見た目 (条件 13) は目視待ち。
  正しさ: 4 — サービスは実 DB の結合試験 (別 Revision への非継承・追記だけ・新しさ・集約・何も変えない・作り直し・役割・overlay) と pipe 越しのシナリオで確認。Editor は偽の I/O と窓の描画で確認。実 Editor を人が操作する確認 (実サービスとの組み合わせ、自分の保存の確認が実際に出なくなること) は未実施。
  コード品質: 4 — 状態の決め方と集約は DB に触れない純粋な関数に切り出して 1 か所。`list` の `review` フィルタの走査上限と cursor は新しい仕組みなので、大量の Revision での実測はしていない。
  テスト: 4 — 受け入れ条件ごとに試験があり (Rust 19 件、Editor の新規試験、シナリオ 27)、全スイートを回した。実サービスを Editor から叩く結合 (`MYE_TRACK_PROBE`) にはレビューを足していない。

不安・質問:
  - 「操作全体」のまとめて承認の導線を、このサブで足すか後続に回すか (planner の判断)。足すなら窓の詳細に「この操作の変更をまとめてレビュー」を置く形で、`PrepareRevisions` を使える。
  - `review_set` を `editor_user` / `restore` の Revision に対して拒否する判断 (上の [追加] の 1 つ目) で良いか。復元 (sub-15) が `restore` の Revision へレビューを付ける要求を持つなら戻す。
  - 変更 (ファイル) 単位の集約を窓のどこに出すか (一覧は Revision 単位なので、Change 単位の見出しが要る)。

触ったファイル: (上の「実装」の一覧。生成物 `build\Editor.vcxproj(.filters)` を除く)
  tools\track\src\{review.rs(新規),ops.rs,ops_record.rs,notify.rs,protocol.rs,revision.rs,scene_overlay.rs,server.rs,session.rs,lib.rs}、tools\track\tests\{review.rs(新規),pipe.rs}、tests\track\{27_review.ndjson,27_review.expected.ndjson(新規),06,11,12,14,15,16,17,18,24,25,26 の expected.ndjson}、src\Editor\ChangeTracking\{ReviewManager.h,ReviewManager.cpp,ReviewSelfTest.cpp(新規),ChangeModel.h/.cpp,ChangeFormat.h/.cpp,ChangeTrackingSession.h/.cpp,ChangeTrackingSelfTest.h/.cpp,SceneChangeOverlay.cpp,TrackProtocol.h}、src\Editor\Windows\Project\AiChangesWindow.h/.cpp、src\Editor\Widgets\DiffView.h/.cpp、src\Editor\App\{OwnSaveRecord.h,OwnSaveRecord.cpp(新規),EditorApp.h,EditorApp.cpp}、src\Engine\Core\Localization\LocalizationTable.inl、docs\adr\ADR-039-ai-change-tracking.md、docs\ai-change-tracking\{architecture.md,db-schema.md,ipc-contract.md}、docs\test_checklists.md、engine_spec.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-14.md (実装メモ)

消した・止めたもの: 止めたプロセスなし (自分が起動した Editor.exe / mye_track.exe は全部自然に終了。実行後の mye_track.exe は 0)。消したのは自分が作った物だけ: %TEMP%\mye_m104o_proj、%TEMP%\mye_m104o_localappdata、src\Editor\ChangeTracking\ReviewProbe.h (スクショ用のプローブ)。既存の %TEMP%\mye_track_*、%TEMP%\mye_m104b\、tools\mcp\target\release\、cache\m104*_shots の既存分には触っていない。cache\m104o_shots\ と cache\m104o_*.out / .err は残した (コミットしない生成物)。

申し送り:
  - sub-15 (復元) へ: 復元の状態 (`changes.state` / `change_hunks.state` / `scene_changes.state`) には触れていない。レビューの `reviews` は `events` から作り直せるので、復元が Revision を足すときは行為者を `restore` にすれば集約に数えられない。
  - ユーザーの目視が要る: cache\m104o_shots の 7 枚 (偽の応答で描いたもの)、実サービス + 実 Editor でのレビュー操作 (docs\test_checklists.md の M104o)、自分の保存の直後に編集を続けても確認が出ないこと。
  - nit: `ReviewSelfTest` の窓の試験は FakeIo の応答に依存する。サービスが返す項目を変えたら、Rig の応答も合わせる。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、基点からの `src\Engine` の差分は `LocalizationTable.inl` と `HotReload\ReloadHub.*` だけ、`src\Shared` / `Interop.cs` / Runtime / Server / GameLogic に差分なし、スキーマ (`v1.sql`〜`v3.sql`) と `tools\mcp` に差分なし (移行は足していない)、スクショ用のプローブ (`ReviewProbe.h`) の残りなし、Debug / Release の `Editor.exe` は最後のソースの変更より新しい。planner の実走: `tools\track_verify.bat` 全シナリオ PASS。`cargo test` (track) は 5 回中 2 回が 265 件全部通過、**3 回は sub-14 より前からある試験が 1 件ずつ落ちた** (`tests\pipe.rs:811`、`tests\watch.rs:988`、`tests\watch.rs:780`。毎回違う。機械はユーザーの別のプログラムで重かった)。sub-14 が足した試験 (`tests\review.rs` 14 件ほか) は 5 回とも失敗していない。Editor 側 (ビルド、`changetracking`、`--selftest-full`、`replay_verify`、check_rules、スクショ) は coder の結果による (planner は Rust 側の揺れの確認に時間を使い、再実行していない)。
  - Rust 側の揺れは sub-14 の変更によるものではないと見て、sub-14 は OK。直しは新しい sub-26 (M104o2) で、復元 (sub-15) の前に行う (spec §2 D39)。
  - 追加・未実装の判定: `editor_user` / `restore` の Revision への `review_set` を断る、構造の変更を持つ Revision への hunk 範囲を断る、`list` の `review` フィルタを集約の 6 状態にする、読み出しの上限、`excludeApproved`、`DiffHunkInput` の `badge`、`OwnSaveRecord`、は全部採る。「操作全体」のまとめての導線と、変更 (ファイル) 単位の集約の表示が無いのは、要求 §10.2 の単位 (Operation / File) に対して足りないので、sub-15 で窓を触るときに足す (小さい追加)。
  - 質問への回答: (1) 操作全体のまとめての導線は sub-15 で足す (詳細の列に「この操作の変更をまとめてレビュー」)。(2) `editor_user` / `restore` への `review_set` を断るのはそのままでよい。復元が作る Revision にレビューを付ける要求は無い。(3) 変更 (ファイル) 単位の集約は、詳細の列の見出しに「このファイルの全 Revision」の状態と件数として出す (sub-15 で)。
  - 未確認: 実サービス + 実 Editor でのレビューの操作、自分の保存の直後に確認が出ないことの実機の確認、スクショ 7 枚の見た目 (ユーザーの目視)。