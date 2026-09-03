# sub-09: M66i: Content Browser に Git バッジ + 保存直後のヒント

- 依存: sub-02 (sub-03〜08 とは独立)
- 状態: OK (commit: M66i — ハッシュは harness.md のサブ進捗表)
- 往復: 2

## やること

決定 4 / §4.3 / §5 の 14。

1. **`SourceControlState`** に `StateFor(normKey) → std::optional<EntryState>` と `FolderStateFor(normDirKey)` (集約はキャッシュ、`status_changed` で無効化)。
2. **`AssetBrowserWindow`**: グリッド / ツリーの各項目に M / A / D / R / ? / 競合の 1 文字バッジ (フォルダは集約結果)。色は `themeColor::*` の既存トークンを意味で割り当て
   (例: M = Warning、A = Success、D / 競合 = Error、? = AccentSoft、R = Warning)。新トークンを足す場合は `ImGuiTheme.h` の 5 箇条 (彩度 0.35–0.60 / 明度 0.65–0.85、Accent は選択専用) に従う。
   `.meta` の行は本体に束ねられて表示されない (現行の AssetBrowser が `.meta` を隠しているならそのまま)。
3. **保存ヒント**: `EditorApp::SaveCurrentScene`、`AssetOps` の書き出し (import / 作成 / リネーム / 削除)、Animation / Controller / Mixer / ProjectSettings の Save の直後に `CollabClient::HintChanged(paths)`。
   Collab が Unavailable のときは no-op (呼び出し側に分岐を書かせない)。
4. **セルフテスト**: フォルダ集約 (子 M → 親 M / 子 ? のみ → 親 ? / 子 M + ? → 親 M / 子 競合 → 親 競合 / 空フォルダ → なし) — sub-02 の (d) に無いケースを足す。

## やらないこと (このサブでは)

- バッジからの右クリック操作 (stage 等)。v1 は表示のみ。

## 触る場所 (planner の見立て)

- 変更: `src\Editor\SourceControl\SourceControlState.*`、`src\Editor\Windows\AssetBrowserWindow.cpp`、`src\Engine\Renderer\ImGuiTheme.{h,cpp}` (必要なときだけ)、`src\Editor\EditorApp.cpp`、`src\Editor\AssetOps.cpp`、各 Save 窓 (1 行ずつ)、`SourceControlSelfTest.cpp`。

## 受け入れ条件 (このサブ)

1. `--selftest` 緑 (集約 5 ケース)。
2. 実機 (fixture): 変更したテクスチャに M、新規に ?、親フォルダに集約バッジ。シーンを保存した直後にヒントが飛び、**監視のデバウンス (300 ms) を待たずに** status が取り直される (実測は git status の所要時間 = Debug で warm ≒ 300 ms / cold ≒ 400 ms。sub-09 round 1 で読み替えを確定)。
3. `tools\shot_verify.bat` 無変更緑 (AssetBrowser は撮影対象外だが `ImGuiTheme` を触ったときの念のため)。
4. `check_rules.ps1` / `replay_verify.bat` 緑。

## 検証コマンド

```
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Debug\Editor.exe --project cache\fixture_proj   (目視)
tools\shot_verify.bat
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

### SELF_EVAL: sub-09 (round 1)

実装:
- `SourceControlState.{h,cpp}`: `ScmPathKey` (小文字 '/' 区切りのキー。ディスク側 `NormalizePathKey` と同じ towlower を通す) / `SourceControlModel::{fileBadges,folderBadges,StateFor,FolderStateFor}` (BuildModel が entries・nodes と同時に組む = status 受信ごとに必ず作り直る「キャッシュ」。フォルダの値は `PropagateFolderState` の結果を写すだけ) / `SourceControlSession::{BadgeForFile,BadgeForFolder,HintSaved,FlushHints,RelativeKeyOf,rootKey_}`。`HintChanged` は `hintInFlight_` を管理し、`Poll()` が 1 フレーム分の保存ヒントを 1 往復にまとめて送る。
- `EditorWidgets.{h,cpp}`: `ScmBadgeColor` (SourceControlWindow の無名 `BadgeColor` を移設。**色表は 1 箇所**) / `DrawScmTileBadge` (タイル左上に面 + 文字。drawlist 直描きでアイテムを消費しない)。
- `AssetBrowserWindow.{h,cpp}`: `OnImGui` に `const SourceControlSession*` を追加。ツリー (assets ルート / 各フォルダ) はラベル末尾に集約バッジ、グリッドはフォルダタイル・ファイルタイルの左上にバッジ。`DoCreate` を「作った実パスを 1 本受けて `scmhint::Changed`」に整理。
- `ScmHint.{h,cpp}` (新規): 保存ヒントの受け口 1 個 (関数ポインタ + user)。EditorApp が `OnStart` で張り `OnShutdown` で外す。
- 呼び出し側: `EditorApp::{SaveCurrentScene,SaveActorEdit}` は `scm_.HintSaved` を直接。`AssetOps` は choke point 3 箇所 (`RegisterImported` = import/複製、`PerformAssetRelocate` = 移動/リネーム/その undo・redo、`RecycleToBin` = 削除)。`AnimationWindow` (2) / `AnimatorControllerWindow` / `AudioMixerWindow` / `ProjectSettingsWindow` (particle backend / layers / partTags / input actions の 4 保存) は `scmhint::Changed`。
- `SourceControlSelfTest.cpp`: (d2) バッジ引き 13 件 (`{D,?}`→D / `{競合,M}`→競合 / 空フォルダ→なし / `.meta` と `.terrain.edit` の束ね / 大小無視 / 一覧と索引の一致)。プローブに絶対パス→バッジの検査 3 件と `hint_changed` 往復の実測ログを追加。

仕様との差分:
- [逸脱] バッジ色は sub-09 の例 (M=Warning / ?=AccentSoft / R=Warning) ではなく **sub-02 が出荷済みの表** (M=Accent / A=Success / R=Prefab / D=Warning / 競合=Error / ?=TextDisabled) を再利用。理由: 一覧と Content Browser で色が違う方が害が大きい。ただし `Modified=Accent` は `ImGuiTheme.h` 配色ルール 3 (Accent は選択専用) と衝突している (「不安・質問」1)。
- [逸脱] `StateFor` は `std::optional<EntryState>` ではなく `ChangeState` を返す。`EntryState` 型は存在せず、`ChangeState::None` が既に「変更なし」= `CombineState` の単位元。
- [追加] `ScmHint` (グローバルな受け口 1 個) を新設。理由: 保存の実体が `AssetOps` の自由関数と 4 窓に散っており、全部に `SourceControlSession&` を通すと無関係な 10 以上のシグネチャが変わる。読み手が 1 つ (Content Browser) のバッジ側は明示的にポインタを渡す。
- [追加] 保存ヒントの合流 (set に貯めて Poll で 1 往復)。台帳の申し送り「飛んでいる要求に相乗り」の実装。
- [追加] 削除 (`RecycleToBin`) / 移動元パスもヒントに含める。
- [未実装] バッジからの右クリック操作 (sub の「やらないこと」どおり)。

検証:
- `bin\x64\Debug\Editor.exe --selftest` → PASS (exit 0、Source control 自己テスト緑。(d2) 13 件すべて PASS)
- `bin\x64\Release\Editor.exe --selftest` → PASS (exit 0)
- `MYE_COLLAB_PROBE=cache\m66i_probe Editor.exe --selftest` (Debug / Release) → PASS。`hint_changed` 往復の実測 = **415 ms (cold) / 297 ms (warm)**。バッジ引きは絶対パスから Untracked を返し、リポジトリ外と無変更は None
- `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
- 実機 (fixture `cache\m66i_fx`、M/?/D を仕込み): `cache\m66i_ui6.png` `cache\m66i_ui7.png` (+ 拡大 `m66i_zoom_tree.png` `m66i_zoom_grid.png` `m66i_zoom_grid2.png`)。ツリー = `assets D` / `materials D` / `scenes` (無印) / `textures M`、グリッド = 変更テクスチャに `M`、未追跡テクスチャに `?`、削除を含むフォルダタイルに `D`
- `tools\replay_verify.bat` → PASS (10 ジョブ、最終コード状態で再実行)
- `tools\shot_verify.bat` → PASS (19 枚。acoustic 2 枚も 1 回で緑、再実行不要)
- MSBuild `/p:MyeWarnAsError=true` で Editor を Rebuild → 警告 0
- 未実行: `cargo test` / `collab_verify.bat` (Rust 側の差分ゼロ、op も増やしていない)

自己採点 (1-5):
- 仕様適合: 4 — §4.1 の対の規則・フォルダ集約 (最重) を再実装せず引くだけにし、§4.3 の色は既存トークンのみ。バッジ色の割り当てだけ sub の例と違う (差分に明記)
- 正しさ: 4 — 純関数 13 件 + 実 DLL プローブ + 実機スクショで確認。未検証はマウス実操作と、大量ファイル (数百タイル) 時のバッジ引きコスト
- コード品質: 4 — 集約・束ね・色表をいずれも 1 箇所に寄せた。`ScmHint` のグローバル 1 個が唯一の妥協 (理由をヘッダに明記)
- テスト: 4 — 集約 5 ケース + planner 追記 2 ケースを含む 13 件。ヒントの合流そのものは DLL 必須のため headless では検査できずプローブ止まり

不安・質問:
1. `Modified = themeColor::Accent` (sub-02 由来) は `ImGuiTheme.h` の配色ルール 3「Accent は選択・フォーカス・トグル ON 専用、状態の意味色と混用しない」に反している。加えて Content Browser のツリーでは**選択行 (Accent の面) の上に Accent の M** が載って読みにくい (`cache\m66i_zoom_tree.png` の `textures M`)。直すと Source Control 窓の見た目も変わるので、sub-09 では触らず据え置いた。変えるなら planner の判断が要る (候補: M を Prefab (淡青) にして R を別トークンへ、など)。
2. 受け入れ条件 2 の「< 300 ms」は Debug + 実 DLL で warm 297 ms / cold 415 ms。内訳はほぼ `git status` のプロセス起動で、ヒント自体は監視の 300 ms デバウンスを丸ごと省いている。この読み替えで良いか。

触ったファイル:
- `src/Editor/SourceControl/SourceControlState.h` / `.cpp`
- `src/Editor/SourceControl/ScmHint.h` / `.cpp` (新規)
- `src/Editor/SourceControl/SourceControlSelfTest.cpp`
- `src/Editor/EditorWidgets.h` / `.cpp`
- `src/Editor/EditorApp.cpp`
- `src/Editor/AssetOps.cpp`
- `src/Editor/Windows/AssetBrowserWindow.h` / `.cpp`
- `src/Editor/Windows/SourceControlWindow.cpp`
- `src/Editor/Windows/AnimationWindow.cpp` / `AnimatorControllerWindow.cpp` / `AudioMixerWindow.cpp` / `ProjectSettingsWindow.cpp`
- `build/Editor.vcxproj` / `build/Editor.vcxproj.filters` (gen_project_files の生成物)
- `plans/m66-git-collab/sub-09.md` (この節)

申し送り:
- バッジ引きは 1 タイルにつき `NormalizePathKey` + map 検索。数百ファイルのフォルダで ~1 ms/frame の見込み (実測していない)。重くなったらフレーム先頭で 1 回だけ相対化する形へ寄せられる。
- `scmhint::Changed` を足す場所を増やすときは「ファイルが増減・改変された唯一の実体」に置くこと (`AssetOps` の choke point 3 箇所が実例)。公開関数の末尾に散らすと必ず取りこぼす。
- `SoundGenWindow` の .wav 書き出しにはヒントを入れていない (sub の列挙外)。監視で 300 ms 後に出る。
- 実機の撮影は「Content Browser の表示フォルダを変える手段が UI 操作しかない」ため、`AssetBrowserWindow` の `init_` に env で開始フォルダを差し替える一時プローブを入れて撮り、撮影後に削除・再ビルドした。同種の撮影をするサブは同じ手を使える。
- **PNG を壊すと `--screenshot` が黙って撮れなくなる**: fixture の `test.png` に IEND 後のゴミを足して「変更」を作ったら、非同期テクスチャの drain が完了せず screenshot 保存自体が起きなかった (ログにも何も出ない)。変更を作るときは**別の正しい PNG で上書き**する。
- `tests\collab\` の次の番号は **10** のまま (このサブでは op も NDJSON も足していない)。
- 生成物: `cache\m66i_fx` (+`.gitconfig`)、`cache\m66i_probe` (+`.gitconfig`)、`cache\m66i_*.png`、`cache\m66i_*.log`。

### SELF_EVAL: sub-09 (round 2)

実装:
- #1: `EditorWidgets.cpp:ScmBadgeColor` を spec §4.3 の確定表へ置換 — M = `Warning` / A = `Success` / D = `Error` / R = `Prefab` / 競合 = `Error` (D と同色。区別はグリフ `D` と `!`) / ? = `TextDisabled`。`Accent` は表から消えた。表は共有 1 箇所なので Source Control 窓 (`SourceControlWindow.cpp:1077`) と Content Browser が同時に変わる。
- #1: `EditorWidgets.h:ScmBadgeColor` の宣言コメントに確定表と「Accent 系を状態色に使わない」理由 (配色ルール 3 / 選択行の面と同色になると読めない) を明記。
- #1: `SourceControlSelfTest.cpp` に **(d3) バッジの色表**を新設 — (a) 5 状態が確定表どおりか、(b) 全 7 状態について `ScmBadgeColor(state)` が `themeColor::Accent` / `AccentSoft` のどちらとも一致しないこと。`ScmBadgeColor` は ? / None で `ImGui::GetStyleColorVec4` を通る = ヘッドレスでは `GImGui` が無く落ちるため、**バックエンド無しの ImGui context を 1 個だけ作って捨てる** (既に context があるときは触らない)。
- 指摘 2 (バッジ引きのコスト) / 3 (SoundGenWindow) は「今は触らない」の指示どおり未着手。3 は下の申し送りに残す。

仕様との差分:
- round 1 の [逸脱] 「バッジ色は sub-02 の表を再利用」は**解消** (spec §4.3 の確定表に一致)。
- [追加] セルフテストのために一時的な ImGui context を作る (テスト内のみ、他スイートに影響しない)。**バックエンドも NewFrame も無し**。
- その他の差分 (`ChangeState` 戻り値 / `ScmHint` / ヒントの合流 / 削除・移動元のヒント) は planner が採用済みのため round 1 の記載のまま。

検証:
- `cmd /c bind\Debug\Editor.exe --selftest` → exit 0、Source control PASSED (新規 2 件 PASS)
- `cmd /c bind\Release\Editor.exe --selftest` → exit 0、Source control PASSED
- **故障点での検証**: `Modified → themeColor::Accent` をわざと戻して再ビルド → `--selftest` が **exit 1** で 2 件とも FAIL (`cache\m66i_r2_regression.log:3563-3564`)。戻して再ビルド後は緑。テストが空振りでないことを確認済み
- `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
- MSBuild `/p:MyeWarnAsError=true` で Debug / Release とも警告 0
- 実機スクショ (fixture `cache\m66i_fx`、M/?/D 入り):
  - `cache\m66i_r2_ui.png` (全景) / **`cache\m66i_r2_zoom_tree.png`** = 指摘の再現条件そのもの。**選択行 (Accent の面) の `textures` に M (琥珀 = Warning)** が載って読める。`assets D` / `materials D` は赤 (Error)
  - **`cache\m66i_r2_zoom_grid.png`** = グリッドのファイルタイル。サムネイルの上の `M` が琥珀
  - `cache\m66i_r2_ui_root.png` / `cache\m66i_r2_zoom_folder.png` = フォルダタイルの `D` が赤 (金色のフォルダグリフと弁別できる)
- `replay_verify.bat` / `shot_verify.bat` → **未実行** (round 1 で緑。round 2 の差分は Editor 層の色表とセルフテストのみで Engine / Runtime に触れておらず、golden は Runtime.exe が撮る。VERDICT の「再実行不要」に従った)

自己採点 (1-5):
- 仕様適合: 5 — spec §4.3 の確定表と 1 対 1。Accent 系の禁止もセルフテストで固定
- 正しさ: 5 — 表の一致・Accent 不使用を機械検査し、**わざと壊して赤くなることまで観測**。実機 3 枚で選択行・サムネイル・フォルダタイルの読みやすさを確認
- コード品質: 4 — 色表・グリフ・集約がいずれも 1 箇所。テストのための一時 ImGui context が唯一の不格好さ (理由をコメントに明記)
- テスト: 5 — 正常系 + 故障点の両方。ルール 3 違反は今後ビルドの度に止まる

不安・質問: なし (round 1 の質問 1 は本 round で解消、質問 2 は planner が spec で読み替え確定)

触ったファイル: (round 1 の一覧 + 今回の差分。今回変わったのは下の 3 本)
- `src/Editor/EditorWidgets.h` / `.cpp` (色表の置換 + 宣言コメント)
- `src/Editor/SourceControl/SourceControlSelfTest.cpp` ((d3) 追加 + imgui / EditorWidgets / ImGuiTheme の include)
- (round 1 から継続) `src/Editor/SourceControl/SourceControlState.h` / `.cpp`、`src/Editor/SourceControl/ScmHint.h` / `.cpp` (新規)、`src/Editor/EditorApp.cpp`、`src/Editor/AssetOps.cpp`、`src/Editor/Windows/AssetBrowserWindow.h` / `.cpp`、`src/Editor/Windows/SourceControlWindow.cpp`、`src/Editor/Windows/{AnimationWindow,AnimatorControllerWindow,AudioMixerWindow,ProjectSettingsWindow}.cpp`、`build/Editor.vcxproj` / `.filters`、`plans/m66-git-collab/sub-09.md`

申し送り:
- ImGui のスタイル色に依存する関数をヘッドレスのセルフテストから呼ぶときは、**context を 1 個作って捨てる**のが最小の手 (`SourceControlSelfTest` (d3) が実例)。バックエンドも `NewFrame` も要らない。`GetStyleColorVec4` は既定スタイルを返すので「テーマの値そのもの」を検査する用途には使えない (= 意味色は `themeColor::*` と直接比較する)。
- `themeColor` の新しい割り当てを足すサブは (d3) と同じ形の固定テストを添えること。**配色ルール違反は画面を見ても「なんとなく読みにくい」としか出ない** (M66i round 1 は選択行の上で初めて露見した)。
- 指摘 3 (`SoundGenWindow` の `.wav` にヒント無し) は sub-10 の申し送りへ。
- 生成物 (round 2 分): `cache\m66i_r2_*.png`、`cache\m66i_r2_*.log`。

## フィードバック履歴

## planner 追記 (sub-02 round 1 の判定から)

- フォルダ集約は sub-02 で **`CombineState` = 最も重いもの** に確定した (`{D, ?}` → D)。上の「子 M + ? → 親 M」はそのまま成り立つが、セルフテストのケースに `{D, ?}` → D と `{競合, M}` → 競合 を足すこと。集約の実体は `SourceControlState.cpp` の `PropagateFolderState` で、AssetBrowser 側はそれを引くだけ (再実装しない)。
- round 1: VERDICT REWORK (planner、2026-09-03)。must 1 件 = バッジ色表の M = Accent (EditorWidgets.cpp:173-174) がテーマ配色ルール 3 に違反 + 選択行で低コントラスト (cache/m66i_zoom_tree.png)。spec §4.3 に確定表を書いた (M = Warning / A = Success / D = Error / R = Prefab / 競合 = Error / ? = TextDisabled) — sub-02 の表を引き継いだのは一貫性として正しい判断だったが、共有化された今が直しどきなので両窓一緒に変える。裏取り: ImGuiTheme.h:17-18 ルール 3 の原文 / EditorWidgets.cpp:165-176 の共有表、SourceControlWindow.cpp:6,17,1077 が同表を参照 / SourceControlSelfTest.cpp:443-479 (d2) 13 件 / golden*.rep 18:22 再生成 (replay 最終状態) / m66i_zoom_tree.png で D = 黄・M = 青を目視。質問 2 は受け入れ条件の文言を読み替えて確定。差分 5 件 (色表の再利用 / ChangeState 戻り / ScmHint / ヒント合流 / 削除・移動元) は色表以外すべて採用。
- round 2: VERDICT OK (planner、2026-09-03)。must 1 の消込を現物で確認: EditorWidgets.cpp:167-177 の共有表が spec §4.3 の確定表と 1 対 1 (M = Warning / A = Success / D・競合 = Error / R = Prefab / ? = TextDisabled)、表の中に Accent 0 件 / SourceControlSelfTest.cpp:501-537 (d3) = 一時 ImGui context を作って捨て、Accent と AccentSoft の両方を弾く / cache/m66i_r2_regression.log:3563-3564 で M = Accent に戻すと 2 件 FAIL (故障点で赤くなることを観測) / cache/m66i_r2_zoom_tree.png で選択行の上の M が琥珀、D が赤で可読 / `git diff --stat -- src/Engine src/Runtime` = 空 (Editor 層のみ。replay / shot の再実行不要)。
