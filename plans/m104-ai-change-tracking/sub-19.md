# sub-19: プレハブ伝播の表示、大きいシーンの計測、仕上げ (M104t)

- 依存: sub-18
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-16、sub-18 の VERDICT で planner が書き直した)。Phase 6 の最後のサブ (この後は配布の sub-20 だけ)。最後の横断の検証 (A20) もここで回す。
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

arch §6.4、§11。要求 §7.4、§14.1、§16.3 の 5 番目 (Prefab 伝播変更を直接編集と区別できる)。

## やること

### A. Debug のスタックオーバーフローの原因の確かめ (最初にやる。spec §2 D38、D43)

sub-18 の coder が見つけたこと: `EditorApp` に `RenderSystem` を 2 つ直に持たせた版で、Debug の `behaviortree` スイートがスタックオーバーフローした (`unique_ptr` にして解消)。`C:\HAL\MyEngin\src\Editor\App\EditorMain.cpp` 583 行目で `mye::EditorApp app;` が `wWinMain` の**ローカル変数**なので、`--selftest` が `EditorApp` を作る前に戻る経路でも、`wWinMain` のスタックの枠に `EditorApp` の大きさぶんの領域が確保されている見込み (推測)。すると、M104 で `EditorApp` のメンバが増えるほど、Debug の自己テストが使えるスタックが減る。**D38 (Debug の `physics` が開始直後にスタックオーバーフロー) が同じ原因である可能性がある。** planner が 2026-11-16 に、`MYE_SELFTEST_ONLY=behaviortree,physics` の Debug で、physics が今も開始直後に終了コード -1073741571 で落ちることを確かめた。

1. 確かめる (推測で直さない):
   - `sizeof(EditorApp)` を、基点 `ca6a3991` の版と今の版で比べる (`static_assert` か一時のログで。一時の変更はコミットしない)。基点の版は別の作業ツリーでビルドしなくても、ヘッダの差分から見積もれるならそれでよい。見積もりなら見積もりと書く。
   - 今の Debug の exe のコピー (リポジトリの外) を `editbin /STACK:` で大きくして、`MYE_SELFTEST_ONLY=physics` が通るかを見る (sub-18 の coder がしたのと同じ確かめ方)。
2. 通るなら、**`EditorMain.cpp` の `EditorApp` をヒープに置く** (`std::make_unique<EditorApp>()`。作る場所は今と同じ、`--selftest` などが戻った後)。Editor 層の変更で、Engine 層に触れない。その後、Debug の `MYE_SELFTEST_ONLY=physics` と、physics を含む Debug の全スイートを回す。通れば D38 は解消として、spec と sub-14 / sub-16 の「physics を除く」の扱いを外す (文書だけ。過去のサブの結果は書き換えない)。
3. 通らない (スタックを大きくしても physics が落ちる、またはヒープに置いても落ちる) なら、D38 は M104 と別の原因として残し、分かったことを spec の D38 に書き足す。**physics 自体のコード (Engine 層) は直さない。**

### B. プレハブ伝播 (`PrefabPropagation`)

arch §6.4。プレハブのベースが変わったとき、そのプレハブを参照しているインスタンスのうち、override していないフィールドが、ロード時 (`RefreshNonOverridden`) とホットリロード時 (`PropagateBaseChange`) に**メモリ上で**変わる。ディスクのシーンファイルには現れない。

1. サービス (`tools\track`): プレハブのファイル (`*.prefab.json`、`*.actor.json`) の Revision で `PrefabAssetChanged` と中のフィールドの変更が記録されたとき、**そのプレハブを参照しているシーンの文書** (`PrefabInstanceComponent` の `prefabHash` で引く。今のディスクのシーンの JSON を読む。読めないシーンは飛ばして数を出す) の、各インスタンスについて、変わったフィールドのうち**そのインスタンスが override していないもの**を `PrefabPropagation` の行として `scene_changes` に記録する。`caused_by` に原因のプレハブの変更の行を入れる。`before_value` / `after_value` はプレハブの Before / After の値 (インスタンスのライブの値は分からない。Editor Snapshot が無いため)。
   - `prefabHash` と、プレハブのファイルの対応 (ハッシュの作り方) を、着手時にエンジンのコード (`PrefabInstanceComponent`、プレハブのライブラリ) で確かめる。対応が取れない場合は、伝播の行を作らずにその理由を文書に書き、以下の 2〜4 は「対応が取れた範囲」で行う。
   - 入れ子のプレハブ (プレハブの中のプレハブ) は、直属のベースだけを対象にする。入れ子の先までは追わない (限界として出す)。
   - 対象のシーンが多い・大きいときの上限 (読むシーンの数、エンティティの数)。超えたら打ち切りと件数。
   - events から作り直せること (spec §2 D26)。スキーマの版は 3 (足すなら v4)。
2. `scene_overlay` (sub-12) は、伝播の行を「伝播」と分かる形で返す (直接の変更とは別の印)。`scene_diff` も。
3. Editor: Hierarchy のバッジと Inspector の印で、伝播による変更を直接の編集と区別する (別の印・別の色。色は sub-24 の設定に 1 つ足すか、既存の色を薄くするかを決める)。AI Changes 窓の一覧で、伝播の行から原因のプレハブの変更へ移動できる。
4. 復元: 伝播の行は戻す対象にしない (restore §3.3)。「復元…」を選ぶと、原因の変更へ案内する文面を出す (sub-16 の `ClassifySceneRow` が `Unsupported` に落としている分の文面)。

### C. 計測と軽量化 (AGENTS.md §3.5: 推測で最適化しない)

1. 計測する (結果を `C:\HAL\MyEngin\docs\ai-change-tracking\performance.md` に、環境・コマンド・値で):
   - 大きいシーン (エンティティ 2 万個の生成。sub-11 の計測と同じ) で: 構造 diff (既存)、`scene_overlay` の応答の時間、Editor の `SceneChangeOverlay` の取り直しと `RebuildAncestors`、Hierarchy のバッジの描画、Scene View の箱とゴーストの候補作り (毎フレーム `FindByFileId`)、Inspector の印、1 フレームの合計の増分 (追跡なし / ありの比較)。
   - 変更の件数を 10 / 1000 / 2 万で変える。
   - 実サービスから 1 万行相当が届いたときの `TrackClient::Poll()` 1 回の時間 (sub-25 の残り)。
   - 数万ディレクトリのツリーでの、監視の分け方の計画づくりの時間と監視の本数 (sub-22 の残り。`mye_track bench plan` を生成したツリーで)。
2. 見直す (計測の結果で決める):
   - Scene View の箱とゴーストの可視の判定とピックが原点だけ (`SceneChangeWireBoxes` の `ProjectChangePoint` の 1 か所。sub-17 / sub-18 の残り)。AABB で判定する費用を測り、変えるかを決める。変えるなら、ピックと重ねの印の位置も同じ判定にする。
   - 遅い箇所があれば、計測で特定してから直す。直したら前後の値。
3. 性能の目標値を、計測値を根拠に arch §11 に置く (「目標値はまだ置かない」の段落を書き換える)。

### D. 仕上げ

1. **要求 §16 の全項目の対応表**を `C:\HAL\MyEngin\docs\ai-change-tracking\acceptance.md` (新規) に作る。各項目について「自動の試験 (スイート名 / `cargo test` の試験名 / シナリオ名)」「手動の確認 (`docs\test_checklists.md` の節)」「未確認」を区別する。**未確認を成功扱いにしない。** 要求 §18 の完成条件 (機能面 8、安全性 7、検証面 7) も同じ表に。
2. `C:\HAL\MyEngin\docs\adr\ADR-039-ai-change-tracking.md` の状態を「実装済み (配布は M104u)」にし、「影響」と「未決のまま残したもの」を実装に合わせる (サブを足した経緯: sub-21〜27。D38 の結論)。
3. `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md`: 進捗表 (全サブ)、「未決事項」(計画の未決事項 1〜6 の結論)、「申し送り」。
4. `C:\HAL\MyEngin\docs\test_checklists.md`: M104 の手動確認の節を 1 つにまとめた「通しの確認」(追跡を有効にする → AI が編集 → 窓で見る → レビュー → 復元 → シーンの比較) を足す。
5. `C:\HAL\MyEngin\engine_spec.md`: エディタの章とホットリロードの章が実装と合っているか。

### E. 最後の横断の検証 (A20)

- `bin\x64\Release\Editor.exe --selftest-full` が全部通る。Debug は、A で D38 が解消すれば physics を含めて全部、解消しなければ physics を除いて全部 (作業ディレクトリはリポジトリ)。実行中と後に `mye_track.exe` が無い。
- `tools\replay_verify.bat` が通る (A で `EditorMain.cpp` を変えた場合は必ず)。
- `tools\check_rules.ps1` 0 error、`tools\check_rules_selftest.ps1` PASS。
- `cargo test` (track、mcp)、`tools\track_verify.bat`、`tools\mcp_verify.bat` (`tools\mcp\src` を変えていなければ省略可。省略したら書く)。
- ABI・SimSnapshot・TypeId が変わっていない: `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` が空。`git diff --stat ca6a3991 -- src\Engine` に出るのは `LocalizationTable.inl` と `HotReload\ReloadHub.*` だけ。
- `git status`: 復元による変更なし、`.mye` なし。リポジトリ直下の `imgui.ini` の更新時刻が作業の前後で同じ。

## やらないこと (このサブでは)

- 入れ子のプレハブの先までの伝播。伝播の行の復元。
- 計測の根拠が無い最適化。インクリメンタルな構造 diff などの大きい作り直し (必要と分かったら別サブとして提案する)。
- physics のコードの修正 (A の 3)。Engine 層の変更 (変わってよいのは `LocalizationTable.inl` だけ)。
- 配布物 (sub-20)。

## 受け入れ条件 (このサブ)

1. A: `sizeof(EditorApp)` の比較 (または見積もり)、スタックを大きくした exe での `physics` の結果、ヒープに置いた後の Debug の `physics` の結果が、`sub-19.md` の実装メモと spec の D38 にある。解消した場合は、Debug の全スイート (physics を含む) が通る。
2. B: プレハブのベースのフィールドを変えると、そのプレハブのインスタンスのうち override していないものに `PrefabPropagation` が出て、override しているものには出ない。`caused_by` が原因を指す。DB を作り直しても同じ → `cargo test` (要求 §16.3 の 5 番目)。
3. B: 伝播による変更と直接の編集が、Hierarchy のバッジ・Inspector の印・AI Changes 窓の一覧で区別される。伝播の行から原因の変更へ移動できる。伝播の行で「復元…」を選ぶと原因の変更へ案内される (戻せない) → `changetracking` スイート + スクショ。
4. C: 計測の手順・環境・値が `performance.md` にあり、同じコマンドで取り直せる。軽くした箇所は前後の値がある。可視の判定を変えたかどうかと、その根拠がある。arch §11 に目標値がある → 文書。
5. D: `acceptance.md` に要求 §16 と §18 の全項目があり、「自動の試験 / 手動の確認 / 未確認」が区別されている。ADR-039・計画・`engine_spec.md`・`test_checklists.md` が実装と合っている → 文書。
6. E の全部。
7. スクショ (一時のプローブ。窓を出す起動の作業ディレクトリはリポジトリの外): 伝播の印 (Hierarchy、Inspector)、一覧の伝播の行と原因への移動、英語。**見た目の合否はユーザーの目視。**

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=physics` (Debug。A の前後)、`--selftest-full` (Release)、Debug の全スイート
- `tools\replay_verify.bat`
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\build_track.bat Release` (サービスを変えたら。`mye_track.exe` が動いていないことを確かめてから)、`tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`、`pwsh -File tools\check_rules_selftest.ps1`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`、`git diff --stat ca6a3991 -- src\Engine`、`git status --short`
- 作業の注意: 窓を出す起動 (`--screenshot`、`--frames`) は作業ディレクトリをリポジトリの外に、`--selftest` はリポジトリで。`editbin` などで exe を変えるときは、リポジトリの外のコピーで行う (`bin\x64\` の exe を書き換えない)。`tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-19 (round 1)
実装:
  - src\Editor\App\EditorMain.cpp:wWinMain — [A] `EditorApp` を `wWinMain` のローカル変数から `std::unique_ptr` (`appHolder`) へ移し、以降は参照 `app` で使う (作る場所は同じ。`--selftest` などが戻った後)。`<memory>` を追加。
  - tools\track\src\propagation.rs (新規) — [B] プレハブ伝播の影響範囲の計算。`prefab_hash` (`.meta` の guid、無ければ正規化した絶対パスの FNV-1a 64 = エンジンの `HashForPath`)、`propagate` (`snapshots` の各シーンの最新の記録を読む。`prefabHash` の数字を含むものだけ JSON として読む。インスタンスのメンバの `PrefabLink.localId` とプレハブ側のエンティティの `fileId` を対応させ、`overrides` に無い項目だけ行にする)、上限 (読むシーン 300、合計 256 MiB、1 シーン 16 MiB、出す行 20000)、要約 (`Outcome`)。
  - tools\track\src\revision.rs:write_document_rows / write_propagation / write_revision — プレハブ / アクターの Revision を書くとき、原因の行 (`FieldModified`、`EntityModified` の `name` / `tagMask`) の `scene_change_id` を集め、入れ子の SAVEPOINT の中で `PrefabPropagation` の行を足す (`revision_id` は原因の Revision、`document_id` は影響を受けるシーン、`caused_by` は原因の行)。失敗しても Revision と Hunk は残す。`write_revision` に `propagation::Env` を渡す (observe.rs と `Deriver` が同じ経路)。`Deriver.root` を足した。
  - tools\track\src\store.rs / startup.rs / observe.rs / lib.rs — `Store.project_root` を足し、`Deriver` へ渡す。`pub mod propagation`。
  - tools\track\src\scene_diff.rs — `PREFAB_PROPAGATION` を足す。**`PrefabInstanceChanged` の判定のコンポーネント名を `PrefabInstanceComponent` から登録名の `PrefabInstance` に直した** (実データでは一度も成り立っていなかった。単体試験の入力も同様)。
  - tools\track\src\scene_overlay.rs — 行に `cause_path` (伝播の行だけ、原因のプレハブのパス) を持たせ、`propagated: true` / `causePath` を伝播の項目だけに返す。伝播の行は確認済みの除外 (approved hidden) の対象にしない。
  - tools\track\src\ops.rs (`scene_diff`)、review.rs、notify.rs — `scene_diff` の伝播の行に `causedBy` (他の行には付けない。既存の NDJSON の形を変えない)。確認の葉の列挙・数え上げ (3 か所) は `PrefabPropagation` を除く。`change_recorded` の `documentId` の取り方に `ORDER BY` を足した。
  - tools\track\tests\propagation.rs (新規) — 結合 3 件 (override していないものだけ・原因・`scene_overlay`・葉の数・DB を作り直しても同じ / 旧形式と読めないシーンの要約 / 削除済みのシーンと参照しないシーンの除外) + 計測 2 件 (`--ignored`)。
  - src\Editor\ChangeTracking\ChangeModel.h/.cpp、SceneChangeOverlay.h/.cpp、ChangeFormat.cpp、InspectorChangeMarks.cpp、HierarchyWindow.cpp、AiChangesWindow.h/.cpp — [B] 伝播を直接の編集と区別: Hierarchy のバッジと Inspector の見出し・フィールドの印を塗りの丸でなく輪にする、ツールチップに原因のプレハブ、一覧の「プレハブの変更の波及」の行、要約の行の文、「原因の変更へ移動」(選んだ行の `causedBy` の行へ選択を移し、一覧を送る)。プレハブ / アクターの変更の通知で開いているシーンの表を取り直す。
  - src\Editor\ChangeTracking\SceneRestorePlan.h/.cpp、SceneRestoreFormat.cpp — 伝播の行は `SceneRestoreIssue::PropagatedRow` (どのシーンを開いていても、`NotOpenDocument` より先に判定) で戻せず、原因の変更へ案内する文面。
  - src\Engine\Core\Localization\LocalizationTable.inl — 文字列 5 つ (`AiChg_BadgePropagated`、`AiChg_InspPropagated`、`AiChg_SrIssuePropagated`、`AiChg_ScGoCause`、`AiChg_ScPropLimits`。英日)。
  - src\Editor\ChangeTracking\SceneChangeOverlaySelfTest.cpp — `TestPropagation` (印・文面・`causedBy`・要約)、`TestPerformanceProbe` (環境変数 `MYE_CHANGETRACKING_PERF` があるときだけ動く計測)。SceneRestoreSelfTest.cpp — 伝播の行の判定 2 件。
  - docs\ai-change-tracking\performance.md §10 (計測の手順・環境・値・判断・目標値)、architecture.md §6.4 / §11 (実装と目標値)、ipc-contract.md、db-schema.md、restore-safety.md、acceptance.md (新規。要求 §16 の 31 項目と §18 の 22 項目)、docs\adr\ADR-039、plans\m104-ai-change-tracking.md (進捗表 25 行を完了、未決事項の結論、申し送り)、engine_spec.md (伝播の節)、docs\test_checklists.md (M104t の節と「M104 通しの確認」11 ステップ)、spec.md の D38 / D43 の結論。

仕様との差分:
  - [逸脱] 伝播の入力は「今のディスクのシーンの JSON」ではなく、`snapshots` の各シーンの**最新の記録 (Blob)**。ディスクを読むと DB を作り直したときに同じ行にならない (spec D26)。観測が追いついていれば内容は同じ。
  - [逸脱] 伝播の行の `revision_id` は**プレハブ側の Revision** (スキーマの `change_revisions.caused_by` を使った別の Revision は作らない)。シーンに変更の記録が無い (ディスクのシーンは変わらない) のに、シーン側の Revision を偽って作らず、確認 (レビュー) と一覧の対象を増やさないため。代わりに確認の葉の列挙・数え上げ (review.rs 3 か所) で `PrefabPropagation` を除き、`scene_overlay` では伝播の行を確認済みの除外の対象にしない。影響: プレハブの Revision の `scene_diff` に、他のシーンの行が (文書の ID つきで) 並ぶ。
  - [追加] 一度も変更の記録が無いシーンの `documents` の行を、伝播の行の外部キーのために、その場で作る (`.meta` の最新の記録の guid、無ければパスから。`document_for_path` と同じ規則)。
  - [追加] `PrefabInstanceChanged` のコンポーネント名の不具合を直した (上記)。spec / db-schema の `PrefabInstanceComponent.prefabHash` は JSON の登録名 `PrefabInstance` に直した。
  - [追加] 伝播の要約の行 (`file_id` なし、`after_value` に件数の JSON): 読めない・読まなかった・省いた・旧形式 (`overrides` キーなし) の数を、黙って欠かさず残すため。
  - [追加] `MYE_CHANGETRACKING_PERF` で動く計測のコード (`TestPerformanceProbe`) と、`--ignored` の計測 2 件。「同じコマンドで取り直せる」ため。
  - [追加] プレハブ / アクターの変更の通知でも開いているシーンの表を取り直す (`SceneChangeOverlay::OnNotification`)。伝播の表示が、シーンを開き直すまで出ないのを避けるため。
  - [未実装] 伝播の行は、プレハブ側のフィールドの値の変化だけ (エンティティの追加・削除、親子の変更、コンポーネントの追加・削除は出さない)。入れ子のプレハブの連鎖も追わない (サブ仕様どおり。限界として architecture.md §6.4 に書いた)。
  - [未実装] 「引けなかった」アセットの件数を別に出す (sub-18 の任意項目)。やっていない。
  - [未実装] 可視の判定とピックは変えていない (原点だけのまま)。計測して、AABB にしても利益が見合わないと判断した (根拠は performance.md §10.3)。決めたことの記録。
  - [未実装] 1 フレームの合計の増分 (追跡なし / ありの比較) は測れていない。ヘッドレスでは Scene View を描かないため、毎フレーム走る部分 (Hierarchy の描画、Scene View の候補作りと選び方) を個別に測った。
  - [逸脱] sub-14 / sub-16 / sub-18 の記述の「Debug は physics を除く」は書き換えていない (過去のサブの結果。D38 の解消は spec D38 / D43 と計画の申し送りに書いた)。planner が外すなら外す。
  - [追加] spec.md の D38 / D43 の結論の欄に、解消の事実を書き足した (サブ仕様の A-2 が指示。coder は spec の本文を書き換えない原則との境目なので、不安・質問にも書く)。

検証:
  - [A の確かめ] `sizeof(EditorApp)` = 204096 バイト (一時の `ShowSize<>` を置いて `MSBuild /t:ClCompile` のエラーから読んだ。置いた分は戻した)。M104 で足した 6 メンバ (`ChangeTrackingSession` 5136、`OwnSaveRecord` 24、`AiChangesWindow` 3400、`RestoreRunWindow` 440、`SceneRestoreDialog` 1016、`ScenePreviewWindow` 1128 = 11144) から、基点は約 193 KB (見積もり。基点はビルドしていない)。
  - [A] リポジトリの外 (`%TEMP%\mye_m104t_a`) の Debug の exe の複製: そのまま `MYE_SELFTEST_ONLY=physics` → 終了コード -1073741571 (落ちた)。`editbin /STACK:8388608` した複製 → 終了コード 0 (physics PASS 52.8 秒)。`EditorApp` をヒープへ置いた Debug → `MYE_SELFTEST_ONLY=physics` PASS (54.6 秒、終了コード 0)。
  - MyEngine.sln を Debug|x64 と Release|x64 で `/p:MyeWarnAsError=true` → どちらもエラー 0 (最終のソースの状態で)。
  - Release `bin\x64\Release\Editor.exe --selftest-full` → 125 スイート 0 失敗 (247.5 秒)。Debug `bin\x64\Debug\Editor.exe --selftest-full` → **125 スイート 0 失敗 (840.4 秒。physics を含む。`physics` PASS 54.7 秒、`behaviortree` PASS)**。作業ディレクトリはリポジトリ。実行中も後も `mye_track.exe` なし。
  - `cargo test --manifest-path tools\track\Cargo.toml` → 全部 ok (単体 82、attribution 23、diff 22、hook 14、pipe 14、project_id 3、**propagation 3 (+ 計測 2 は ignored)**、reconcile 10、review 15、revision 13、scene_diff 13、session 41、storage 18、watch 24)。`cargo test --manifest-path tools\mcp\Cargo.toml` → 58 ok (`tools\mcp\src` は変えていない)。
  - `tools\build_track.bat Release` (mye_track.exe が動いていないことを先に確認) → `tools\track_verify.bat` → 全シナリオ PASS (最初の実行で `scene_diff` / `scene_overlay` の NDJSON の形が変わって 3 件 FAIL → 伝播の項目だけに新しい項目を付けるよう直して再実行)。`tools\mcp_verify.bat` は `tools\mcp\src` を変えていないので省略。
  - `pwsh -NoProfile -File tools\check_rules.ps1` → 0 error (警告 66、新規の警告は規則 7 の既存の型の走査のみ)。`tools\check_rules_selftest.ps1` → 全 PASS。
  - `tools\replay_verify.bat` → **既定の並列度 (7 本) では 2 回とも、先頭のジョブ `demo` だけが 0.4 秒で FAIL (「The system cannot find the batch label specified - chain」= cmd のバッチ読取りの競合。ハッシュの照合の前で、他の 23 ジョブは PASS)**。`demo` を単独で回すと (`chcp 437 >nul & call tools\replay_verify.bat --job demo`) 終了コード 0 で、Debug の stress verify・Release・Server.exe の 3 つとも 600 ticks hash-identical。`MYE_REPLAY_JOBS=3` で全体を回し直すと **`[PASS] replay consistency` (24 ジョブ全部、557 秒)**。私の変更は Engine 層に無く (`LocalizationTable.inl` だけ)、`demo` の失敗は照合に入る前なので、原因はこの bat の並列起動の競合と見ている (断定はしていない。以前のサブでは既定の並列度で通っている)。
  - `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` → 空。`git diff --stat ca6a3991 -- src\Engine` → `LocalizationTable.inl` と `HotReload\ReloadHub.*` (`.h` `.cpp` `SelfTest.cpp`) だけ。`git status`: 復元による変更なし、`.mye` なし。
  - 計測 (docs\ai-change-tracking\performance.md §10): Editor 側は `MYE_SELFTEST_ONLY=changetracking` + `MYE_CHANGETRACKING_PERF=1` の Release (3 回)、サービス側は `cargo test --release --test propagation -- --ignored`。取り直し (20000 件) 583〜612 ms、`Poll()` 1 ページ最大 31 ms、`RebuildAncestors` 280〜311 ms (原因は最初の `FindByFileId` の線形走査。切り分けと、直しても合計が変わらないので戻したこと)、Scene View の候補作り 3.0〜4.6 ms/フレーム (20000 件) と 0.08 ms (1000 件)、AABB 8 隅の見積もり +2.5 ms (20000 件)、`scene_overlay` 18 ms (1000 件) / 1.1 秒 (20000 件)、伝播 (100 シーン x 100 インスタンス) の増分約 6 ms、監視の計画づくり 2.8 秒 (30200 ディレクトリ、監視 1 本)。
  - スクショ (Release、作業ディレクトリは `%TEMP%\mye_m104t_run`。一時プローブ = 偽のサービスの返事を `EditorApp` に流す。**コミットしない・外した**): `cache\m104t_shots\` の 1_propagation_ja / 2_cause_ja / 3_propagation_en / 4_inspector_propagated_ja / 5_inspector_mixed_ja / 6_inspector_propagated_en (.png)。Hierarchy の輪の印 (伝播だけ) と塗りの丸 (直接の編集・混在)、Inspector の見出しとフィールドの輪、一覧の「プレハブの変更の波及」の行と要約の行と「原因の変更へ移動」(2 枚目は原因の行が選ばれた状態)、英語。**見た目の合否はユーザーの目視**。2 と 4 は開いて見た (文字は切れていない)。1・3・5・6 は撮ったが、最終の版の画像は開いて見ていない。
  - **リポジトリ直下の `imgui.ini` の更新時刻が 00:37:28 から 04:49:15 に変わった。** 原因は、必須の横断の `tools\replay_verify.bat` が Editor をリポジトリ直下で起動すること (私の窓を出す起動 = スクショと `--frames` は、すべて作業ディレクトリを `%TEMP%\mye_m104t_run` にした。`--selftest-full` の後、replay_verify の前に確認したときは 00:37:28 のままだった)。大きさは前後とも 3527 バイトで、先頭の内容も標準の配置だが、控えが無いので同じ内容かは確かめられない。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 7 件のうち、A・B (サービス + Editor)・C (計測と判断と目標値)・D (対応表・ADR・計画・engine_spec・checklists)・E を行った。逸脱 2 件 (入力は記録のシーン、伝播の行の `revision_id` はプレハブ側) と [未実装] (引けなかった件数、1 フレーム合計) を書いた。
  正しさ: 4 — 結合試験で「override していないものだけ・原因・DB の作り直し・旧形式・削除済み」を確かめ、Debug / Release の全スイート、cargo、track_verify、規則が通った。実プレハブ + 実サービス + 実 Editor での通しは未確認 (スクショは偽の返事)。
  コード品質: 4 — 純関数の `propagation.rs` に限り、入れ子の SAVEPOINT で伝播の失敗が記録を止めない。既存の応答の形は変えない。Editor 側は表示だけで World に触れない。`FindByFileId` の線形走査は直していない (測って、直しても合計が変わらなかったため)。
  テスト: 4 — Rust 結合 3 件 + Editor の `TestPropagation` + 復元の判定 2 件。上限 (300 シーンなど) を超えたときの動作の試験は無い。
不安・質問:
  - 伝播の行を原因の Revision (プレハブ側) に付ける設計 (逸脱 2) で良いか。スキーマの `change_revisions.caused_by` が想定していた「シーン側の伝播 Revision」(一覧にシーンの行として出る) にするなら、`changes` / `change_revisions` の列挙 (list、notify、review、restore) の全部が影響を受ける。今の形は、確認と一覧の対象を増やさないために選んだ。
  - spec.md の D38 / D43 の結論を coder が書き足した (サブ仕様の指示。原則は planner が書く)。planner が書き直すなら差し替えてよい。sub-14 / sub-16 / sub-18 の「Debug は physics を除く」は残してある。
  - `FindByFileId` の最初の線形走査 (2 万個で 約 0.3 秒、1000 件の変更で約 15 ms。`Scene::fileIdCache_` が採番点でしか温まらない) は Engine 層の性質で、Editor だけでは直らない。別のサブにするか。
  - 本物の Claude Code でプレハブを編集させる通しは未実施。
  - `tools\replay_verify.bat` の既定の並列度で `demo` に起きる競合 (上記) は、私の変更とは別と見ているが、確かめるなら `MYE_REPLAY_JOBS` を変えて回すこと。また、この bat はリポジトリ直下の `imgui.ini` を書き換える (受け入れ条件 E の「imgui.ini の更新時刻が同じ」とは両立しない)。どちらを優先するか。
  - 伝播の行を選ぶと、レビュー (承認・却下) のボタンが今までどおり出る (確認の対象ではないのに、押すとサービスは受ける。葉の列挙からは除いてあるので集計には影響しない)。非表示にするか。
触ったファイル:
  docs/adr/ADR-039-ai-change-tracking.md docs/ai-change-tracking/acceptance.md docs/ai-change-tracking/architecture.md docs/ai-change-tracking/db-schema.md docs/ai-change-tracking/ipc-contract.md docs/ai-change-tracking/performance.md docs/ai-change-tracking/restore-safety.md docs/test_checklists.md engine_spec.md plans/m104-ai-change-tracking.md plans/m104-ai-change-tracking/spec.md plans/m104-ai-change-tracking/sub-19.md (実装メモ) src/Editor/App/EditorMain.cpp src/Editor/ChangeTracking/ChangeFormat.cpp src/Editor/ChangeTracking/ChangeModel.cpp src/Editor/ChangeTracking/ChangeModel.h src/Editor/ChangeTracking/SceneChangeOverlay.cpp src/Editor/ChangeTracking/SceneChangeOverlay.h src/Editor/ChangeTracking/SceneChangeOverlaySelfTest.cpp src/Editor/ChangeTracking/SceneRestoreFormat.cpp src/Editor/ChangeTracking/SceneRestorePlan.cpp src/Editor/ChangeTracking/SceneRestorePlan.h src/Editor/ChangeTracking/SceneRestoreSelfTest.cpp src/Editor/Windows/Project/AiChangesWindow.cpp src/Editor/Windows/Project/AiChangesWindow.h src/Editor/Windows/Scene/HierarchyWindow.cpp src/Editor/Windows/Scene/InspectorChangeMarks.cpp src/Engine/Core/Localization/LocalizationTable.inl tools/track/src/lib.rs tools/track/src/notify.rs tools/track/src/observe.rs tools/track/src/ops.rs tools/track/src/propagation.rs (新規) tools/track/src/review.rs tools/track/src/revision.rs tools/track/src/scene_diff.rs tools/track/src/scene_overlay.rs tools/track/src/startup.rs tools/track/src/store.rs tools/track/tests/propagation.rs (新規)
消した・止めたもの:
  - 消した (すべて私が作った物。`%TEMP%` 直下): `mye_m104t_a` (editbin 用の Debug の exe の複製。リポジトリの外)、`mye_m104t_run` (スクショ用の作業ディレクトリ。中は `imgui.ini` だけ)、`mye_m104t_dirs` (監視の計画づくりの計測用に作った 30200 個のディレクトリ)。リポジトリのファイルは 1 つも消していない。
  - 一時のプローブ (偽のサービスの返事を `EditorApp` に流すコード、`AiChangesWindow.cpp` の窓の大きさとスクロールの指定、`EditorMain.cpp` の `ShowSize` の置き場所) は、撮影・測定のあとに外した (`EditorApp.cpp` は `git checkout`、他は印の行を削除)。ソースにプローブの語 (`M104T_PROBE`、`m104tprobe`) は残っていない。
  - 止めたプロセス: 無し (自分が起動した `Editor.exe` / `mye_track.exe` は自分で終了するまで待った。`mye_track.exe` は検証の前後とも動いていない。`mye_mcp.exe` と `tools\mcp\target\release\` には触れていない)。`tools\build_track.bat Release` で `bin\x64\Debug|Release\mye_track.exe` を新しいものに置き換えた (動いていないことを確かめてから)。
申し送り:
  - sub-20 (配布): `tools\build_track.bat` が `bin\x64\*` へ置く `mye_track.exe` は今のソースのもの。`tools\track\target\release\mye_track.exe` も同じ。
  - 私が作って後で消した物は「消した・止めたもの」に書いた。残してある物 (司会が消してよい): `%TEMP%\mye_m104t_*.txt` (計測の出力)、`cache\m104t_shots\` (スクショ。コミットしない)、`cache\m104t_*.out / .err / .log` (自己テストと replay_verify の全文ログ)。

```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-16)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、基点からの `src\Engine` の差分は `LocalizationTable.inl` と `HotReload\ReloadHub.*` の 4 ファイルだけ、ABI まわり・スキーマ・`tools\mcp`・`.claude`・`.mcp.json` に差分なし、プローブの語の残りなし、`EditorMain.cpp` 585〜586 行目で `EditorApp` が `unique_ptr` (`appHolder`) と参照 `app` になった、Debug / Release の `Editor.exe` は最後のソースの変更より新しい。**planner の実走: `MYE_SELFTEST_ONLY=physics,behaviortree,changetracking` の Debug が 3 スイート 0 失敗 (終了コード 0、153 秒)。sub-14 から「physics を除く」としてきた Debug の physics が通った = D38 が解消した。** `cargo test` (track)、`track_verify`、`check_rules` は planner も回した (結果は VERDICT)。Release / Debug の `--selftest-full` (どちらも 125 スイート 0 失敗)、`replay_verify` (並列度 3 で PASS)、計測、スクショは coder の結果による。
  - 逸脱の判定: 伝播の入力を「今のディスク」ではなく「各シーンの最新の記録 (Blob)」にした (作り直しで同じ行になるため)、伝播の行をプレハブ側の Revision に付けた (シーン側に偽の Revision を作らず、レビューと一覧の対象を増やさない)、は採る。`PrefabInstanceChanged` のコンポーネント名の不具合 (`PrefabInstanceComponent` → 登録名 `PrefabInstance`) を見つけて直したのはよい。
  - 未実装の判定: 「引けなかった」アセットの件数 (任意)、可視の判定を原点のままにした (計測の根拠つき)、1 フレームの合計の増分 (個別に測った) は可。
  - coder が spec.md の D38 / D43 の結論の欄を書き足した件: サブ仕様 (A の 2) が指示したことで、planner が内容を確かめて受け入れる (上の実走と一致)。sub-14 / sub-16 / sub-18 の「physics を除く」は過去のサブの記録としてそのまま残す。
  - 質問への回答は VERDICT に書いた (伝播の行の付け方は採る / `FindByFileId` の最初の線形走査は Engine 層の性質で M104 の外 / `replay_verify` の並列の競合と `imgui.ini` の書き換えは bat の既存の性質で M104 の外 / 伝播の行のレビューのボタンは reviewer の指摘に回す)。