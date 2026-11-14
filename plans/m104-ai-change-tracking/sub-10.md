# sub-10: AI Changes 窓、Diff 表示部品 (M104k)

- 依存: sub-09
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。大きければ着手時の見直しで M104k / M104k2 に割る (spec §7.2)。

## Phase 2 までの結果による追記 (2026-11-15、sub-08 の VERDICT。sub-09 の VERDICT で詳細にする)

- 質問の運用: 推奨で進め、Notion に後回し。このサブに着手前の確認は無い。
- **`editor_saved` をこのサブに入れる** (sub-09 から移した): Editor が自分で保存したファイル (シーン、プレハブ、アセットの `.meta` など) を、内容のハッシュ付きでサービスへ知らせる op (`role = editor`)。サービスは、観測した After が一致したら `editor_user` / `verified` にする (attr §3 の表の 1 行目。結論の関数には sub-06 で入っている)。Editor 側は、保存の経路がいくつあるか (`WriteFileReplacing` の呼び手) を着手時に洗い、**追跡が有効で接続中のときだけ**知らせる。知らせ損ねても害は無い (その保存が `unknown` か `inferred` に落ちるだけ) が、AI の変更の一覧に人の保存が混ざるので、主な経路 (シーンの保存、プレハブの Apply、アセットの移動・リネーム) は押さえる。
- 設定の切り替え (`EditorSettings::changeTrackingEnabled`、sub-09) の UI をこの窓に置く。有効にするときに、最初のベースラインで内容が複製されること (量の目安を `status` から出す) を 1 回だけ確認する。無効にしても、動いているサービスは止めない (接続を閉じるだけ) ことを文面に書く。
- サービスの状態 (sub-09 の `State()`)、ベースラインの進捗、Blob の総量、欠落区間、監視が 1 本に戻っているか (`watches.fallback`) を窓に出す。
- `describe` は `diff_mode` (通常 / `whole` / `binary` / `excluded` / `unavailable`) を返す。Hunk が無い理由を表示する。`changes.state = superseded` の扱い。
- 帰属の表示: 区分・根拠の一覧・候補のセッション (`describe` の `evidence` / `candidateSessions`)。`verify_gap` の Revision は「取りこぼしを後から拾った」と分かる表示にする。

## やること

要求 §6.4、arch §7、attr §8。

1. `src\Editor\Windows\Project\AiChangesWindow.*`: セッション履歴、セッション別・ファイル別の変更の一覧 (ページ単位の遅延読み込み)、帰属の区分と**根拠の一覧**、`completeness` が `complete` でない旨、サービスの状態 (追跡停止中など)、欠落区間。Unknown / Inferred は既定で別の節、切り替えで混ぜられる。
2. `src\Editor\Widgets\DiffView.*`: Hunk の列を受けてインラインと左右分割で描く読み取り専用の部品。追加・削除の色分け、Hunk 単位の選択、`eol_only` の表示、大きい差分でもフレームを止めない (見えている行だけ描く)。**既存の Source Control の Diff 窓からは使わない** (既存の描画を変えない)。
3. 対象ファイル・行への移動 (既存の「外部エディタで開く」経路があればそれを使う。着手時に確かめる)。
4. サービス側に `hunks` (本文付き、ページ送り) と `blob_get` が要る。sub-05 で作っていないので、このサブで足す (`role = editor`)。
5. `EditorApp` への登録: メンバ、`layouts_.Init` の表、`OnImGui`、Window メニュー、`SetupDockLayout` (arch §7 の表)。
6. 文字列は `LocalizationTable.inl` + `Tr()`、英日。`Tr()` の結果を書式文字列として渡さない (AGENTS.md §4.3)。
7. `engine_spec.md` のエディタの章、`docs\test_checklists.md` の手動確認の項目。

## やらないこと (このサブでは)

- 承認・復元のボタン (置き場所だけ空けてよいが、押せるものを置かない)。シーンの構造表示 (sub-11、12)。

## 触る場所 (planner の見立て)

- 新規: `AiChangesWindow.*`、`DiffView.*`。`C:\HAL\MyEngin\src\Editor\App\EditorApp.*`、`C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl`、`build\Editor.vcxproj`
- `C:\HAL\MyEngin\tools\track\src\` (`hunks`、`blob_get`)、`tests\track\`
- 参考: `C:\HAL\MyEngin\src\Editor\Windows\Project\SourceControlWindow.cpp` (窓の作り)

## 受け入れ条件 (このサブ)

1. 偽の応答から組んだモデルで、一覧の行・帰属の表示文字列・根拠の一覧・`completeness` の注記が期待どおりに整形される → `changetracking` スイート。
2. DiffView がインラインと左右の両方で、複数 Hunk・`eol_only`・空の Before (追加)・空の After (削除) を落ちずに描く。1 万行の差分で 1 フレームの描画行数が見えている範囲に限られる → スイート (描画行数の計数)。
3. 窓のスクショ (偽データ、インラインと左右、日本語と英語) が撮れている → `--screenshot` (Release)。見た目の合否はユーザーの目視 (spec §2 D18)。
4. 規則 10 (ローカライズ) が通る → `tools\check_rules.ps1`。
5. `hunks` がページ送りでき、`blob_get` が 512 KiB ずつ返し、欠けた Blob は `blob_missing` → `tools\track_verify.bat`。
6. 実サービスにつないで、実際にファイルを変えると窓に出ることを手動で 1 回確かめる。確かめられなければ「未確認」と書く → 手動。
7. `Editor.exe --selftest` が通る。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`tools\track_verify.bat`、`tools\check_rules.ps1`、`--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
