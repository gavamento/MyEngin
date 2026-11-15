# sub-23: Editor の保存をサービスへ知らせる (M104k2)

- 依存: sub-10
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 順序: sub-10 → **sub-23** → sub-11 (番号は末尾。sub-09 の VERDICT で、sub-10 から分けた)
- 書き込みの深さ: **着手可** (2026-11-15、sub-10 の VERDICT で planner が見直した。保存の経路の洗い出しそのものがこのサブの仕事なので、経路の一覧は coder が作る)
- 質問の運用: 推奨で進め、Notion に後回し。

## 目的

Editor の利用者が Editor で保存したファイルが、AI Changes 窓で「不明な変更」や「AI の推定」に並ばないようにする。attr §3 の表の 1 行目 (`editor_saved` があれば `editor_user` / `verified`)。結論の関数には sub-06 で入っているが、根拠を作る op と、Editor から知らせる経路がまだ無い。

## やること

1. サービス側: op `editor_saved` (`role = editor` だけ。引数 `path`、`hash`)。ipc §4.4。`Store::record_event` を通し、派生の表 (根拠) を events から作り直せる形にする (spec §2 D26)。
   - 照合: 観測した Revision の After のハッシュが、知らせの `hash` と同じなら `editor_saved` の根拠を付ける。知らせと観測のどちらが先に届いても成り立つ (知らせを一定時間保持する。保持の上限と時間は定数。sub-06 の「後から届く根拠」と同じ作り)。
   - 知らせを受けたら対象パスをすぐ flush してよい (sub-06 の管理された操作と同じ `observe_managed`)。
   - **AI の操作の窓と重なったとき**: Editor の保存と、AI の `declared_edit_match` が同じ Revision に成り立つことは、内容が同じでない限り無い。attr §3 の表は `editor_saved` が先頭なので Editor が勝つ。この優先でよいかを試験で確かめ、attr に 1 行書く。
2. Editor 側: 保存の経路を洗う (`WriteFileReplacing` の呼び手と、それ以外の書き込み)。**追跡が接続中 (`State() == Ready`) のときだけ**、書いた内容のハッシュ (`Sha256Hex`) とパスを `editor_saved` で送る。押さえる経路: シーンの保存、プレハブ / アクターの保存と Apply、アセットの `.meta` の書き込み、アセットの移動・リネーム、設定ファイル (`.mye` の下は追跡の対象外なので不要)。洗った結果の一覧 (送る経路 / 送らない経路と理由) を文書に残す。
   - 送る場所は 1 か所にまとめる (保存のたびに各所から `TrackClient` を呼ばない)。Engine 層の `WriteFileReplacing` に手を入れない。Editor 層で、書いた後に知らせる小さい関数を呼ぶ形にするか、既存の「保存した」通知の集まる場所があればそこに足す。
   - ハッシュの計算がフレームを止めないこと (大きいファイルは保存そのものが既に同じ量の仕事をしている。二重に読み直さない。書いたバイト列から計算する)。
   - 送れなかった (切断中、時間切れ) 場合は何もしない (その保存が `unknown` / `inferred` に落ちるだけ)。保存を失敗させない・遅らせない。
3. 非対話起動 (`allowChangeTracking` が偽) と追跡が無効のときは、ハッシュも計算しない。

4. sub-10 の結果 (2026-11-15):
   - AI Changes 窓の整形は `src\Editor\ChangeTracking\ChangeFormat.h/.cpp` にある。行為者が `editor_user` のときの文面 (「Editor の利用者」) と、根拠 `editor_saved` の文面が既にあるかを確かめ、無ければ足す (英日)。
   - 「セッションなし (不明 / 推定)」の項目を、読み込み済みの行からではなく、`list` の `attribution` フィルタ (unknown / inferred) で取るように直す (sub-10 の nit)。`editor_user` / `restore` の変更は、既定では AI の変更の節にも不明の節にも出さず、「Editor / 復元による変更」の節 (切り替えで表示) に分ける。
   - サービスの op を足すときは `protocol.rs` の `IMPLEMENTED_OPS` と `role_allows`、ipc の表を更新する。`schema\v1.sql` は変えない (根拠は `attribution_evidence` に入る)。
   - スクショは sub-10 と同じ流儀 (一時のプローブ。コミットしない)。撮るのは「Editor の利用者」の変更が出ている一覧の 1 枚。

## やらないこと (このサブでは)

- Engine 層の変更。保存の経路そのものの変更。`restore_expect` (sub-15)。

## 受け入れ条件 (このサブ)

1. `editor_saved` の知らせと一致する観測が `editor_user` / `verified` になる。知らせが先でも後でも同じ。ハッシュが違えばならない。`mcp` / `hook` の役割では `forbidden` → `cargo test` + `tools\track_verify.bat`。
2. DB を壊して作り直しても、`editor_saved` の根拠が戻る → `cargo test`。
3. Editor の押さえた経路それぞれで、保存すると `editor_saved` が 1 回送られ、ハッシュが書いた内容と一致する (偽の I/O で、送られた行を確かめる) → `changetracking` スイート。
4. 追跡が無効・非対話起動・切断中は、送られず、ハッシュの計算も呼ばれない → スイート。
5. 保存の経路の一覧が文書にある (送る / 送らないと理由) → 文書。
6. 実サービスでの確認 (手動 1 回、一時プロジェクト): Editor でシーンを保存すると、AI Changes 窓でその変更が「Editor の利用者」として出る。確かめられなければ「未確認」と書く。
7. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1`、`cargo test`、`tools\track_verify.bat` が通る。`src\Engine` などの下位の層に差分が増えない。

## 検証コマンド

- ビルド (Debug / Release)、`Editor.exe --selftest`、`cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`、`pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-23 (round 1)
実装:
  - tools\track\src\protocol.rs: `IMPLEMENTED_OPS` に `editor_saved`、`role_allows` は `Role::Editor` だけ (mcp / hook は `forbidden`)
  - tools\track\src\ops_record.rs:`editor_saved` — path を正規化 (外は bad_args)、hash は 64 桁 16 進 (大文字は小文字へ)。対象パスをすぐ flush (`Source::Managed`、`op_end` と同じ即時の読み取り) してから、`editor_saved` イベントを `record_and_apply` で記録。server.rs の `record_op` に配線
  - tools\track\src\attribution.rs: 定数 `EDITOR_SAVED_WINDOW_MS` (10 秒、前後とも)。新しい Revision の評価 (`editor_saved_notice`: 先に届いていた知らせを `events` から引く) と、知らせの反映 (`apply_editor_saved`: 先にあった Revision に根拠を足す) の 2 経路で、知らせが先でも観測が先でも同じ結果。結論の書き込みを `recompute_conclusion` へ切り出し (`conclude` は変えていない = attr §3 の先頭行の `editor_saved` が AI の `declared_edit_match` に勝つ)。セッションが 1 つも無い場合の早期 return を、`editor_saved` の根拠があるときは通さないようにした。`gap` の Revision は対象外
  - tools\track\src\session.rs / revision.rs / observe.rs: `editor_saved` を `apply_event` / `is_session_event` / `rebuild_derived` の対象に。作り直しは全イベントを入れてあるので、`evaluate_new_revision` に `events_through` (処理中のイベントの `event_id`) を足し、記録したときと同じ順で根拠が入るようにした (作り直しの試験で、根拠・結論・evidence_id の並びまで一致)。`session_candidate_sql` は actor が `editor_user` / `restore` の変更を AI のセッションの候補から外す
  - tools\track\src\schema\v2.sql、schema\mod.rs: 移行 v2 = `events(kind, at)` の索引だけ (表・列は変えない。v1.sql は不変)。照合が全イベントの走査にならないため。tests\storage.rs は固定の版 1 / 2 / 索引 9 本を `latest_version()` 基準へ直し、テスト用の移行は「最新の次の版」を作る形にした
  - tools\track\src\ops.rs: `list` の `attribution` は文字列、または文字列の配列 (どれかに合う)。知らない値は bad_args
  - tools\track\tests\session.rs: 9 件追加 (知らせが後 / 先、ハッシュ違い・パス違い、窓より古い知らせ、AI の宣言と同じ内容で Editor が勝つ + セッションの一覧から外れる、引数の検査と役割、list の配列、作り直しで根拠が戻る、pipe 越しの役割と一覧)
  - src\Editor\ChangeTracking\ChangeTrackingSession.h/.cpp: `NoteSaved(absPath)` (Ready のときだけ、プロジェクトの中・.mye の外だけ順番待ちへ積む。同じファイルは 1 件、上限 256) と `DrainSaved` (`Poll()` の末尾。1 回に `kTrackSavedPerPoll` = 2 件、8 MiB 以下の通常ファイルだけ `Sha256FileHex` → `editor_saved`)。積んだあとに切れたら順番待ちを捨てる。ハッシュ関数は差し替え可 (試験が呼び出し回数を数える)。TrackProtocol.h に `trackop::kEditorSaved`
  - src\Editor\App\EditorApp.h/.cpp: `NoteFileSaved(absPath)` を新設 = 送る場所 1 か所 (Source Control の `HintSaved` + `changeTracking_.NoteSaved`)。`scmhint` の受け口、シーンの保存、アクターの保存、`.gitignore` の追記がここを通る
  - 通知の足りない経路を `scmhint::Changed` に繋いだ: InspectorWindow.cpp (プレハブの Apply 2 か所 = `ApplyPrefabInstanceAndNotify`)、InspectorWindowAssets.cpp (`.meta` 2 か所、マテリアル / サウンド / 物理マテリアル / ナビフィルタの保存ボタン)、FeelAssetInspector.cpp (Feel の保存)、AssetBrowserWindow.cpp (取り込みの `.meta`)、AssetOps.cpp (移動・リネームで同伴する `.meta`)
  - src\Editor\ChangeTracking\ChangeFormat.h/.cpp、ChangeModel.h/.cpp: 節に `Editor` を追加 (`ClassifyChange(actor, attribution, mix)`。editor_user / restore は常に Editor の節)、`AttributionNote(actor, attribution)` (Editor / 復元の変更を AI の「検証済み」の説明にしない)、一覧の行は Editor / 復元のとき帰属の区分でなく行為者の名前、`TrackListFilter.attributions` (1 つなら文字列、複数なら配列で送る)
  - src\Editor\Windows\Project\AiChangesWindow.h/.cpp: 「エディタ / 復元による変更」の節 (チェックで表示、既定は出さず「N 件…は非表示です」を出す)。「セッションなし (不明 / 推定)」を `attribution = [unknown, inferred]` のサービスへの絞り込みで取る形に直した (sub-10 の nit。`SelectScope` を公開して試験が同じ口を通す)。説明文は折り返す (狭い詳細欄で右が切れていたのをスクショで確認して直した)
  - src\Engine\Core\Localization\LocalizationTable.inl: 英日 5 本 (節の見出し、表示の切り替え、非表示の件数、Editor / 復元の説明) だけ。下位の層の差分はこれだけ
  - src\Editor\ChangeTracking\ChangeTrackingSelfTest.cpp / ChangeViewSelfTest.cpp: 保存の知らせの試験 (経路 8 形 × 3 検査、無効・非対話・プロジェクト無し・接続前・切断中・積んだあとの切断、外 / .mye / 重複、読めない・上限超過・上限ちょうど、1 Poll の上限と順序、キューの上限、サービスの拒否)、節・文面・窓の試験、`MYE_TRACK_PROBE=1` の実サービス結合に「Editor が保存と知らせたファイルは editor_user、外から書いたものは unknown」を追加
  - docs: 経路の一覧 docs\ai-change-tracking\editor-save-routes.md (新規)、ipc-contract.md (§4.4 の取り決め、list の attribution)、attribution-rules.md (§3 に Editor が勝つ旨と限界)、db-schema.md (版 2、イベントの payload)、engine_spec.md §9.2、docs\test_checklists.md (M104k2 の手動確認)、plans\m104-ai-change-tracking.md の進捗表
仕様との差分:
  - [逸脱] 「書いたバイト列から計算する・二重に読み直さない」 — Engine 層の書き手 (`SceneSerializer::SaveToFile`、`Prefab::SaveEdited`、各 `SaveToFile`) は書いたバイト列を返さず、`WriteFileReplacing` に手を入れない制約と両立しない。そのため**書き終わったファイルを 1 回読み直して** `Sha256FileHex` (ストリーム) でハッシュを取る。読み直しは次の `Poll()` 以降 (保存の直後ではない) で、8 MiB 以下・1 回 2 件まで。残る穴 (保存と読み直しの間に別のプロセスが書くと、その内容を Editor の保存として知らせる) は attr §3 に書いた
  - [追加] スキーマ版 2 (`events(kind, at)` の索引。v1.sql は不変、sub の「変えるなら移行 v2 以降」の範囲)。索引が無いと、新しい Revision ごとに全イベントを走査する。tests\storage.rs の固定の版の数え方を直した
  - [追加] `list` の `attribution` に配列 (ipc を更新。MCP の `change_list` は文字列のままで動く。tools\mcp\src は触っていない)
  - [追加] `session_candidate_sql` で editor_user / restore を AI のセッションの候補から外す (AI の根拠が残っていても、Editor が勝った変更がセッションの件数・一覧に残らないように)
  - [追加] `gap` の Revision は `editor_saved` の対象外 (観測の時刻が書いた時刻ではないので、時刻の窓が成り立たない。D35 の verify_gap も含めて外した)
  - [追加] 通知の入口を増やした (上の「実装」): プレハブの Apply、`.meta`、Inspector のアセット保存ボタン、移動で同伴する `.meta`、`.gitignore`。いずれも既存の `scmhint::Changed` / `NoteFileSaved` へ 1 行 (Source Control のヒントも増える。挙動は「バッジの更新が早まる」だけ)
  - [未実装] 知らせない経路: フォルダの移動 (個々のファイルのハッシュを取らない)、削除 (内容が無い)、Undo / Redo によるアセットの書き戻し (元から `scmhint` の口が無い)、生成物・ツールの出力。理由つきの一覧は editor-save-routes.md §2.2
  - [未実装] AI の窓と同じ時間に Editor が同じ内容を書いた場合の区別 (窓の前後 10 秒で同じ内容なら Editor を採る。attr に限界として書いた)
検証:
  - cargo test --manifest-path tools\track\Cargo.toml → 全 PASS (session 35 件 = 新規 9 件を含む、storage 18 件)
  - tools\build_track.bat Release → tools\track_verify.bat → 24 シナリオ全 PASS
  - MyEngine.sln Debug|x64 / Release|x64 (/p:MyeWarnAsError=true、全再ビルド) → どちらも警告 0・エラー 0 (最後のテスト編集の後に増分ビルドし直し、Editor.exe が最後のソース編集より新しいことを確認)
  - MYE_SELFTEST_ONLY=changetracking Editor.exe --selftest (Debug) → ALL PASS (FAIL 0、PASS 252 行)
  - Editor.exe --selftest (Debug 71 s / Release 25 s) → 112 suites, 0 failed。途中、Release のビルドと並走させた回だけ負荷で computemanaged と changetracking の時間依存の試験が落ちた (単独で再実行して全 PASS。並走をやめた)
  - MYE_TRACK_PROBE=1 + changetracking (Debug、実サービス) → 全 PASS (Editor の `NoteSaved` → 実ファイルの読み直し → 実サービス → モデルで actor = editor_user、外から書いたものは unknown、窓が Editor の節を描く、shutdown 後に mye_track.exe が残らない)
  - pwsh -NoProfile -File tools\check_rules.ps1 → 0 error (61 warning は既存)
  - git diff -- src\Engine → LocalizationTable.inl の 6 行 (5 キー) だけ。src\Shared / src\Scripting / Runtime / Server / GameLogic に差分なし。ABI / SimSnapshot / TypeId は触っていない
  - スクショ (一時のプローブ + Release の Editor.exe --lang ja|en --frames 50 --shot-frame 40 --screenshot。プローブは外した。外したあとの全ビルドと selftest が通り、git grep にプローブの語が無い) → C:\HAL\MyEngin\cache\m104k2_shots\1_editor_user_ja.png (日本語)、C:\HAL\MyEngin\cache\m104k2_shots\1_editor_user_en.png (英語)。「エディタ / 復元による変更」の節に「エディタの操作」の 3 件、詳細に「エディタがこの内容を保存したと知らせた」の根拠と折り返した説明が出ている。撮れた・文字が切れていない・化けていない、までは確認。**見た目の合否はユーザーの目視 (未確認)**。cache\m104k_shots\ は触っていない
  - 受け入れ条件 6 の手動確認 (実 Editor でシーンを保存する) → **未確認** (Editor を操作できない)。代わりに MYE_TRACK_PROBE の実サービス結合 (上) で、Editor 側の実コード (`NoteSaved` → 本物のハッシュ → 本物の pipe) から editor_user になるところまで確認。シーンの保存ボタン → `NoteFileSaved` の配線そのものは目視 / 手動
  - tools\mcp_verify.ps1 → 未実行 (tools\mcp\src を変えていない。使用中の mye_mcp.exe に触らない)
  - Editor.exe --selftest-full、tools\replay_verify.bat → 未実行 (sub の検証コマンドに無い。ABI・Engine 層のシミュレーションに触れていない)。CI での実走 → 未確認 (未 push)
自己採点 (1-5):
  仕様適合: 4 — 条件 1〜5・7 は検証済み。「書いたバイト列から計算」だけ逸脱 (Engine 層を触らない制約との両立不能で、理由を書いた)。条件 6 の実 Editor での手動確認は未確認
  正しさ: 4 — 知らせが先 / 後、窓の外、ハッシュ違い、AI との競合、作り直しの一致 (evidence_id の並びまで)、役割、引数、切断中・無効でハッシュを呼ばないことを試験で確認。残る穴は「保存と読み直しの間に他プロセスが同じパスへ書く」と、窓内で AI が同じ内容を書く場合 (どちらも attr に限界として記載)
  コード品質: 4 — 送る場所は `NoteFileSaved` 1 か所、サービス側は照合の経路が 2 つだが同じ根拠を同じ形で足す。`ChangeTrackingSelfTest.cpp` はさらに長くなった。経路の一部は既存の `scmhint` に 1 行ずつ足した (散らばっている書き手に合わせた)
  テスト: 4 — Rust は役割・照合・作り直し・pipe を、C++ は経路ごとの送信・無効時の無計算・上限・窓の節を、実サービス結合は端から端を確認。経路ごとの「実際に保存ボタンを押す」は手動 (EditorApp を試験で立てられない)
不安・質問:
  - [読み直しの採否] 上の逸脱。planner の許容できる形か。許容できないなら、Engine 層の書き手が書いたバイト列を返す口 (WriteFileReplacing は変えず、各 SaveToFile が戻り値で返す) を Engine 層に足す別サブが要る (影響範囲が大きいので勝手に足していない)
  - [スキーマ版 2] 索引だけの移行を足した。移行は `.bak-v1` を取るので、既に追跡を有効にしている利用者の DB は、サービスを新しくした最初の起動で 1 回コピーされる (大きい DB だと一瞬重い)。索引を足さず全イベントを走査する案 (却下: 新しい Revision ごとの O(イベント数)) と、版を上げずに起動時に `CREATE INDEX IF NOT EXISTS` を打つ案 (却下: v1.sql 凍結の趣旨に反する) があった
  - [10 秒の窓] 定数 `EDITOR_SAVED_WINDOW_MS`。観測のデバウンス (数百 ms) と Editor の 1 フレーム遅れ + pipe の遅れより十分長く、別の変更を巻き込むほど長くない値として置いた。実測ではない
  - [Editor の節の既定] Editor の保存による変更を既定で非表示にした (sub の指示どおり)。「N 件は非表示です」を出して存在は分かるが、これで足りるか
  - [既存の化け] docs\ai-change-tracking\ipc-contract.md に、前のサブから残っている制御文字 (TAB と \x13) が 3 か所ある (例: 22479 文字目付近の `tools<TAB>rack\src\notify.rs`、`tools\track\tests<TAB>...pipe.rs`)。このサブの範囲外なので直していない (今回足した行には制御文字が無いことを確認)
触ったファイル:
  - tools\track\src\protocol.rs、ops_record.rs、attribution.rs、session.rs、revision.rs、observe.rs、ops.rs、server.rs、schema\mod.rs、schema\v2.sql (新規)
  - tools\track\tests\session.rs、storage.rs
  - src\Editor\ChangeTracking\ChangeTrackingSession.h / .cpp、ChangeFormat.h / .cpp、ChangeModel.h / .cpp、TrackProtocol.h、ChangeTrackingSelfTest.cpp、ChangeViewSelfTest.cpp
  - src\Editor\Windows\Project\AiChangesWindow.h / .cpp
  - src\Editor\App\EditorApp.h / .cpp
  - src\Editor\Windows\Scene\InspectorWindow.cpp、InspectorWindowAssets.cpp、FeelAssetInspector.cpp
  - src\Editor\Windows\Asset\AssetBrowserWindow.cpp、src\Editor\Asset\AssetOps.cpp
  - src\Engine\Core\Localization\LocalizationTable.inl
  - docs\ai-change-tracking\editor-save-routes.md (新規)、ipc-contract.md、attribution-rules.md、db-schema.md
  - engine_spec.md、docs\test_checklists.md、plans\m104-ai-change-tracking.md (進捗表の M104k2 の行)、plans\m104-ai-change-tracking\sub-23.md (実装メモ)
  - 生成物でコミットしないもの: cache\m104k2_shots\ (スクショ 2 枚と run ログ)、bin\x64\Debug|Release\mye_track.exe (build_track.bat の出力)、tools\track\target
申し送り:
  - tools\track_verify.bat は 24 シナリオのまま。`editor_saved` の pipe 越しの役割の確認と一覧への反映は cargo test (tests\session.rs) が持つ
  - sub-11 (シーン構造 diff) 以降: Editor の保存の変更は actor = editor_user で `attribution = verified`。承認 (sub-14) / 復元 (sub-15) の対象に Editor の保存を含めるかは仕様に無いので、`actor` を見て決めること
  - sub-15 (`restore_expect`): `editor_saved` と同じ作り (イベント → 窓の中で同じ内容を観測 → 根拠) で足せる。`attribution.rs` の `editor_saved_notice` / `apply_editor_saved` と `session.rs` の `apply_event`、`revision.rs` の `rebuild_derived` の kind の並びに足す。`conclude` は `restore_expected` を既に持つ。AI Changes 窓の「Editor / 復元」の節と文面 (`AiChg_AttrRestoreNote`) は先に入れてある
  - 削除の Editor 確定は未対応 (Editor の削除は内容のハッシュが無い)。必要なら `editor_saved` に「消した」を表す形 (hash = null など) を足す
  - 後始末: 自分が作った一時ファイル `%TEMP%\mye_track_corrupt_32300` (最初の cargo test で落ちた storage の試験が残したもの) を消した。`%TEMP%\mye_track_*` の既存分 (mye_track_pipe_hunks_blob_3260 を含む) と `%TEMP%\mye_m104b\` は触っていない。止めたプロセスは無い (自分が起動した Editor.exe / mye_track.exe はすべて自分で終了済み。確認済み)。MSBuild のノードプロセス (自分のビルドが起動したもの) は idle で残っているが止めていない。`tools\mcp\target\release` とユーザーの `mye_mcp.exe` には触っていない。`C:\HAL\MyEngin\.mye` は作っていない

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、下位の層の差分は `LocalizationTable.inl` (7 行) だけ、`schema\v1.sql` と `tools\mcp` に差分なし、プローブの語が残っていない、`Editor.exe` は最後のソースの変更より新しい。planner の実走: Release の `--selftest` 全体が 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`tools\check_rules.ps1` 0 error、`cargo test` (track) 206 件通過、`tools\track_verify.bat` 全シナリオ PASS。
  - 逸脱 (書いたバイト列からではなく、書き終わったファイルを読み直してハッシュを取る): 採る。Engine 層の書き手がバイト列を返さず、`WriteFileReplacing` に手を入れない制約の方を守った判断でよい。ただし「保存と読み直しの間に別のプロセスが書くと、その内容を Editor の保存として知らせてしまう」穴は、**外の変更を Editor の利用者として確定する**向きの誤りなので、狭めておく: `NoteSaved` の時点 (保存の直後、同じ呼び出しの中) でファイルの大きさと更新時刻を控え、ハッシュを取る直前と直後に同じであることを確かめ、違えば送らない。sub-12 (次に Editor 側を触るサブ) に入れた (should)。
  - 追加の判定: スキーマ版 2 (索引だけ)、`list` の `attribution` の配列、Editor / 復元の変更を AI のセッションの候補から外す、`gap` を対象外にする、通知の入口を既存の `scmhint` に足す、は全部採る。
  - 質問への回答: (1) 読み直しは上のとおり許容し、狭める。Engine 層へ口を足す別サブは作らない。(2) 版 2 の索引の移行はよい。(3) 10 秒の窓は初期値でよい。(4) Editor の節を既定で出さず件数だけ出すのは、それで足りる。(5) `ipc-contract.md` の制御文字は sub-11 で直す。
  - 受け入れ条件 6 (実 Editor でシーンを保存する手動確認) は未確認のまま。見た目の合否 (`cache\m104k2_shots\` の 2 枚) もユーザーの目視。