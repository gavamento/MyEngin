# sub-19: SpriteShape2D (描画・衝突・ハンドル)

- 依存: sub-08, sub-18
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.11、4.2.3

## やること
1. `.spriteshape.json` / `SpriteShapeLibrary` (プロファイル: `fill`、`edges[] {angleFrom, angleTo, sprite}`、`height`。GUID キー、ReloadHub、Create メニュー、Inspector 編集)。
2. `SpriteShape2DComponent` (**hash 対象**、TypeId 末尾): `profile / pointCount / points[24] / closed / smooth / collider / oneWay / sortingLayerId / orderInLayer`。Catalog、Create → 2D → Sprite Shape。`kSimSnapshotVersion` +1。
3. 純関数: `EvaluateSpline(points, count, closed, smooth) → 折れ線` (smooth = Catmull-Rom、1 区間 8 分割、端は端点を複製)、`BuildEdgeQuads(polyline, profile) → 頂点列` (各区間の角度で `edges[]` を選び、`height` の帯を区間に沿って敷く。UV の u は区間長 / スプライト幅で繰り返し)、fill は sub-18 の `Triangulate` を折れ線に掛ける (UV = ワールド座標 / ppu)。
4. 衝突: `collider` のとき、折れ線を `StaticShapeSource` (sub-08) の辺チェーン (薄い箔列、`oneWay` 継承、layer / mask は 0 / 全部 — 必要なら Collider と同じフィールドを足して差分に書く) として供給。sim 入力なので点が変われば箱も変わる (キャッシュは点列のハッシュで無効化)。
5. Scene View ハンドル: 点のドラッグ・追加・削除 (`UndoStack::Record`)。ギズモ: 折れ線 (白)、`collider` なら緑。
6. selftest: スプラインの点数、角度範囲の選択、球が 8 点の丘に沿って転がり落ちる (plane2D、Debug / Release ハッシュ焼き込み)、2 ワールド並走の一致。
7. 確認スクショ: 丘 (8 点 smooth、縁 3 種 + fill)。

## やらないこと (このサブでは)
- 角スプライト (corner sprites)、ベジェ接線のハンドル、開いた形状の fill

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Rendering\Sprite2D\SpriteShape.{h,cpp}` (ライブラリ + 純関数)、`SpriteShapeSelfTest.{h,cpp}`
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `Sprite2DRenderer.{h,cpp}`、`src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (供給元の登録、sub-11 と同じ口)、`SceneViewWindow.cpp`
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub`、`AssetBrowserWindow.cpp`、`AssetOps`、`InspectorWindow.cpp`
- `SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 33) スクショ `plans\m94-2d\screenshots\spriteshape.png`、ハンドルのエディタスクショ、selftest PASS (Debug / Release ハッシュ同値)。
2. SpriteShape2D の無いシーンの hash-diff が無差分 (opt-in 型)。replay_verify 全 PASS。
3. (spec 1-5) golden 不変、0 警告、check_rules 0、snapshot +1 のみ。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest` (両構成)、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
