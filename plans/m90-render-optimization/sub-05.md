# sub-05: メッシュ LOD (meshoptimizer、.meta でオプトイン、kCookVersion 6、選択、UI)

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
spec §4.1.2、§4.2、§2 #5 #6 #17 #18 #19。
1. meshoptimizer をソースのままコミット (`external\meshoptimizer\`、使う .cpp だけ)。`external\VERSIONS.md` に版・コミット・ライセンス (MIT)。vcxproj のフラグは Engine と揃える (`/fp:precise`、警告設定)。memory「スクリプト編集の罠: MSBuild のフラグは揃える」。
2. `.meta` にモデルの LOD 設定 (段数 0..3 追加段、段ごとの目標三角形比、screen-size 閾値。無い = 段なし)。`ImportMetaResolver` / `AssetDatabase` のテクスチャ設定と同じ流儀で解決する (Renderer 層から Engine 層を参照しない)。
3. クック: 両ローダ (glTF / FBX) の登録地点で、段ありのメッシュに `meshopt_simplifyWithAttributes` (法線・UV を属性に、境界ロック) を段ごとに掛け、元の頂点を指す IB を LOD0 の後ろに連結して段表を作る。目標に届かない段は作らない。`CookedMesh` に段表と生成に使った設定を入れる。kCookVersion 5→6 (`CookedCache.h` に理由の段落)。フレッシュパースと Replay がバイト一致。
4. キャッシュの無効化: blob に記録した LOD 設定と現在の `.meta` を比べ、違えば再クック (§2 #19)。
5. `Mesh` (GPU) に段表。描画は選ばれた段の範囲で `DrawIndexed*`。インスタンシングの run は (mesh, 段) で分ける。sub-02/03 のオクルージョンの indirect 引数も段の範囲を使う。
6. 選択関数 (純関数): ワールド AABB の外接球の screen-size、段の閾値 × `lodBias`、ヒステリシス、強制段、段の欠落。前フレームの段は viewKey ごとに持つ (履歴なし = 距離だけ)。影のキャスターはカメラ基準の段。
7. 物理・NavMesh・RT・MeshLibrary の CPU コピーは LOD0 (今の IB) のまま。
8. `RenderSystem` の `lodBias` / 強制段、エディタの描画設定メニューと、モデルの import 設定 UI (段数・比)。`Tr()` 両言語。
9. `render_bench` の高ポリのモデルに `.meta` で段を付ける (アセットの追加は最小。既存アセットの `.meta` を変えるなら、それを使う golden が無いことを確認)。
10. 統計: LOD 段ごとの描画数・tri。
11. 失敗の局所化: 段を作れないメッシュは段なしで登録 + WARN 1 回。

## やらないこと (このサブでは)
- 手作り LOD の読み込み、dither / crossfade、LOD 選択の並列化 (sub-06)。

## 触る場所 (planner の見立て)
- `external\`、`build\Engine.vcxproj(.filters)` (`tools\gen_project_files.ps1`)
- `src\Engine\Engine\Asset\ModelCook.*`、`CookedCache.h`、`ModelLoader` / `FbxLoader.cpp` の登録地点
- `src\Engine\Core\Asset\ImportMetaResolver.*`、`src\Engine\Engine\Asset\AssetDatabase.*`
- `src\Engine\Renderer\Device\GpuResources.h/.cpp` (`Mesh`、`MeshLibrary::Register`)
- `src\Engine\Engine\Rendering\RenderSystem.cpp`、`MeshInstancing.h`、各描画パスの `DrawIndexed*`
- Editor: `AssetBrowserWindow` / `InspectorWindow` のテクスチャ import 設定の隣、`EditorApp.cpp` の描画設定メニュー

## 受け入れ条件 (このサブ)
1. selftest: 段ありのクックで段ごとの三角形数が単調減少、2 回のクックがバイト一致、キャッシュ経由とフレッシュパースがバイト一致、段なしのメッシュは v5 と同じ IB・頂点バイト列。
2. selftest: `.meta` の段数を変えると手動削除なしで再クックされる。
3. selftest: 選択関数の境界値 (上げ・下げ閾値、lodBias、強制段、段の欠落、履歴なし)。
4. selftest: 段ありのモデルでも物理コライダーと NavMesh の入力三角形数が LOD0 のまま。
5. `render_bench` の dump で LOD ON は OFF (強制段 0) より tri が減り、段の分布が出る。近景は LOD0。遠景のスクショを報告 (目視はユーザー)。
6. 既存 golden 全 PASS (段を付けたモデルを使う golden が無いこと)。
7. `tools\check_rules.ps1` PASS、ローカライズ両言語。
8. 段を作れないメッシュ (例: 三角形が数枚) は段なしで登録される (selftest)。

## 検証コマンド
- `tools\gen_project_files.ps1`、ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`
- `render_bench` の dump と撮影、`tools\replay_verify.bat` (クック形式の変更で cook 有無のビット一致 M51b が保たれるか)

## 実装メモ (coder が追記)

## フィードバック履歴
