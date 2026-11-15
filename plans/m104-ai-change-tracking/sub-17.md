# sub-17: 削除されたエンティティのゴーストと、Before の読み取り専用の表示 (M104r)

- 依存: sub-16
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-16 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。

## 着手前の確認の扱い (推奨で裁定済み。spec §2 D12、§6.2)

- 「Phase 6 (P2) へ進むか」→ **進む** (2026-11-15 の裁定。進捗表どおり sub-17 → sub-18 → sub-19 → sub-20)。逆の答えが来たら、sub-17〜19 を行わず、sub-20 (配布) の依存を sub-16 に付け替える。A17〜A19 が落ちる。

要求 §8.2、§8.4 の拡張段階、§16.3 の 3 番目 (Entity 削除時に Before を表示できる)。arch §7。**World にエンティティを作らない。表示のために World を書き換えない。**

## 使えるもの

- 削除されたエンティティの Before: `scene_diff` の `EntityRemoved` の行の `before_value` (エンティティの JSON の断片。字句のまま。`TrackSceneChange::beforeValue`)。`scene_overlay` (sub-12) の `entities[].status = removed`。
- ゴーストの描画: `GhostMeshPass::Add(const Mesh* mesh, const XMFLOAT4X4& world, uint32_t rgba)` (`C:\HAL\MyEngin\src\Engine\Renderer\Passes\GhostMeshPass.h` 33 行目)。前例は `C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewGhost.cpp` (分岐ゴースト、M72e / M72i。半透明 40%、メッシュが引けない物はワイヤ箱、SceneView の RT にだけ描く、読むだけ)。
- 強調の色: sub-24 の `OverlayPalette` の「削除」の色。表示の入・切: `SceneChangeOverlay::Show()`。
- Scene View の箱の候補作り: sub-24 の `SceneChangeWireBoxes` (純粋な関数。可視・上限 256・近い順)。ゴーストはライブに居ないので、この候補には入らない (別の経路。sub-24 の申し送り)。

## やること

### A. Before から描くための情報を取り出す (`src\Editor\ChangeTracking\RemovedEntityGhosts.h/.cpp`、新規。純粋な部分と、アセットを引く部分を分ける)

1. `EntityRemoved` の `before_value` (JSON の断片) から、**World を通さずに**次を読む: 名前、`fileId`、親の `fileId`、`LocalTransform` (位置・回転・拡大)、`MeshRendererComponent` のメッシュの参照 (アセットの guid / パス)。字句を保つ読み方は不要 (表示だけなので `nlohmann::json` で読んでよい)。読めない・コンポーネントが無いものは「位置だけ」「描けない」として扱う。
2. **ワールド行列**: 削除されたエンティティの親の連鎖を、(a) 同じ変更で一緒に消えた親 (同じ Revision の `EntityRemoved`) の Before の `LocalTransform`、(b) ライブのシーンに今いる親の今のワールド行列 (`FileIdComponent` を読むだけで引く。採番しない)、の順でたどって合成する。親が見つからなければ、ルートとして扱い、その旨を注記にする (「親の位置が分からないので、ローカルの位置で描いています」)。
   - **一時の `Scene` / World にエンティティを作って変換を計算する方法は採らない** (World を書かないことの確かめが難しくなり、ECS の登録・アーキタイプの生成が走る)。行列の合成は、既存の変換の式 (`LocalTransform` から行列を作る関数) を読むだけで使う。合成の結果が、同じエンティティがライブにいたときの `WorldTransform` と一致することを試験で確かめる (エンティティを消す前に記録しておき、消した後に Before から計算した行列と比べる)。
3. メッシュ: 参照から、今読み込まれているメッシュを引く (読むだけ。新しく読み込まない。読み込まれていなければワイヤ箱か印)。**アセットの今の内容で描く** (削除の時点のメッシュとは限らない)。参照先のメッシュが Before の後に変わっていれば (追跡の履歴で分かる範囲。分からなければ分からないと出す)、注記に「メッシュは今の内容で描いています」。
4. スキンメッシュ・パーティクル・ライト・UI など、メッシュでない描画のコンポーネントは、ワイヤ箱か位置の印にとどめる。

### B. Scene View への描画 (`SceneViewWindow` の既存の経路から)

- 描くのは SceneView の RT だけ (GameView、撮影の経路、Runtime には出さない。`SceneViewGhost.cpp` と同じ)。
- 半透明のメッシュ (`GhostMeshPass::Add`)。色は `OverlayPalette` の「削除」に、不透明度を下げたもの。メッシュが引けないものはワイヤ箱 (`EditorLinePass`)。
- 可視のものを優先し、1 フレームに描く数に上限 (sub-24 と同じ形。超えたら帯に注記)。
- 強調の表示が切 (`Show()` が偽)、追跡が無効・未接続のときは 1 つも描かない。
- 生の D3D 型を `ChangeTracking\` に持ち込まない (規則 16)。`GhostMeshPass` を呼ぶのは `SceneViewWindow` / `SceneViewGhost` 側 (既に規則 16 の許可リストにある経路) にして、`ChangeTracking\` からは「何を、どの行列で、どの色で」の一覧だけを渡す。許可リストへの追加が要るなら、理由を書き `engine_spec.md` §6.3 も更新する (AGENTS.md §4.1)。

### C. 読み取り専用の Before の表示

- ゴーストをクリック (Scene View のピック) するか、AI Changes 窓の「シーンの変更」の一覧で `EntityRemoved` の行を選ぶと、Before の内容 (名前、親、コンポーネントとフィールドの値。字句のまま) を**読み取り専用**で見られる。`Selection` には入れない (実体が無い)。表示は AI Changes 窓の詳細の列か、小さい別の窓。編集の操作 (値の変更、コンポーネントの追加) は出さない。
- 「このエンティティを復元…」の導線 (sub-16 のシーンの復元の、Entity の削除の取り消し) へつなぐ。
- Scene View のピックは、既存のピックの経路を変えずに、ライブのエンティティに当たらなかったときだけゴーストを調べる (ライブの選択の挙動を変えない)。難しければ、ピックは入れず一覧からの選択だけにして理由を書く。

### D. Scene Overlay の入・切 (要求 §8.4 の拡張段階)

- 既存の強調の入・切 (sub-24 で保存される) に、ゴーストの表示の入・切を足す (帯のチェック。保存する)。未確認 (`unknown` / `inferred`) の変更のアイコン (要求 §8.1「未確認 = アイコン」) を Scene View の強調に出す: sub-24 のワイヤ箱の色分けに、帰属が未確認のものの印を足す (色ではなく小さい印。Hierarchy のバッジにも同じ印)。

### E. 文書

- arch §7、`C:\HAL\MyEngin\engine_spec.md` §9.2 (と、規則 16 の許可リストを変えたなら §6.3)、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表)。

## やらないこと (このサブでは)

- Before / After の独立の表示、左右の 3D 比較 (sub-18)。プレハブ伝播 (sub-19)。
- World・ECS への一時のエンティティの生成。メッシュの新しい読み込み。過去のアセットの内容の復元。
- Engine 層の変更 (`GhostMeshPass`、`EditorLinePass` に手を入れない。足りなければ「不安・質問」で返す)。変わってよいのは `LocalizationTable.inl` だけ。

## 受け入れ条件 (このサブ)

1. ゴーストを出した状態と出していない状態で、本番の Scene の `HashWorld`、エンティティ数、アーキタイプ数が同じ。`FileIdComponent` が付かない。メッシュの読み込みが新しく起きない → `changetracking` スイート。
2. Before から計算したワールド行列が、消す前にライブで記録した `WorldTransform` と一致する (親が同じ変更で消えた場合、親がライブにいる場合、親が見つからない場合 = 注記)。誤差は既存の変換の試験と同じ許容 → スイート (純粋な関数)。
3. 削除されたエンティティの Before の内容 (名前、コンポーネント、フィールドの値) が、読み取り専用で表示される。編集の操作が出ない・効かない。`Selection` が変わらない → スイート (ヘッドレスの ImGui フレーム) (要求 §16.3 の 3 番目)。
4. Before の行が読めない / Blob が欠けている / メッシュの参照が引けない、のそれぞれで、ゴーストの代わりにワイヤ箱か「表示できない」が出て、落ちない → スイート。
5. 描く数の上限、可視の優先、強調が切・追跡が無効のとき 0 → スイート (`ChangeTracking\` が渡す一覧の数を数える口)。
6. 未確認の変更の印が、Scene View の強調と Hierarchy のバッジに出る。確定 (`verified` / `claimed`) の変更には出ない → スイート。
7. Play 中に表示を切り替えても sim の結果が変わらない → スイート。
8. スクショ (一時のプローブ): 削除されたエンティティのゴースト (メッシュ・ワイヤ箱)、Before の読み取り専用の表示、未確認の印、英語。**見た目の合否はユーザーの目視。**
9. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1` (規則 10、16、19)、`cargo test` (track)、`tools\track_verify.bat` が通る。下位の層の差分が `LocalizationTable.inl` だけ。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `--selftest` を Debug / Release
- `pwsh -NoProfile -File tools\check_rules.ps1`
- スクショ: 一時のプローブ + `bin\x64\Release\Editor.exe --frames 40 --shot-frame 36 --screenshot`。プローブはコミットしない
- exe の実行は PowerShell から。試験は一時プロジェクトか偽のデータで行う。`tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。Windows のパスを含むコードは Write / Edit ツールで書く (Bash のヒアドキュメントはバックスラッシュを潰す)。

## 実装メモ (coder が追記)

### SELF_EVAL: sub-17 (round 1)

実装:
- tools\track\src\scene_overlay.rs: `scene_overlay` の各エンティティに `uncertain` (寄与した行の Revision の帰属が unknown / inferred) と、削除されたエンティティの `before` (消える直前の JSON の断片。64 KiB 超は `before: null` + `beforeOversize` + `beforeBytes`) を足した (加えただけ。スキーマは版 3 のまま)。単体試験 4 件、tests\pipe.rs (実サービス: 外部の編集は uncertain、追加は before なし)、tests\review.rs (unknown / inferred = 真、verified / claimed = 偽、別文書の帰属は混ざらない)。tests\track\26 / 29 の期待 NDJSON は、足した項目だけが違うことを確かめて撮り直した。
- src\Editor\ChangeTracking\RemovedEntityGhosts.h/.cpp (新規): Before の読み (`ParseRemovedBefore`)、親の連鎖からのワールド行列の合成 (`ComposeGhostWorld`。同じ表の削除済み → ライブの今の行列 → 見つからなければルート + 注記。輪・段数上限で打ち切り)、`CollectRemovedGhostCandidates` (World は読むだけ)、`SelectRemovedGhosts` (見えているもの・近い順・64 件)、`HitTestRemovedGhosts`、読み取り専用の表示の中身 `BuildRemovedBeforeView`。GPU の型は持たない。
- SceneChangeWireBoxes.h/.cpp: 画面内の判定を `ProjectChangePoint` に切り出してゴーストと共有。`ChangeBoxCandidate::uncertain` を追加。
- SceneChangeOverlay.h/.cpp: `OverlayEntity` に uncertain / before / beforeOversize、`OverlayBadge::uncertain`、ゴーストの入・切 (`ShowGhosts`、保存の受け口)、`Tracking()`、ゴーストの数と注記 (`NoteGhosts`)、Before を見る要求 (`RequestInspectRemoved` / `TakeInspectRequest`)。
- SceneViewWindow.h/.cpp: `AppendRemovedGhosts` (`GhostMeshPass::Add` か `EditorLinePass`。メッシュは `meshes.GetDrawable` で今読み込まれているものを引くだけ)、`DrawChangeMarks` (未確認の「?」とゴーストの名前)、ゴーストのピック (ライブのエンティティに当たらなかったときだけ。Selection は既存どおり)。
- HierarchyWindow.h/.cpp: 未確認の小さい印。ChangeFormat.cpp: バッジのツールチップに未確認の説明。
- AiChangesWindow.h/.cpp: 帯にゴーストのチェックと「上限」「表示できない」の注記。Before の読み取り専用の表示 (`DrawRemovedBefore`。入力のウィジェットなし)、一覧の EntityRemoved の行と Scene View のゴーストのクリックから開く (後者は窓も開く)。「このエンティティを復元...」(`RequestRestoreRemoved` → `TickRestoreRequest`: 削除した変更を選び、describe と scene_diff が揃ったらその行で sub-16 の `SceneRestoreDialog` を Entity の範囲で開く)。
- EditorSettings.h/.cpp、EditorApp.cpp: `aiChangeShowGhosts` の保存・読み込み。
- LocalizationTable.inl (末尾に 20 件。日英): 下位の層の差分はこれだけ。
- 試験 (SceneChangeOverlaySelfTest.cpp、`changetracking` スイートに 90 件前後): Before の読み / 合成行列がライブの `WorldMatrix` と一致 (親も消えた・親がライブ・親が見つからない・輪・深すぎ) / 読めない・欠けている・大きすぎる・メッシュ不明 / 上限・可視・切・無効・切断で 0 / 復元後 Refresh でゴーストが消える / ピック / Before の表示と復元への導線 / 未確認の印 / 設定の保存 / Play 中の切り替え。
- docs: architecture.md §7、engine_spec.md §9.2、test_checklists.md (M104r の節)、ipc-contract.md (scene_overlay)、plans\m104-ai-change-tracking.md (進捗表)。

仕様との差分:
- [追加] サービス (tools\track) の `scene_overlay` に `uncertain` / `before` / `beforeOversize` / `beforeBytes` を足した。理由: 強調の表はエンティティごとに畳んだものしか持たず、削除されたエンティティの Before も帰属も Editor に届かない。scene_diff を Revision ごとに取り直して Editor で畳み直す案は、畳み方を 2 か所に置く (sub-12 の設計に反する) ので採らなかった。加えただけで既存の項目は変えない。sub-17 の「下位の層の差分は LocalizationTable.inl だけ」は Engine 層の話として読み、tools\track は Engine 層ではないと解釈した。違う解釈なら planner が判断してほしい。
- [追加] tests\track\26_scene_overlay.expected.ndjson と 29_scene_restore.expected.ndjson の撮り直し (新しい項目だけが違う。差分は目視と機械で確認)。
- [逸脱] ゴーストの上限は 64 (ワイヤ箱の 256 より低い)。半透明メッシュは線より重いため。定数 `kMaxRemovedGhosts`。
- [逸脱] 「メッシュが Before の後に変わったか」は分からない (追跡の履歴では判定しない) ので、メッシュで描いたゴーストには常に「今の内容で描いています (削除の時点と違うかもしれません)」を出す。分からないと出す、の読み。
- [追加] ゴーストの表示・Before の「入口」は Scene View のクリック (ライブの物に当たらなかったときだけ。Ctrl なしなら従来どおり選択は解除) と一覧の行。ピックは「ゴーストの原点が画面上で 18 px 以内」で、メッシュの輪郭では当たらない。
- [未実装] 一覧の EntityRemoved の行をクリックしたとき、既存の `ActivateSceneChange` は「ライブのシーンにいないので選べません」の注記を出したままにしている (Before の表示は同時に出る)。

検証:
- cargo test --manifest-path tools\track\Cargo.toml → 全部 ok (scene_overlay の新規 4 + pipe 1 追加アサート + review 1 新規を含む)
- tools\build_track.bat Debug → mye_track.exe を bin\x64\Debug / Release へ (動いている mye_track は無いことを確認してから)。tools\track_verify.bat → 初回は 26 / 29 が新しい項目のぶん FAIL → 差分が足した項目だけと確認して撮り直し → all scenarios passed
- MSBuild MyEngine.sln Debug|x64 と Release|x64 (/p:MyeWarnAsError=true) → どちらも 0 error / 0 warning (最終版のソースで)
- MYE_SELFTEST_ONLY=changetracking Debug → PASS (約 29 s)、Release → PASS (約 6 s)。新規 82 + 8 件の PASS。
- Debug の fast / full 全部 (physics を除く 124 スイート) → 0 failed (934 s)。※この実行はスクショ用の一時プローブと帯の 1 行の整理の前のソースで、その後は changetracking だけ Debug / Release で回した。
- Release `--selftest-full` → 125 スイート 0 failed (331 s、最終版のソース)
- pwsh tools\check_rules.ps1 → 0 error (規則 10 / 16 / 19 に新規の指摘なし。warning 66 件は既存)
- スクショ (一時のプローブ + Release の Editor.exe --scene assets\scenes\flow_game.scene.json --frames 40 --shot-frame 36 --screenshot。プローブは外して再ビルドし、grep で残っていないことを確認): C:\HAL\MyEngin\cache\m104r_shots\ の 1_scene_ja / 3_scene_en (Scene View のゴースト = 半透明メッシュ・ワイヤ箱 2 つ・位置の球、名前、未確認の「?」、Hierarchy の Floor の「?」)、2_before_ja / 4_before_en (AI Changes 窓の Before の読み取り専用の表示と「このエンティティを復元...」)。撮れた・文字が切れていない・化けていない、まで確認。**見た目の合否は目視 (未確認)**。プローブは自動実行の起動ではサービスへ接続しないため、強調の表を直接入れた (実サービスからの経路ではない)。
- 未実行: 実 Editor + 実サービスでの手操作 (外部で削除 → ゴースト → クリック → 復元)。replay_verify (Editor 層だけの変更で sim・ABI・SimSnapshot は触っていない)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜9 を試験・スクショ・コマンドで確認した。サービスへの追加 (差分欄) と、手操作の未実施が減点
  正しさ: 4 — 合成行列はライブの WorldMatrix と一致する (3 つの親の状況 + 輪)。World を作らない・変えないことは HashWorld / エンティティ数 / アーキタイプ数で確認。ピックは原点の近さだけ、可視判定も原点だけ (sub-19 の nit のまま)
  コード品質: 4 — 純粋な部分 (RemovedEntityGhosts) と描画 (SceneViewWindow) を分け、画面内の判定は共有。AiChangesWindow がまた大きくなった
  テスト: 4 — 純粋な関数・ヘッドレスの ImGui 窓・設定の保存・実サービス (Rust) を確認。メッシュを新しく読み込まないことは「引く口が `MeshAvailable` の 1 つだけ」という構造と問い合わせ回数で確かめていて、実 GPU のメッシュライブラリでは確かめていない

不安・質問:
- 差分欄の最初 (サービスの `scene_overlay` への追加) を planner が認めるか。認めない場合の代替は「Before を scene_diff から Revision ごとに取り、帰属を list の行から引く」で、Editor 側に畳み方が漏れる。
- 未確認の印の対象は `scene_overlay` に寄与した行のどれかが unknown / inferred のエンティティ。「最後の変更だけで決める」案もあるが、1 つでも未確認が混ざっていれば確認を促す方を採った。
- ゴーストのピックは原点の近さだけ。大きいメッシュの端をクリックしても当たらない。sub-19 で可視判定を見直すとき一緒に直すか。

触ったファイル: (生成物を除く)
  tools\track\src\scene_overlay.rs、tools\track\tests\pipe.rs、tools\track\tests\review.rs、tests\track\26_scene_overlay.expected.ndjson、tests\track\29_scene_restore.expected.ndjson、
  src\Editor\ChangeTracking\RemovedEntityGhosts.h (新規)、RemovedEntityGhosts.cpp (新規)、SceneChangeOverlay.h / .cpp、SceneChangeWireBoxes.h / .cpp、ChangeFormat.cpp、SceneChangeOverlaySelfTest.cpp、
  src\Editor\Windows\Project\AiChangesWindow.h / .cpp、src\Editor\Windows\Scene\SceneViewWindow.h / .cpp、HierarchyWindow.h / .cpp、
  src\Editor\App\EditorApp.cpp、EditorSettings.h / .cpp、src\Engine\Core\Localization\LocalizationTable.inl、
  docs\ai-change-tracking\architecture.md、docs\ai-change-tracking\ipc-contract.md、docs\test_checklists.md、engine_spec.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-17.md (実装メモ)
  生成物 (tools\gen_project_files.ps1 が更新。tracked): build\Editor.vcxproj、build\Editor.vcxproj.filters
申し送り:
- sub-19: 可視判定は原点だけ (ワイヤ箱もゴーストも `ProjectChangePoint` 1 か所)。ゴーストのピックも原点の近さだけなので、同じ所で見直せる。
- sub-18: Before の読み取り専用の表示 (`RemovedBeforeView` / `AiChangesWindow::DrawRemovedBefore`) は、削除されたエンティティだけ。Before / After の独立の表示と左右の比較はこれの外。ゴーストのメッシュは今のアセットで描く (削除の時点のメッシュではない) ので、「過去のメッシュ」を出す sub-18 の方針と食い違わないか確認してほしい。
- 自分の作業の副作用: 最初のスクショの 2 回はリポジトリ直下を作業ディレクトリにして Editor を起動したため、git 管理外の imgui.ini (レイアウト) が更新された (以降は %TEMP%\m104r_wd を作業ディレクトリにした)。tools\build_track.bat が bin\x64\Debug / Release の mye_track.exe を新しい版に置き換えた (動いていないことを確認済み)。
- 一時物 (消していない): cache\m104r_shots\ の png / log / err、%TEMP%\m104r_* (ビルド・試験のログ)、%TEMP%\m104r_wd\ (スクショ用の作業ディレクトリ。自分で作った)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-16)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、下位の層の差分は `LocalizationTable.inl` (23 行) だけ、スキーマ・`tools\mcp`・`.claude`・`.mcp.json` に差分なし、プローブの残りなし、`src\Editor\ChangeTracking\` に `GhostMeshPass` / D3D の型の参照は 0 件 (規則 16 を守っている)、`RemovedEntityGhosts.cpp` に World を書く呼び出し (エンティティの生成、コンポーネントの追加、採番、`ApplyPartial`、読み込み、破棄) は 0 件、Debug / Release の `Editor.exe` は最後のソースの変更より新しい。planner の実走: Debug の `MYE_SELFTEST_ONLY=changetracking` が終了コード 0・FAIL 0、Release の `--selftest` 全体が 112 スイート 0 失敗 (作業ディレクトリがリポジトリのとき。`%TEMP%` だと 3 スイートが落ちる = 既存の前提で M104 と無関係)、`cargo test` (track) 290 件通過、`tools\track_verify.bat` 全シナリオ PASS、`tools\check_rules.ps1` 0 error。スクショ 4 枚、Debug の physics を除いた全スイート、Release の `--selftest-full` は coder の結果による。
  - 追加の判定: サービスの `scene_overlay` に `uncertain` / `before` / `beforeOversize` / `beforeBytes` を足したのは採る。sub-17.md の「下位の層」は Engine など C++ の下位の層のことで、`tools\track` は対象外という coder の読みで正しい。畳み方をサービスの 1 か所に置く sub-12 の設計を守っている。期待値 2 本の撮り直しも採る (差分は足した項目だけ)。
  - 逸脱の判定: ゴーストの上限 64、メッシュで描いたゴーストには常に「今の内容で描いています」を出す、は採る。
  - 質問への回答: (1) サービスへの追加は認める。(2) 未確認の印は「寄与した行のどれかが unknown / inferred」でよい (確認を促す側)。(3) ゴーストのピックが原点だけの件は sub-19 で、可視の判定と一緒に見直す (sub-19.md に書いた)。
  - **作業の副作用**: coder がリポジトリ直下を作業ディレクトリにして Editor を窓つきで起動したため、利用者の `imgui.ini` (`.gitignore` 済み) が 2026-11-16 00:37 に書き換わった。元の内容は残っていない。sub-18 以降の作業の注意に書いた (窓を出す起動は作業ディレクトリをリポジトリの外に)。