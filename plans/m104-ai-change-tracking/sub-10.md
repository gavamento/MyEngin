# sub-10: AI Changes 窓、Diff 表示部品 (M104k)

- 依存: sub-09
- 状態: OK (commit は台帳 harness.md を参照)
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

SELF_EVAL: sub-10 (round 1)
実装:
  - tools\track\src\ops.rs:hunks / blob_get / base64_encode — `hunks` は本文つきのページ送り (1 ページの Hunk の JSON 合計 768 KiB 以内。単独で収まらない Hunk は本文なしの `oversize` + `beforeBytes` / `afterBytes`)。`blob_get` は 64 桁の小文字 16 進だけ受け、512 KiB まで範囲読みして base64。欠けた Blob は `blob_missing`。`sessions` の各行に `changeCount` を足した。依存は足していない (base64 は自前、RFC のテストベクタで検査)
  - tools\track\src\protocol.rs / server.rs — IMPLEMENTED_OPS と role_allows (hunks / blob_get は editor だけ)、`blob_missing` コード、Context に blobs_dir
  - tools\track\tests\pipe.rs / tests\track\24_hunks_blobs.* — 40 Hunk (2.4 MB) が 3 ページ以上に分かれて全部そろう、2 MB の 1 行が oversize、Blob を 512 KiB ずつ 4 回で原本とバイト一致、不正な hash / offset / length、mcp・hook は forbidden。track_verify に 24 本目のシナリオ。`sessions` の changeCount で 12 / 15 / 16 / 17 / 18 の期待を撮り直し (差は changeCount の追加だけ。機械で確認)
  - src\Editor\Widgets\DiffView.h/.cpp — `DiffRows` (ImGui を呼ばない行の組み立て。インラインと左右の行を入力時に 1 回だけ組む。タブ展開・全角 2 桁・4096 バイトで切る・eolOnly・行末の印・oversize の注記・Hunk 見出し) と `DiffViewer` (ImGuiListClipper で見えている行だけ描く。Hunk 選択、LastDrawnRows)。追跡に依存しない (DiffHunkInput が入力)
  - src\Editor\ChangeTracking\ChangeFormat.h/.cpp — ImGui を呼ばない整形 (帰属 4 種・根拠 12 種・completeness 4 種・diff_mode 5 種・状態 9 種の文面、節への振り分け、行・セッション行、時刻、大きさ、プロジェクトの外を指さないパス結合)
  - src\Editor\ChangeTracking\ChangeModel.h/.cpp / TrackProtocol.h / TrackClient.cpp — TrackHunk、describe の後に hunks を全ページ取る (行のある diffMode だけ。取り終えるまで Detail は取得中。選び直しで古いページを捨てる。失敗は HunksFailed)、changeState / changeCount の解釈
  - src\Editor\Windows\Project\AiChangesWindow.h/.cpp — 状態の帯 + 要約、有効・無効のチェック (有効化は確認 1 回、無効化は接続を閉じるだけ、自動実行の起動では灰色 + 理由 1 行)、セッション / 変更 / 詳細の 3 列、Unknown・Inferred の別の節 (混ぜる切り替え)、根拠の一覧と候補のセッション、completeness の 1 行の注記、diff_mode の理由、Blob 欠け、インライン / 左右の切り替え、ファイルを開く (externalEditorCmd の経路) / エクスプローラー。承認・復元の操作は置いていない。閉じている間は SetInterest(0)
  - src\Editor\App\EditorApp.h/.cpp — メンバ、layouts_ の表、OnImGui、Window メニュー、SetupDockLayout (center、既定は閉じている)
  - src\Engine\Core\Localization\LocalizationTable.inl — 英日 159 行 (変換指定子なし。ウィンドウ名は "AI Changes###AiChanges")
  - src\Editor\ChangeTracking\ChangeTrackingSelfTest.cpp / ChangeViewSelfTest.cpp / .h — sub-09 の nit: モデルの試験 (g) を節ごとに独立 (ModelRig が節ごとに偽の I/O を持つ) に分けた。新しい試験: Hunk のページ送り・失敗・古いページ、窓の開閉と SetInterest(0)、無効の間の I/O ゼロ、設定の切り替え (shutdown を送らない)、ヘッドレスの ImGui フレームで窓を実際に描く (全 9 状態)、文面 (英日)、節、DiffRows、1 万行の差分で 1 フレームに描く行が見えている範囲だけ。MYE_TRACK_PROBE=1 に実サービスの hunks とライブのモデルの窓描画を足した
  - docs: engine_spec.md §9.2、docs\test_checklists.md (M104k)、docs\ai-change-tracking\ipc-contract.md (hunks / blob_get / changeCount)、plans\m104-ai-change-tracking.md (M104i を完了、M104k2 の行、M104k の行。M104j の行は表が割れていたのも直した)
仕様との差分:
  - [逸脱] クラス名は `DiffViewer` (ファイルは DiffView.h/.cpp のまま) — SourceControlState.h に既存の `mye::DiffView` があり、同名だと ODR 違反になる。既存の Diff 窓は変えていない
  - [追加] `sessions` に `changeCount` (service の ipc 追加。破壊なし) — 「セッション一覧に変更の数」を出す手段がサービスに無かった。既存の change_count() を使う。5 つの track_verify の期待が変わった (changeCount の追加だけ)
  - [未実装] oversize の Hunk を Editor が `blob_get` で原本を取って自分で表示する部分 — 注記 (サイズつき) と「ファイルを開く」までにした。Hunk が全体でない oversize (lines 内の巨大 1 行) は原本 Blob と対応しないので、Editor 側で差分を作り直す設計が要る。`blob_get` はサービス側と試験だけで、Editor からは呼んでいない
  - [逸脱] 「セッションなし (不明 / 推定)」の項目は、サーバに絞り込みが無いので、読み込み済みの行から不明 / 推定だけを出す (さらに読み込むと増える)
  - [逸脱] 1 行が 4096 バイトを超えると省略記号で切る (描画の費用の上限)。「長い行 (横スクロール)」は 4096 バイトまでの行が横スクロールで読める
  - [追加] 窓の既定ドック先は center (3 列 + 差分が横に広い)。既定で閉じているのでタブは作られない
  - [追加] M104j の行が表で割れていた (バックスラッシュの後ろが制御文字に化けていた) ので直した
検証:
  - MyEngine.sln Debug|x64 / Release|x64 (/p:MyeWarnAsError=true) → どちらも警告 0・エラー 0
  - MYE_SELFTEST_ONLY=changetracking Editor.exe --selftest (Debug) → ALL PASS (新しい試験を含む)
  - Editor.exe --selftest (Debug 72 s / Release 24 s) → 112 suites, 0 failed
  - MYE_TRACK_PROBE=1 + changetracking (Debug、実サービス) → 全 PASS (hunks の本文がモデルへ・追記が文脈つきの Hunk で届く・窓がライブのモデルを描く・shutdown 後に mye_track.exe が残らない・一時ディレクトリが残らない)
  - cargo test --manifest-path tools\track\Cargo.toml → 全 PASS (新しい pipe の試験を含む)
  - tools\build_track.bat Release → tools\track_verify.bat → 24 シナリオ全 PASS (23 + 新しい 24_hunks_blobs)
  - pwsh -NoProfile -File tools\check_rules.ps1 → 0 error (61 warning は既存)
  - git diff --stat ca6a3991 -- src\Engine src\Runtime src\Server src\GameLogic src\Shared src\Scripting → LocalizationTable.inl だけ (159 行の追加)
  - build\Editor.vcxproj / .filters の差分 → 新しいソース 7 本 (ChangeFormat、ChangeViewSelfTest、DiffView、AiChangesWindow の .cpp / .h) の追加だけ
  - スクショ 5 種 (一時のプローブ + Release の Editor.exe --screenshot。プローブは外した。外したあとのビルドと selftest が通り、diff にプローブの語が無い) → C:\HAL\MyEngin\cache\m104k_shots\1_running_inline.png (日本語。動作中 + 一覧 + インライン Diff)、2_split.png (左右分割)、3_sections_gap.png (Unknown / Inferred の節 + gap の注記)、4_no_service.png (サービスなし)、5_english.png (英語)。撮れた・文字が切れていない・化けていない・ImGui のエラー表示が出ていないまで確認。見た目の合否は目視
  - 実サービスでの GUI 操作 (一時プロジェクトで Editor を対話起動して外からファイルを変える) → 未確認 (Editor を操作できない)。代わりに MYE_TRACK_PROBE=1 で、実サービス → モデル → 窓の描画 (ヘッドレス) まで確認
  - tools\mcp_verify.ps1 → 未実行 (tools\mcp\src を変えていない。使用中の mye_mcp.exe に触らない)
  - CI での実走 → 未確認 (未 push)
自己採点 (1-5):
  仕様適合: 4 — 条件 1〜9・11・12 は検証済み。oversize を Editor が blob_get で表示する部分は未実装 (注記 + 外部エディタまで)、条件 10 の GUI 操作は未確認
  正しさ: 4 — 実サービスでの hunks / blob_get / モデル / 窓の描画を確認。ページ送り・oversize・Blob 範囲読み・不正 hash・役割は Rust の試験と track_verify が止める。クリックでの選択と有効化の確認モーダルは目視だけ (ヘッドレスではマウス入力を流していない)
  コード品質: 4 — 純関数 (DiffRows / ChangeFormat) と描画を分け、追跡に依存しない部品にした。ChangeViewSelfTest / AiChangesWindow が長め
  テスト: 4 — 行の組み立て・文面 (英日)・節・設定の切り替え・窓の開閉・ヘッドレスの描画・実サービスを検査。クリックと確認モーダルは未検査
不安・質問:
  - [oversize の扱い] 「Editor が blob_get で Before / After を取って自分で表示する」を、このサブでは注記までにした。lines 内の巨大 1 行 (原本 Blob と Hunk が対応しない) を正しく出すには、Editor 側で原本の 2 つの Blob から行 diff を作り直す設計が要る。whole の大きいファイルだけなら Blob の先頭を表示すれば足りる。どちらを sub-11 以降 (またはこのサブの round 2) でやるか判断してほしい
  - [changeCount] sessions に changeCount を足した (MCP の change_* の出力は変わっていない。期待が変わった行は、シナリオが pipe へ直接 sessions を呼ぶ行だけ)。これで足りるか、セッションごとの件数は別の op にすべきか
  - [クラス名] DiffView → DiffViewer の改名は、planner の仕様では sub-14 / sub-15 / sub-18 が「DiffView」と呼ぶ。以後の仕様の語を DiffViewer に直すか、既存の mye::DiffView を改名するか (既存の Diff 窓を触らない約束なので、前者にした)
触ったファイル:
  - tools\track\src\ops.rs、protocol.rs、server.rs、tools\track\tests\pipe.rs
  - tests\track\24_hunks_blobs.ndjson、24_hunks_blobs.expected.ndjson (新規)、12_managed_edit / 15_session_restart / 16_mcp_cold_start / 17_mcp_attach / 18_hook_verified の .expected.ndjson (changeCount の追加だけ)
  - src\Editor\Widgets\DiffView.h / .cpp (新規)
  - src\Editor\Windows\Project\AiChangesWindow.h / .cpp (新規)
  - src\Editor\ChangeTracking\ChangeFormat.h / .cpp (新規)、ChangeViewSelfTest.cpp (新規)、ChangeModel.h / .cpp、TrackProtocol.h、TrackClient.cpp、ChangeTrackingSelfTest.h / .cpp
  - src\Editor\App\EditorApp.h / .cpp
  - src\Engine\Core\Localization\LocalizationTable.inl
  - build\Editor.vcxproj、build\Editor.vcxproj.filters (gen_project_files.ps1 の出力。新しいソース 7 本の追加だけ)
  - engine_spec.md、docs\test_checklists.md、docs\ai-change-tracking\ipc-contract.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-10.md (実装メモ)
  - 生成物でコミットしないもの: cache\m104k_shots\ (スクショと run ログ)
申し送り:
  - tools\track_verify.bat は 24 シナリオ。mcp_verify は回していない (tools\mcp\src 不変)
  - 一時のプローブ (ChangeModel に偽データを入れる口 + EditorApp の環境変数 MYE_AICHG_PROBE) は外した。`git diff` に残っていない。プローブでは Editor.exe の `--screenshot` が自動実行扱いで、窓の本物の経路 (対話起動 → 設定 → Start) を通らない
  - 後始末: 自分で作った一時ファイル (%TEMP% の mye_ct_*.txt / mye_selftest_*.txt) を消した。止めたプロセスは、タイムアウトで自分が起動した Editor.exe (pid 6384、selftest が ImGui のエラーで止まったもの) だけ。`%TEMP%\mye_track_*` の既存分と `%TEMP%\mye_m104b\` と tools\mcp\target\release は触っていない。このリポジトリのルートでは追跡を有効にしていない (`.mye` を作っていない)
  - 窓の既定ドックは center。imgui.ini が既にある環境では、最初は浮いた窓として出る (Source Control と同じ挙動)
  - sub-09 の nit 2 件 (進捗表の M104i、モデルの試験を節ごとに独立) は対応済み

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、下位の層の差分は `LocalizationTable.inl` (159 行の追加) だけ、`MYE_AICHG_PROBE` が `src` / `tools` に残っていない、`src\Editor\SourceControl` と `SourceControlWindow.cpp` に差分なし、`Editor.exe` は最後のソースの変更より新しい。planner の実走: Release の `--selftest` 全体が 112 スイート 0 失敗で、実行中も実行後も `mye_track.exe` なし、`tools\check_rules.ps1` 0 error、`cargo test` (track) 197 件通過、`tools\track_verify.bat` 全シナリオ PASS。撮り直された期待値 5 本の差分は `changeCount` の追加だけ (機械で確認)。スクショ 5 枚があり、planner が 1 と 3 を開いて見た: 文字の切れ・化けなし、状態の帯、3 列、Unknown / Inferred の別の節、根拠の一覧、`gap` の注記、インラインの Diff と「改行だけの変更」の見出しが出ている。**見た目の合否はユーザーの目視 (未確認)。**
  - 逸脱の判定: クラス名を `DiffViewer` にした (既存の `mye::DiffView` と同名を避ける) のは採る。以後の仕様で「DiffView」と書いてあるのは `src\Editor\Widgets\DiffView.h` の `DiffViewer` / `DiffRows` のこと。「セッションなし」を読み込み済みの行から出す、4096 バイトで行を切る、も可。
  - 未実装 (oversize の Hunk を `blob_get` で取って表示する): このサブでは求めない。`whole` の大きいファイルについて、Before / After の先頭だけを `blob_get` で取って見せる処理を sub-15 に入れる (復元のプレビューで Editor が `blob_get` を使い始めるサブ)。`lines` の中の巨大な 1 行は、注記と「ファイルを開く」までで足りる (Editor 側で行 diff を作り直さない)。
  - 追加 (`sessions` の `changeCount`): 採る。別の op にしない。
  - 受け入れ条件 10 (実サービスでの GUI 操作) は未確認のまま (coder は Editor を操作できない)。`MYE_TRACK_PROBE=1` で実サービス → モデル → 窓の描画までは確かめてある。ユーザーの手動確認として `docs\test_checklists.md` に残す。
  - nit: 「セッションなし」の項目は、サーバの `list` の `attribution` フィルタ (unknown / inferred) を使えば、読み込み済みの行に限らず出せる。sub-23 か sub-14 で窓を触るときに直す。