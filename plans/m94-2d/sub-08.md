# sub-08: 2D 形状 (`kPolygon2D` / `kEdge2D`、`.shape2d.json`) と C++ クエリ

- 依存: sub-01
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #19、4.1.8

## やること
1. `collidershape::kPolygon2D = 6`、`kEdge2D = 7`、`kCount = 8`。`.shape2d.json` と `ShapeLibrary2D` (`src\Engine\Engine\Physics\Collider\Shape2DLibrary.{h,cpp}`、GUID キー、ReloadHub、Create メニュー、Inspector 編集は点の一覧)。`ColliderComponent.meshAsset` で参照 (kMesh / kTerrain と同じ流儀。Inspector の表示名を shape で切り替える)。
2. 解決 (`PhysicsSystem` の静的収集 :1701 と動的ボディの形状確定):
   - 静的 `kPolygon2D`: 点列を耳刈りで三角形化し、z ∈ [−depth/2, +depth/2] のプリズム (前後面 + 側面) の三角形スープを作って kMesh の経路に流す (`ConvexColliderLibrary` / `CookedCache` のような焼き込みは不要。メモリ上のキャッシュを `ShapeLibrary2D` が持つ)。凹も可。
   - 動的 `kPolygon2D`: プリズムの頂点の凸包を kConvex の経路へ (`ConvexCollision.cpp` の既存)。凹は埋まる (Inspector 注記)。質量・慣性は kConvex の既存計算。
   - `kEdge2D` (静的のみ。動的に付いていたら WARN して無視): 線分ごとに薄い箱 (半厚 `edgeRadius`、長さ = 線分、z 半厚 = depth/2、Z 回転 = 線分の向き) を**静的収集に複数の ShapePose として供給する口**を作る (sub-11 のタイル箱、sub-19 の SpriteShape も同じ口を使う。`StaticShapeSource` のような小さなインターフェースで、PhysicsSystem が tick ごとに `Collect(std::vector<StaticShape>&)` を呼ぶ。所有は呼び出し側)。`oneWay` は Collider の値を全線分に継承。
3. クエリ: `OverlapBoxWorld` にマスク付きの口 (`PhysicsQueries.cpp:80` に `mask` 引数の overload)、`BoxCastWorld(center, halfExtents, dir, maxDist, mask, skip, outHit)` (`SphereCastWorld` :92 の 32 回保守的前進と同じ構造。AABB vs 形状の最近接距離は `shapes::` の既存 Overlap を距離化できないなら、箱の 8 頂点 + 中心からの `Raycast` で近似してよい — 採った方式を差分に書く)。
4. Scene View ギズモ: `kPolygon2D` / `kEdge2D` の点列 (緑)、選択中は点をドラッグ (`HandlePatrolRoute` の流儀)、辺クリックで追加、Delete で削除。編集はアセット (`.shape2d.json`) に書く (独自 Undo は sub-05 の `SpriteEditModel` と同型の小さいもの、またはドラッグ 1 回 = 保存 1 回で Undo 無し。採った方を差分に書く)。
5. PhysicsSelfTest に `Shapes2D` 節 (spec 4.1.8 の 5 項目) + Debug / Release のハッシュ焼き込み。

## やらないこと (このサブでは)
- ABI 露出 (sub-14)、タイルの箱 (sub-11)、SpriteShape (sub-19)

## 触る場所 (planner の見立て)
- `Components.h:203-211` (shape 列挙)、`Components.cpp` (和名の説明)
- 新規 `src\Engine\Engine\Physics\Collider\Shape2DLibrary.{h,cpp}`、`StaticShapeSource.h` (名前は任意)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.{h,cpp}` (:1701 静的収集、形状確定、:1843-2054 凸包の質量)、`PhysicsQueries.cpp:80-92`、`Shapes.{h,cpp}` (必要なら)
- `src\Engine\Engine\Physics\Collider\ConvexCollision.cpp` (読むだけで済むはず)
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub`、`AssetBrowserWindow.cpp`、`AssetOps`、`InspectorWindow.cpp` (Collider 節の表示)
- `SceneViewWindow.cpp` (ギズモ)、`PhysicsSelfTest.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 18) `Shapes2D` 節 5 項目が Debug / Release で PASS、焼いたハッシュ同値。
2. (spec 18) 点編集のエディタスクショ `plans\m94-2d\screenshots\shape2d_edit.png`。
3. 既存形状 (0..5) のシーンの hash-diff が無差分 (`--physics-demo`、`--joint-demo`、`--fracture-demo`)。replay_verify 全 PASS。
4. (spec 1-5) golden 不変、0 警告、check_rules 0、版 bump なし (shape は int32 の値追加、形は変わらない)。

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest` (両構成)、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`
- `tools\shot_verify.bat` + 既存 30 枚 `--tol 0`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
