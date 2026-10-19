# sub-18: Line2D

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.11

## やること
1. `Line2DComponent` (NoHash、TypeId 末尾): `pointCount / points[32] (Float2 ×32、個別登録) / width (0.1) / color / texture (AssetID、空 = 白) / loop / fill / sortingLayerId / orderInLayer`。Catalog、Create → 2D → Line。`kSimSnapshotVersion` +1。
2. 純関数 (Engine 層、selftest 対象): `BuildStroke(points, count, width, loop) → 頂点列` (各頂点で左右に width/2、継ぎ目はマイター (角度が鋭すぎる場合はベベル)、端は平ら)、`Triangulate(points, count) → index 列` (耳刈り、CW/CCW どちらでも、自己交差は未定義 = 入力側の責務)。`fill` は閉じた多角形を color で塗り、`texture` があれば UV = ローカル座標 / 1 単位。
3. `Sprite2DRenderer` の収集に Line2D を足す (同じソートキー規則、同じ VB)。Scene View ギズモ: 点列 (白) と、選択中の点ドラッグ・追加・削除 (sub-08 の流儀。コンポーネントのフィールドに直接書くので `UndoStack::Record` が使える)。
4. selftest `Line2DSelfTest`: 5 点の頂点数、loop の継ぎ目、凸 / 凹多角形の三角形数 (n − 2)、`pointCount` 0 / 1 / 2 の境界 (何も描かない / 描かない / 1 本)。
5. 確認スクショ: 5 点・幅 0.2 の折れ線 + 塗りの凹五角形。

## やらないこと (このサブでは)
- 丸いキャップ / ジョイン、破線、テクスチャの繰り返しモード、グラデーション

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- 新規 `src\Engine\Engine\Rendering\Sprite2D\Line2DGeometry.{h,cpp}`、`Line2DSelfTest.{h,cpp}`
- `Sprite2DRenderer.{h,cpp}`、`SceneViewWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 32) スクショ `plans\m94-2d\screenshots\line2d.png`、端が切れていない (端点の画素値)。selftest PASS。
2. (spec 1-5) golden 不変、0 警告、check_rules 0、snapshot +1 のみ。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest`、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`

## 実装メモ (coder が追記)

## フィードバック履歴
