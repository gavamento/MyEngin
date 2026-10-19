# sub-05: スプライトエディタ窓

- 依存: sub-03
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #22、4.1.2、4.3

## やること
1. `src\Editor\Windows\Sprite\SpriteEditorWindow.{h,cpp}` (`AnimatorControllerWindow` の構造: `bool open` + `OnImGui(EngineContext&, Selection&)` + `HasUnsavedChanges()`)。EditorApp に登録 (メンバ、`layouts_.Init` の ID "SpriteEditor"、毎フレーム呼び出し、Window メニュー、未保存判定)。Project 窓でテクスチャをダブルクリック / Inspector のテクスチャ節の「Open Sprite Editor」ボタンで開く。
2. 編集モデル `SpriteEditModel` (純関数 + 状態、`BehaviorTreeEditModel` の流儀): `.meta` の `sprites[]` を読み、`std::deque` の 128 段 Undo (`BeginGesture / EndGesture`)、保存時 JSON との比較で Dirty。
   - 純関数 (selftest 対象): `SliceGridByCellSize(texW, texH, cellW, cellH, offX, offY, padX, padY, keepEmpty) → rects`、`SliceGridByCellCount(cols, rows, ...)`、`SliceAutomatic(pixels RGBA8, w, h, minSize) → rects` (アルファ > 0 の 4 連結成分の AABB)、`ApplySlices(existing, newRects, method)` (method 0 = Delete Existing、1 = Safe: 既存と重なる新規を捨てる)、`PivotPreset(k) → (px, py)` (9 種)。
3. UI: 左に拡大・パン可能なテクスチャ表示 (ImGui の `Image` + 自前の矩形描画。ズームは整数倍と 1/2、Point 表示)、矩形の選択 / ドラッグ / リサイズ (8 ハンドル) / Delete、右に選択矩形の name / rect / pivot (プリセットコンボ + 数値) / border 4 辺、上に Slice パネル (Type: Grid by Cell Size / Grid by Cell Count / Automatic、Method、Pivot)、Apply (= Slice を実行して矩形を置き換え)、Save (Ctrl+S)。Save は `AssetDatabase::EnsureMeta → ReadMeta → 書き換え → WriteMeta` → `SpriteLibrary::Register(texPath)` → `TextureLibrary::ReplaceFromFile` (filter / ppu が変わったときだけ再読込は不要だが、流儀どおり呼ぶ)。
4. Inspector のテクスチャ Import Settings (`InspectorWindow.cpp:3078-3108`) に `filter` と `pixelsPerUnit` を足す (sprites は窓で)。この Apply も `SpriteLibrary::Register(texPath)` を呼ぶ (`.meta` 単体の変更は ReloadHub が拾わない。sub-03 の申し送り)。`.meta` に重複 id があれば後勝ちで WARN (spec R6) — `SpriteLibrary::Register` がそうなっているか確認し、違えば直す。
5. 文字列は全部 `Tr()`。selftest `SpriteEditorSelfTest` (純関数): 4×4 グリッド、連結成分 3 個の合成画像、Safe の重なり除外、Undo 3 段。

## やらないこと (このサブでは)
- カスタム物理形状 (スコープ外)、アトラスの自動パッキング (スコープ外)、アニメのプレビュー (sub-07)

## 触る場所 (planner の見立て)
- 新規 `src\Editor\Windows\Sprite\SpriteEditorWindow.{h,cpp}`、`SpriteEditModel.{h,cpp}`、`src\Editor\SelfTest\SpriteEditorSelfTest.{h,cpp}` (Editor 層の selftest の置き場は既存の `src\Editor\SelfTest\` に合わせる)
- `src\Editor\App\EditorApp.{h,cpp}` (:100-122, :1045-1069, :1602-1628, :357 / :1960)
- `src\Editor\Windows\Project\AssetBrowserWindow.cpp:745-778` (ダブルクリック)、`InspectorWindow.cpp:2965-3108`
- `LocalizationTable.inl`、`EditorMain.cpp` (selftest)、vcxproj
- 参考: `src\Editor\Windows\AI\BehaviorTreeEditModel.h:179-238` (Undo deque)、`src\Editor\Undo\UndoStack.h` (使わない理由は spec 2. #22)

## 受け入れ条件 (このサブ)
1. (spec 14) 純関数 selftest PASS (期待矩形の一覧をテストに書く)。Undo 3 段。
2. (spec 14) 実機: 16×16 ×4 の市松 PNG を Automatic で 4 矩形に切り Save → `.meta` に `sprites` 4 個、`Sprite2D.sprite` にピッカーから 1 枚を選んで表示が変わる。エディタスクショ `plans\m94-2d\screenshots\sprite_editor.png`。
3. (spec 1-3) 0 警告、check_rules 0 (規則 10)、`MYE_SELFTEST_ONLY=m94` に `SpriteEditorSelfTest` を足して PASS、版 bump なし。
4. **チェックポイント (sub-01〜05 の分、spec 4-5)**: `Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`tools\replay_verify.bat` 全ジョブ PASS、`tools\shot_verify.bat` + 既存 golden 30 枚 `--tol 0` で maxDiff=0。割れたら sub-01〜05 のどれが原因かを `--hash-diff` / `--img-diff` で特定して SELF_EVAL に書く (planner が差し戻し先を決める)。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
**チェックポイント (このサブで回す)**:
- `bin\x64\Debug\Editor.exe --selftest` と `bin\x64\Release\Editor.exe --selftest` (全体)
- `tools\replay_verify.bat`
- `tools\shot_verify.bat` (Release) + 既存 30 枚に `Editor.exe --img-diff tests\golden\X.png tests\actual\X.png --tol 0`

## 実装メモ (coder が追記)

SELF_EVAL: sub-05 (round 1)
実装:
  - src/Editor/Windows/Sprite/SpriteEditModel.{h,cpp} (新規): 純関数 spriteedit::SliceGridByCellSize / SliceGridByCellCount / SliceAutomatic (4 連結成分の AABB、最初の画素の走査順) / ApplySlices (DeleteExisting / Safe) / PivotPreset (9 種) / PivotPresetOf / RectsOverlap / ClampRectToTexture と、編集モデル SpriteEditModel (.meta の tex.sprites を読み、deque 128 段の Undo / Redo、BeginGesture / EndGesture、保存時の内容との比較で Dirty、Save = EnsureMeta → ReadMeta → sprites だけ差し替え → WriteMeta → SpriteLibrary::Register)
  - src/Editor/Windows/Sprite/SpriteEditorWindow.{h,cpp} (新規): 窓。ツールバー (Undo / Redo / Save / Revert / Fit / 倍率)、Slice パネル (Type 3 種 / Method / Pivot 9 種 + カスタム / Apply)、キャンバス (整数倍と 1/2 のズーム、中・右ドラッグでパン、矩形の選択・移動・8 ハンドルのリサイズ・空き地ドラッグで新規、Delete、pivot と border の描画)、右パネル (一覧、name / position / size / pivot / border)、未保存の確認モーダル、Ctrl+Z/Y/S/Delete (窓にフォーカスがあるときだけキーを握る)。Save 後に TextureLibrary::ReplaceFromFile
  - src/Editor/SelfTest/SpriteEditorSelfTest.{h,cpp} (新規): 33 項目 (グリッド / 自動 / Apply / pivot / Undo・ジェスチャ・128 段 / Save / 重複 id)。EditorMain.cpp の MYE_SELFTEST_ONLY=m94 と全体の両方へ接続
  - src/Editor/App/EditorApp.{h,cpp}: メンバ spriteEditor_、layouts_ に "SpriteEditor"、毎フレーム呼び出し (AssetBrowser と Inspector の TakePendingOpenSpriteEditor を渡す)、Window メニュー、未保存判定を 2 か所のゲート入力へ
  - src/Editor/Windows/Asset/AssetBrowserWindow.{h,cpp}: テクスチャのダブルクリック = スプライトエディタで開く (従来は OS の既定アプリ)。Import Settings モーダルの Apply が sprites を上書きしないよう保持
  - src/Editor/Windows/Scene/InspectorWindow.{h,cpp}: テクスチャの Import Settings に filter / pixelsPerUnit / [Open Sprite Editor]。Apply は sprites を保持し、SpriteLibrary::Register を呼ぶ (sub-03 の契約)
  - src/Engine/Engine/Rendering/Sprite2D/SpriteLibrary.cpp: Register で .meta の重複 id を後勝ち + MYE_LOG_WARN にした (従来は静かに後勝ちで、ids へ重複 push していた)
  - src/Editor/SourceControl/GitTransaction.{h,cpp} / SourceControlSelfTest.cpp: GateBlocker::SpriteEditorDirty (+ GateInputs.spriteEditorDirty、DocumentDirty.spriteEditor) と文言。表の static_assert に追随
  - src/Engine/Core/Localization/LocalizationTable.inl: Win_SpriteEditor / Spr_* / Insp_OpenSpriteEditor / Insp_SpriteFilter / Insp_PixelsPerUnit / GateB_SpriteEditorDirty (英日)
  - build/Editor.vcxproj(.filters): gen_project_files.ps1 の出力
仕様との差分:
  - [逸脱] テクスチャの「Point 表示」は未実装 (常にリニア)。ImGui のサンプラを差し替えるには Editor 層で D3D を触る必要があり (AGENTS 4.1)、窓の仕様 1 行のために境界を破らない。1/2 倍と整数倍の段は守る。ズームを上げると滲む
  - [追加] SliceGridByCellSize / SliceGridByCellCount に末尾引数 const uint8_t* rgba = nullptr — keepEmpty = false (空セルを捨てる) には画素が要るため。null なら keepEmpty に関係なく全セルを返す
  - [追加] ApplySlices に firstId / namePrefix 引数。Delete Existing でも id を既存の最大 + 1 から振る (消した id を使い回すと、古い Sprite2D の参照が別の絵を指すため。代わりに再スライスのたび id が増える)
  - [追加] 窓の OnImGui は (EngineContext&) のみ (Selection を取らない — 使わないので)。窓は既定で閉じ、Project のダブルクリック / Inspector のボタン / Window メニューで開く
  - [追加] GateBlocker::SpriteEditorDirty を新設 (HasUnsavedChanges をゲートへ繋ぐため。controllerDirty 等への相乗りは理由の表示が嘘になるので避けた)
  - [追加] 空き地のドラッグで矩形を新規追加 (spec 4.1.2 の「手動追加」)。幅・高さとも 1 画素以内のドラッグは作らない (クリックの揺れ)
  - [追加] AssetBrowser の Import Settings モーダルと Inspector の Apply が、選択時のコピーで sprites を上書きしないようにした (放置するとスプライトエディタで保存した後の Apply でスライスが消える)
  - [追加] .dds など stb が読めない形式をダブルクリックすると、窓はエラー行を出すだけになる。テクスチャのダブルクリックが従来の OS 既定アプリ起動でなくなった
検証:
  - msbuild MyEngine.sln Debug / Release (/clp:ErrorsOnly;WarningsOnly) -> 0 警告 0 エラー (一時プローブを除去した最終状態で再ビルド)
  - pwsh tools\gen_project_files.ps1 -> 実行済み
  - pwsh tools\check_rules.ps1 -> 3 error (既存: EditorMain.cpp の #ifdef _DEBUG、Editor.bat / Setup.bat の LF) / 50 warning。着手前と同じ件数。今回のファイル由来の新規は無い (InspectorWindow.cpp の rule 7 2 件は既存行)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest (Debug) -> Sprite2DAsset ALL PASS、SpriteEditor PASS (0 failure)、exit 0。重複 id の WARN が stderr に出ることも確認
  - [チェックポイント] Editor.exe --selftest 全体: Debug exit 0 / Release exit 0 (並行実行)。SpriteEditor suite を含み、FAIL 無し
  - [チェックポイント] tools\replay_verify.bat -> 6 分 18 秒。runner の失敗ジョブは rules の 1 本だけ (check_rules の既存 error 3 件による exit 1)、[diag] no mismatch markers。ハッシュ照合ジョブの失敗は無し。ただし私が保持できたのは出力の末尾 40 行で、各ジョブの PASS 行の個別確認は未実施
  - [チェックポイント] tools\shot_verify.bat (Release、既定 tol=3) -> [PASS] 30 shots。続けて既存 golden 30 枚を --img-diff --tol 0 で照合: 29 枚 maxDiff=0。demo_terrain_deferred だけ maxDiff=5 / diffPixels=2 / 518400
      - 原因の切り分け: M94 より前 (基点 25fb355) を git archive で別ディレクトリへ展開し Runtime.exe を Release ビルド、同じ条件 (--warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --terrain-demo --deferred) で撮影 -> 基点の絵と M94 (sub-01〜05) の絵は maxDiff=0 でビット一致、基点の絵も golden に対しては同じ maxDiff=5 / 2 画素。よって sub-01〜05 は原因ではない。shot_verify.bat 冒頭が書く「異方性フィルタは WARP のビルド違いで一致しない (この 1 枚だけ tol=12)」と同じ種類と推測する (この機械の OS は 10.0.26300、golden を撮った WARP の版は未確認)。一時ディレクトリは削除済み (cache の junction は先に外し、本物の cache が残ることを確認)
  - 実機 (Release Editor.exe --warp、一時 probe 経由): 64x32 の市松 PNG (4 色の 16x16、斜めにだけ接する) を開く -> 自動分割 (Automatic、Delete Existing) -> Save -> 1 枚選択、を --frames 60 --shot-frame 45 で撮影。plans\m94-2d\screenshots\sprite_editor.png。4 矩形、選択矩形の 8 ハンドルと pivot (赤点)、右パネルの name / position / size が出ている。窓の既定サイズが小さくキャンバスが潰れていたのを SetNextWindowSize (960x680、初回のみ) で直した後の絵
  - .meta の中身 (保存直後、1 回目の Apply、id 1〜4):
      "tex": { "compress": 1, "filter": 2, "generateMips": 0, "sprites": [
        { "id": 1, "name": "checker_0", "pivot": [0.5, 0.5], "rect": [0, 0, 16, 16] },
        { "id": 2, "name": "checker_1", "pivot": [0.5, 0.5], "rect": [32, 0, 16, 16] },
        { "id": 3, "name": "checker_2", "pivot": [0.5, 0.5], "rect": [16, 16, 16, 16] },
        { "id": 4, "name": "checker_3", "pivot": [0.5, 0.5], "rect": [48, 16, 16, 16] } ], "srgb": 0 }
      (rect は左上原点の画素。filter 2 は撮影用に私が書いた種値で、保存後も保たれていた = filter / srgb を壊さない確認)
  - 「Sprite2D.sprite にピッカーから 1 枚を選んで表示が変わる」: ピッカーの実クリックは未実施 (自動操作の手段が無い)。代わりに、Save 後の SpriteLibrary::Enumerate (ピッカーの元データ) が "checker" と "checker#checker_0..3" の 5 行を返すことをログで確認し、Sprite2D.sprite にスライス id を直接書いた一時シーンを Runtime.exe --warp で撮った: 全面スプライトは 4 色の市松、スライス指定の 2 個は緑 1 枚と黄 1 枚だけが表示された (期待どおり)。なお再スライスで id が 5〜8 に進んだ後に古い id (2, 4) を書いた最初の撮影はマゼンタ (未解決) になり、id の継続 (上記差分) と未解決フォールバックの両方を実地で見た。一時ファイル (assets\_tmp_m94e、一時シーン、プローブ用の差分、SpriteLibrary のデバッグログ) は全て削除・除去済み
  - imgui.ini: 撮影のため一時退避し、元に戻した
  - 未検証: ズーム / パン / ドラッグ (移動・リサイズ) / Delete / Ctrl+S / 数値欄 / 未保存モーダル / Inspector の [Open Sprite Editor] と Import Settings の filter・ppu / Project のダブルクリック / Window メニュー / ゲートの表示は、コンパイルと純関数・モデルの selftest までで、実操作は未確認 (ユーザー目視。台帳の sub-24 へ)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1・3・4 を満たし、2 は Point 表示の逸脱とピッカー実クリック未実施が残る
  正しさ: 4 — 純関数とモデルは selftest 33 項目 + 全体 selftest / replay_verify / shot_verify が通る。窓の入力処理 (ドラッグ等) は実操作未確認
  コード品質: 4 — モデルと窓を分け、純関数に寄せた。窓の DrawCanvas が長い (入力と描画を 1 関数に置いた)
  テスト: 4 — 純関数と Undo と Save と重複 id を selftest で守る。GUI の入力経路は無い
不安・質問:
  - Point 表示は仕様の窓要件に入っているが未実装 (上記)。許容するか、Renderer 層に ImGui 用サンプラ切替の口を足す小サブを切るか判断してほしい
  - Delete Existing でも id を継続させた設計 (再スライスのたびに id が増える) でよいか。1 から振り直す方式だと、古い Sprite2D の参照が別の絵を指す
  - テクスチャのダブルクリックを OS 既定アプリからスプライトエディタへ変えた。.dds などの非対応形式はエラー表示止まりになる
  - demo_terrain_deferred の tol 0 差 (2 画素) は環境差と判断したが、golden を撮った WARP の版は確認していない
触ったファイル:
  build/Editor.vcxproj, build/Editor.vcxproj.filters,
  src/Editor/App/EditorApp.cpp, src/Editor/App/EditorApp.h, src/Editor/App/EditorMain.cpp,
  src/Editor/SourceControl/GitTransaction.cpp, src/Editor/SourceControl/GitTransaction.h, src/Editor/SourceControl/SourceControlSelfTest.cpp,
  src/Editor/Windows/Asset/AssetBrowserWindow.cpp, src/Editor/Windows/Asset/AssetBrowserWindow.h,
  src/Editor/Windows/Scene/InspectorWindow.cpp, src/Editor/Windows/Scene/InspectorWindow.h,
  src/Editor/Windows/Sprite/SpriteEditModel.cpp, src/Editor/Windows/Sprite/SpriteEditModel.h,
  src/Editor/Windows/Sprite/SpriteEditorWindow.cpp, src/Editor/Windows/Sprite/SpriteEditorWindow.h,
  src/Editor/SelfTest/SpriteEditorSelfTest.cpp, src/Editor/SelfTest/SpriteEditorSelfTest.h,
  src/Engine/Core/Localization/LocalizationTable.inl, src/Engine/Engine/Rendering/Sprite2D/SpriteLibrary.cpp,
  plans/m94-2d/screenshots/sprite_editor.png, plans/m94-2d/sub-05.md (実装メモ節のみ)
申し送り:
  - sub-07 (アニメのプレビュー) / sub-12 (タイルパレット窓) は SpriteEditModel の BeginGesture / EndGesture と deque Undo の形を流用できる
  - SpriteEditModel::Save は TextureLibrary を触らない (窓が ReplaceFromFile を呼ぶ)。窓以外から保存するなら同じ組を呼ぶこと
  - ユーザー目視 (sub-24): スプライトエディタ窓の実操作一式 (上記の未検証欄)、Inspector のスプライトピッカー (sub-04 から持ち越し)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20、チェックポイント 1)。スクショ `sprite_editor.png` で Automatic 分割の 4 矩形・8 ハンドル・pivot・右パネルを確認。`SpriteLibrary.cpp` の重複 id 後勝ち + WARN、`AssetBrowserWindow.cpp` の sprites 非上書きを差分で確認。チェックポイント: 全体 selftest 両構成 exit 0、replay_verify はハッシュ照合ジョブに mismatch 無し (失敗は `rules` = 既存 3 error)、shot_verify 30 枚 PASS、`--tol 0` は 29 枚 0 / `demo_terrain_deferred` は基点 `25fb355` の絵とビット一致 (golden との 2 画素差は WARP の版差) → spec 5. 条件 4 に例外を明記。[逸脱] Point 表示と [追加] のダブルクリック挙動は sub-06 の追補 8 (a)(b) へ。id 継続は採用 (spec 4.1.2)。should: 次のチェックポイントからは replay_verify の全文ログを `cache\m94\` に残す。
