# sub-14: レビュー (M104o)

- 依存: sub-24、sub-13 (順序は sub-24 → sub-25 → sub-13 → sub-14)
- 状態: 未着手
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

## フィードバック履歴
