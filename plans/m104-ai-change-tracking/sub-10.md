# sub-10: AI Changes 窓、Diff 表示部品 (M104k)

- 依存: sub-09
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-09 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。
- 分割: `editor_saved` (Editor の保存をサービスへ知らせる) は **sub-23 (M104k2)** に分けた。このサブには入れない。順序は sub-10 → sub-23 → sub-11。

要求 §6.4、§15 Phase 3、`C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §7 (arch)、`attribution-rules.md` §8 (attr)、`ipc-contract.md` §4.3 (ipc)。

## sub-09 までにあるもの (使うもの)

- `src\Editor\ChangeTracking\`: `ChangeTrackingSession` (`Start` / `Poll` / `Shutdown` / `State` / `Model`)、`ChangeModel` (`SetInterest(mask)` / `SetFilter` / `LoadMore` / `SelectRevision`、status / sessions / list / describe の保持、通知で「古い」印)、`TrackClient` (`Request(op, args, onDone)`)。窓が開いている間だけ `SetInterest` を立てる (立てない部分は取りに行かない)。
- 設定: `EditorSettings::changeTrackingEnabled` (既定 false)。変えた後は `ChangeTrackingSession::Start` をやり直す (`NoService` の探し直しも同じ)。`EditorApp::allowChangeTracking` が偽 (非対話起動) のときは、設定が true でも接続しない。
- サービスの op: `status` / `list` / `describe` / `sessions` / `subscribe` / `shutdown`。`describe` は Hunk の要約 (行番号と行数)、`diff_mode`、`completeness`、`evidence`、`candidateSessions` を返す。
- 窓の足し方 (arch §7): `EditorApp` にメンバ、`layouts_.Init` の表、`OnImGui`、Window メニュー、`SetupDockLayout`。前例は `C:\HAL\MyEngin\src\Editor\Windows\Project\SourceControlWindow.cpp` (状態ごとに理由を 1 つ出す作りは 255〜269 行目)。

## やること

### A. サービス側 (`tools\track`): `hunks` と `blob_get`

ipc §4.3。どちらも `role = editor` だけ。読み取り専用の接続で引く (`ops.rs`)。`schema\v1.sql` は変えない。

- `hunks`: `revisionId`、`cursor?` → Hunk の配列 (`hunkId`、`ordinal`、`oldStart` / `oldCount` / `newStart` / `newCount`、`beforeText`、`afterText`、`contextPre`、`contextPost`、`eolOnly`、`state`)、`nextCursor`。1 回の応答が 1 行の上限 (1 MiB) を超えないよう、本文の合計で区切ってページにする (1 つの Hunk が単独で上限を超える場合は、その Hunk の本文を入れずに `oversize: true` と大きさだけ返し、Editor は `blob_get` で Before / After を取って自分で表示する。`diff_mode = whole` の大きいファイルがこれに当たる)。
- `blob_get`: `hash`、`offset`、`length` (最大 512 KiB) → base64 の中身、総サイズ。Blob が無ければ `blob_missing`。除外されたスナップショットのハッシュは存在しないので引けない。**`hash` は 64 桁の 16 進だけを受ける** (パスとして解釈される余地を残さない)。
- `protocol.rs` の `IMPLEMENTED_OPS` / `role_allows`、ipc の表、試験 (`tests\pipe.rs`、`tests\track\` のシナリオ)。

### B. `DiffView` (`src\Editor\Widgets\DiffView.h/.cpp`、新規)

Hunk の列を受けて描く、読み取り専用の部品。追跡に依存しない形にする (入力は自前の構造体。`ChangeTracking\` を include しない。sub-15 のプレビューでも使う)。

- 表示: インライン (1 列。削除行と追加行を上下に) と左右分割 (Before / After を横に並べ、行をそろえる)。切り替えは部品の状態。
- 行番号 (旧 / 新)、追加・削除の色分け (既存の `themeColor` に合う色。無ければこの部品の中の定数で、sub-12 の色の設定とは別)、Hunk の見出し (`@@ -a,b +c,d @@` 相当)、文脈行。
- `eolOnly` の Hunk は「改行だけの変更」と見出しに出し、本文の差として色を付けない。行末の種類 (LF / CRLF / なし) が変わった行に印を付ける。
- Hunk 単位の選択 (クリックで選ぶ。選択は呼び手へ返す。sub-14 / sub-15 が使う)。
- **見えている行だけ描く** (`ImGuiListClipper`)。全体の行数は入力を受けたときに 1 回数える。毎フレーム本文を走査しない・毎フレーム文字列を作らない。
- タブ、長い行 (横スクロール)、日本語、空の Before (追加)、空の After (削除)、Hunk が 0 個 (理由の文面を呼び手が渡す)。
- 文字列は呼び手が `Tr()` したものを渡すか、部品の中で `Tr()` を使う。`Tr()` の結果を書式文字列として渡さない (AGENTS.md §4.3)。
- **既存の Source Control の Diff 窓と `PerformanceCompareWindow` は変えない。**

### C. AI Changes 窓 (`src\Editor\Windows\Project\AiChangesWindow.h/.cpp`、新規)

1. **状態の帯**: `ChangeTrackingSession::State()` ごとに 1 つの文面 (無効 / プロジェクトなし / 接続中 / 動作中 / サービスなし / 版が違う / プロジェクト違い / DB が使えない / 切断・再接続待ち)。動作中のときは、ベースラインの進捗 (`scan_done` / `scan_total`)、追跡中のファイル数、Blob の総量、欠落区間の数、監視が 1 本へ戻っているか (`watches.fallback`)。
2. **有効・無効の切り替え**: チェックボックス 1 つ。有効にするときは 1 回だけ確認を出す (最初のベースラインでプロジェクトのファイルの内容が `.mye\ai_changes\blobs\` に複製されること。量は有効にする前には分からないので、文面は一般的な説明にとどめ、有効にした後の帯に実際の総量を出す)。無効にしても動いているサービスは止まらない (接続を閉じるだけ) ことを文面に書く。`allowChangeTracking` が偽の起動では切り替えを出さない (出しても効かないため、その旨を 1 行)。
3. **セッションの一覧** (左): `sessions`。provider、開始時刻、状態 (running / finished / interrupted)、変更の数。「セッションなしの変更」(Unknown / Inferred) の項目。
4. **変更の一覧** (中): 選んだセッション (または全部) の `list`。ページ単位の遅延読み込み (末尾に来たら `LoadMore`)。行ごとに、パス、種類 (追加 / 変更 / 削除 / リネーム)、帰属の区分、`completeness` が `complete` でない印 (`coalesced` / `gap` / `no_before`)、`superseded` の印。フィルタ (パス、種類、帰属)。
   - **Unknown と Inferred は、既定では別の節に分けて出す** (attr §8)。切り替えで混ぜられる。
5. **詳細** (右): 選んだ Revision の `describe` + `hunks`。
   - 帰属: 区分と、**根拠の一覧** (種類ごとの文面) と、候補のセッション。区分だけを見せない。
   - `completeness` が `complete` でないときは、その意味を必ず 1 行で出す (「この間に複数回書かれた可能性がある」「止まっていた間 / 取りこぼしを後から拾った変更」「変更前の内容を見ていない」)。`source = verify_gap` は「取りこぼしを後から拾った」と分かる文面。
   - `diff_mode` が通常なら DiffView。`whole` はその旨 + DiffView。`binary` / `excluded` / `unavailable` は Hunk が無い理由を出す。Blob が欠けているとき (`blob_missing`) は「内容を表示できない」。
   - 「対象ファイルへ移動」: 既存の外部エディタで開く経路 (`EditorSettings::externalEditorCmd` を使っている箇所を着手時に探す) があれば、Hunk の新しい側の先頭行で開く。無ければ、エクスプローラーで場所を開くまで。
6. **承認・復元の操作は置かない** (sub-14 / sub-15)。場所だけ空けてよいが、押せるものを置かない。
7. `EditorApp` への登録 (arch §7 の表のとおり)。窓名は `"表示名###AiChanges"` の形。既定のレイアウトでは閉じておく (Window メニューから開く)。窓が閉じている間は `SetInterest(0)`。
8. 文字列は全部 `C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl` + `Tr()`、英日。`###` の後ろの識別子と書式指定子の順序を両言語で一致させる (規則 10)。
9. 整形 (一覧の行の文字列、帰属と根拠の文面、`completeness` の注記、時刻の表示) は、ImGui を呼ばない関数に分け、スイートから呼べるようにする。時刻は UTC ミリ秒を現地時刻の文字列にする (既存の整形があれば使う)。

### D. 文書

- `C:\HAL\MyEngin\engine_spec.md` のエディタの章に AI Changes 窓 (何が見えるか、既定は無効、有効にすると何が起きるか)。
- `C:\HAL\MyEngin\docs\test_checklists.md` に手動確認の項目 (実サービスにつないで、ファイルを変えると窓に出る / 日本語と英語 / 大きい差分)。
- ipc (`hunks`、`blob_get`)、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表。M104i の行が「未着手」のまま古いので「完了」に直す。M104k2 の行を足す)。

## 画面の確かめ方

- 非対話起動 (`--screenshot`) では追跡が無効なので、そのままでは窓に中身が出ない。**スクショは一時のプローブで撮る** (このプロジェクトの流儀: モデルへ偽のデータを入れて窓を開く数行を一時的に足し、`--screenshot` で撮り、プローブを外す。プローブはコミットしない)。撮った画像の絶対パスを SELF_EVAL に書く。外した後に `git diff` にプローブが残っていないことを確かめる。
- 撮るもの: (1) 動作中 + セッションと変更の一覧 + インラインの Diff (2) 左右分割の Diff (3) Unknown / Inferred の節 + `gap` の注記 (4) 「サービスなし」の状態 (5) 英語の表示。Release ビルドで撮る。
- **見た目の合否はユーザーの目視** (spec §2 D18)。coder が確かめるのは「撮れた」「文字が切れていない・化けていない」まで。

## やらないこと (このサブでは)

- `editor_saved` (sub-23)。シーンの構造の表示・バッジ・ハイライト (sub-11、12)。レビュー・復元 (sub-14〜16)。色の設定 (sub-12)。
- Engine 層の変更。既存の Diff 窓の変更。
- 容量の回収。

## 受け入れ条件 (このサブ)

1. `hunks` がページ送りでき、全ページをつなぐと sub-04 の Hunk と一致する。単独で上限を超える Hunk は `oversize` で返る。`blob_get` が 512 KiB ずつ返し、つなぐと原本とバイト一致する。欠けた Blob は `blob_missing`。64 桁の 16 進でない `hash` は `bad_args`。`mcp` / `hook` の役割では `forbidden` → `cargo test --manifest-path tools\track\Cargo.toml` + `tools\track_verify.bat`。
2. 整形の関数: 偽の応答から組んだモデルで、一覧の行、帰属の区分 4 種、根拠 12 種の文面、`completeness` 4 種の注記、`diff_mode` 5 種の文面が、英日とも空でなく期待どおり → `changetracking` スイート。
3. Unknown / Inferred が既定で別の節に入り、切り替えで混ざる (節への振り分けの関数) → スイート。
4. DiffView の行の組み立て (ImGui を呼ばない部分): 複数 Hunk、`eolOnly`、空の Before、空の After、Hunk 0 個、CRLF と LF の混在、タブ、日本語、でインラインと左右の行の列が期待どおり。左右分割で、削除だけ・追加だけの行の反対側が空行になり、行がそろう → スイート。
5. DiffView が 1 万行の差分で、1 フレームに描く行が見えている範囲に限られる (描いた行数を数える口を持ち、ヘッドレスの ImGui フレームか、クリッパーへ渡す範囲の計算の試験で確かめる。既存のスイートに ImGui のフレームを回す前例があればそれに合わせる。無ければ範囲の計算だけを確かめ、その限界を書く) → スイート。
6. 窓が閉じている間、モデルが一覧を取りに行かない (`SetInterest(0)`)。追跡が無効の間、窓を開いても I/O の口が呼ばれない → スイート。
7. 設定の切り替え: 有効にすると `Start` がやり直されて接続へ進み、無効にすると接続が閉じる (サービスへ `shutdown` を送らない) → スイート (偽の I/O)。
8. スクショ 5 種が撮れている (上の「画面の確かめ方」)。プローブが `git diff` に残っていない → 画像のパスと `git diff`。
9. 規則 10 (ローカライズ) が通る。`tools\check_rules.ps1` が 0 error。規則 19 が通る (`DiffView` は `Widgets\` で、Engine 側から include されない)。
10. **実サービスでの確認 (手動 1 回)**: 一時プロジェクトで追跡を有効にして Editor を対話起動し、外からファイルを変えると、窓の一覧に出て、Diff が見える。確かめられなければ「未確認」と書く (成功扱いにしない)。**このリポジトリのルートをプロジェクトにして追跡を有効にしない。**
11. `Editor.exe --selftest` (Debug / Release) が全部通り、実行中と実行後に `mye_track.exe` が存在しない。`git diff --stat ca6a3991 -- src\Engine src\Runtime src\Server src\GameLogic src\Shared src\Scripting` に出るのは `LocalizationTable.inl` だけ (ローカライズの表は Engine 層にある。それ以外が出たら理由を書く)。
12. `build\Editor.vcxproj` / `.filters` の差分が新しいソースの追加だけ。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `--selftest` を Debug / Release
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\build_track.bat Release`、`tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- スクショ: 一時のプローブ + `bin\x64\Release\Editor.exe --screenshot <出力先>` (シーンの撮影は Runtime.exe、エディタの窓は Editor.exe)
- exe の実行は PowerShell から。`tools\mcp\target\release\` の exe に触らない (`mcp_verify` は回さなくてよい。`tools\mcp\src` を変えないため)。

## 実装メモ (coder が追記)

## フィードバック履歴
