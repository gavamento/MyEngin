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
4. Inspector のテクスチャ Import Settings (`InspectorWindow.cpp:3078-3108`) に `filter` と `pixelsPerUnit` を足す (sprites は窓で)。
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
3. (spec 1-5) golden 不変、0 警告、check_rules 0 (規則 10)、版 bump なし。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest`、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`

## 実装メモ (coder が追記)

## フィードバック履歴
