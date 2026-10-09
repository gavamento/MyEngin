# sub-05: メッシュ LOD (meshoptimizer、.meta でオプトイン、kCookVersion 6、選択、UI)

- 依存: sub-01
- 状態: OK (commit 29a775e)
- 往復: 1

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

### SELF_EVAL: sub-05 (round 1)

実装:
- `external\meshoptimizer\` (v1.3 `9e1f07b`、meshoptimizer.h / allocator.cpp / indexgenerator.cpp / simplifier.cpp / LICENSE.md、改変なし)、`external\VERSIONS.md` に追記、`build\Engine.vcxproj` に 3 本 (WarningLevel3、他の external と同じ手書き)、`tools\gen_project_files.ps1` の external 一覧へ追加 (filters 用)。フラグは `Common.props` の `/fp:precise` を継承。
- `ImportMetaResolver.h/.cpp` — `importmeta::ModelLodSettings` (levels 0..3、ratio[3]、screenSize[3]、Normalize、`==`) と `InstallModelLod` / `ResolveModelLod`。`AssetDatabase.h/.cpp` — `AssetMeta::lod`、`.meta` の `"lod"` を読み書き (段なしは書かない = 既存 .meta はバイト一致)、`ModelLodThunk` (`InstallAsKeyResolver` が接続)。
- `Engine\Asset\MeshLodBuilder.h/.cpp` (新規) — `BuildMeshLods`: 頂点を溶接 (`meshopt_generateVertexRemap`、FBX は三角形ごとに頂点を持つので必須) → 段ごとに `meshopt_simplifyWithAttributes` (法線・UV、スキンは骨ウェイトも属性、`LockBorder`、誤差上限 0.05) → 元の頂点を指す index へ戻す。目標の 1.25 倍以内に届かない / 前の段より減らない段は作らず、最初の段から作れなければ段なし + WARN (key ごとに 1 回、届いた数と目標つき)。screenSize は .meta の値、0 なら `0.5*sqrt(ratio)`、前の段の 0.8 倍以下に頭打ち。
- `ModelCook.h/.cpp` — `CookedMesh` に `lodIndices` / `lods`、`ModelCookData::lodSettings`、`AddMesh(…, lod)`、`RegisterMeshWithLods` (両ローダ共通の登録地点)、blob の先頭に LOD 設定 + メッシュごとに段表 (Serialize / Deserialize / Replay)、`TryReplayFromCache` が現在の .meta と blob の設定を比べて違えば再クック (封印キャッシュは比べない)。`CookedCache.h` — kCookVersion 5→6 (理由の段落)。`ModelLoader.cpp` / `FbxLoader.cpp` — `LoadContext::lodSettings` を `ResolveModelLod` で解決し、登録を `RegisterMeshWithLods` へ (Load / RegisterAssets の両経路)。
- `GpuResources.h/.cpp` — `MeshLodLevel`、`Mesh::lods` / `lodIndices` / `LodRange(lod)`、`MeshLibrary::Register` に `lodIndices` / `lodLevels` (矛盾した段表は段なしで登録 + WARN)、IB は LOD0 の後ろに連結、`RecreateGpu` も連結して作り直す。`Mesh::indices` / `indexCount` は LOD0 のまま (物理・NavMesh・RT・MeshLibrary の CPU コピーは変えない)。
- `Renderer\Mesh\MeshLod.h` (新規) — 純関数 `LodScreenSize` / `SelectLod` (ヒステリシス `kLodHysteresis` = 10%、強制段、段の欠落、履歴なし) と `LodHistory` (viewKey ごとの前フレームの段、世代つき)。
- `RenderTypes.h` `RenderItem::lod`、`RenderQueue.cpp` のソートに mesh の次に lod、`MeshInstancing.h` の run 条件に lod。`DeferredPath.cpp` / `ForwardPath.cpp` / `ShadowPass.cpp` / `ShadowAtlas.cpp` の本描画・影・サーフェス・透明の全 `DrawIndexed*` を `Mesh::LodRange(item.lod)` の範囲へ (オクルージョンの indirect 引数の `indexCount` / `startIndex` もこれ)。Picking / Ghost / Terrain / Water は LOD0 のまま。
- `RenderSystem.h/.cpp` — `lodBias` / `lodForcedStage`、`lodHistory_[4]`、ステージ 2 (並列) で段を決める (履歴は読むだけ)、ステージ 3 (直列) で履歴を書く。画面外の影キャスターも同じ式。`EngineLoop.h/.cpp` / `EngineCli.cpp` — `--lod-bias F` / `--lod-force N`。
- `Profiler.h/.cpp` — `AddDraw(triangles, lod, instances)`、`RenderStats::lodTriangles[4]`、`RenderStatsDump.cpp` に `lodTriangles`。`ProfilerWindow.cpp` — LOD 分布の行。`EditorApp.cpp` — Rendering メニューに Mesh LOD (bias スライダ、段の固定)。`InspectorWindow.h/.cpp` — モデルアセットの LOD 設定 (段数・比・画面高さ比、適用で `.meta` を書いて `ReloadMeshes`)。`LocalizationTable.inl` — 両言語の文字列 11 件。
- `DemoContent.cpp` `BuildRenderBenchScene` — `assets\models\lod_sphere.glb` (+ `.meta` で 3 段、`tools\gen_lod_test_gltf.ps1` が生成、93 KB) を登録し、遠景の球 8 個を LOD 球へ、末尾に LOD の見本 8 個 (カメラから 14 / 34 / 43 / 65 m の左右 1 組ずつ) を追加。
- テスト: `Engine\Asset\MeshLodSelfTest.h/.cpp` (新規、`EditorMain.cpp` に登録)、`RenderStatsSelfTest.cpp` (lodTriangles)、`EngineCliSelfTest.cpp` (`--lod-bias` / `--lod-force`)。

仕様との差分:
- [逸脱] `lodBias` の向き: spec §4.1.2 は「段の閾値 × lodBias」だが、同じ節の「Unity の `QualitySettings.lodBias` 相当」に合わせ、`screenSize × lodBias` と閾値を比べる (> 1 で詳細な段を長く使う)。字面どおりだと > 1 が粗くなり Unity と逆になる。反転は `SelectLod` の 1 行 — 理由: 仕様内の 2 つの記述が食い違うため。
- [追加] LOD 設定の保持場所を「`CookedMesh`」でなく `ModelCookData` (blob の先頭、ファイル単位) にした。`.meta` はファイル単位で、メッシュごとに同じ値を持つ意味がなく、先頭に置くとキャッシュの照合を全部読む前に済ませられる。段表と段の index 列は `CookedMesh` (メッシュ単位)。
- [追加] 既定値: ヒステリシス 10% (`kLodHysteresis`)、自動の screenSize = `0.5*sqrt(ratio)` (比 0.5 / 0.25 / 0.125 で 0.354 / 0.25 / 0.177)、誤差上限 0.05、目標への届き具合 1.25 倍、属性の重み 0.5。spec に数値が無いため。
- [追加] `--lod-bias` / `--lod-force` (CLI)、Profiler / dump の `lodTriangles`、`AddDraw` の引数追加、`RenderQueue::Sort` の lod キー、ProfilerWindow の LOD 行、`tools\gen_lod_test_gltf.ps1` と `assets\models\lod_sphere.glb(.meta)`。render_bench に LOD 球を足したので **dump の基準値 (tris 等) が sub-01〜04 のものから変わる** (既存の生成順は変えず末尾に追加、遠景 8 個は球の種類だけ差し替え)。render_bench を使う golden は無いことを確認 (`tests\golden` / selftest に参照なし)。
- [未実装] アセットブラウザの右クリック「インポート設定」には出していない (Inspector のみ。アセットを選択すると出る)。spec は「アセットブラウザ / Inspector」。
- [未実装] 履歴 (`LodHistory`) をシーン切替でクリアしていない。世代が違えば履歴なし扱いなので影響は「境目付近の初回の段がヒステリシスの帯の分だけ前のシーンに引かれる」だけ (決定的撮影は 1 プロセス 1 シーンなので再現する)。

検証:
- `pwsh tools\gen_project_files.ps1` → Engine.vcxproj の external は手書きのまま、filters 更新。
- Debug / Release ビルド → 警告・エラーなし (最終ソースで両構成)。
- `bin\x64\Release\Editor.exe --selftest` → exit 0 (80 秒、最終ソース)。`MeshLod self test: ALL PASS` (85 件: 選択・履歴・式 30、生成 11、登録・blob 往復 12、GPU の IB 連結と復旧 4、.meta 5、GLB の実ファイル 12、実アセット 11)、`Render stats self test: PASS`、`Engine CLI self test: OK`、`CookedCache self test: ALL PASS`。
- `bin\x64\Debug\Editor.exe --selftest` → 最終ソースで exit 0 (8 分 2 秒)。**途中のソース (MeshLod の最初の版) の Debug 実行では exit 1**: `ServerNetSelfTest` の `V1 LoadPersist / LoadGame in a session` 2 項目だけが FAIL (台帳の一過性 FAIL と同じ項目、MeshLod は PASS)。最終ソースの再実行では出ず。
- `tools\check_rules.ps1` → 0 error 50 warning (rule 7 の既存警告。新規ファイルからは無し)。
- `tools\replay_verify.bat` → `[PASS]` 18 ジョブ全部 (Debug/Release の 13 シーン + snapshot + time travel + rule)。kCookVersion 6 でも cook 有無のビット一致 (M51b) は保たれた。
- `tools\shot_verify.bat` → 2 回 (中間と最終)、30 枚全部 PASS (exit 0)。golden は 1 枚も更新していない。
- render_bench (`Runtime.exe --render-bench-demo --deferred --no-audio --font-embedded --width 960 --height 540 --shot-frame 30 --render-stats-dump <json> --screenshot <png>`、Release 実 GPU):
  - LOD 自動: drawCalls 23 / tris 1335104 / shadowTris 369800 / lodDraws [3364, 2, 2, 10] / lodTris [1324192, 3968, 1984, 4960]。
  - `--lod-force 0` (OFF): drawCalls 18 / tris 1379744 / shadowTris 396584 / lodDraws [3378, 0, 0, 0]。→ tris −44640 (−3.2%)、影 tris −26784 (−6.8%)。グリッド 3364 個は LOD を持たないので全体では小さい差。LOD 球だけなら 16 個で 3968×16 = 63488 → 12000 台。
  - `--lod-force 3`: tris 1324192、lodDraws [3362, 0, 0, 16] (段のあるメッシュ 16 個が全部最も粗い段)。
  - 近景 (14 m) 2 個は LOD0、34 m が LOD1、43 m が LOD2、65 m と遠景 8 個が LOD3 (見本の位置で意図どおり)。
  - 決定的な数 (counts 全欄) は Release / Debug / Debug `--warp` / Release `--warp` の 4 構成で完全一致。
  - Forward (`--deferred` なし) でも同じ lod 分布。オクルージョン ON/OFF (`--no-occlusion`) の `--img-diff --tol 0` は Deferred / Forward とも diffPixels=0 (LOD の indirect 経路が画素一致)。LOD 自動 vs 強制 LOD0 は 6942 画素が違う (LOD 球だけ、maxDiff 114)。
  - スクショ (目視はユーザー): `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\89e8c94f-4cec-4f3f-b7ac-0b1d093ded4d\scratchpad\` の `lod_on.png` / `lod_off.png` / `lod_all3.png` (正面)、`cut_on.png` / `cut_off.png` / `cut_all3.png` (`--render-bench-cut-frame 10` の斜めの視点、遠景の LOD3 の球が右上に見える)。
- 実アセットの生成 (selftest のログ): `Player_Researcher_FP.fbx` の 23 メッシュ (428〜812 三角形) と `CesiumMan.glb` (スキン、4672 三角形) が 2 段ずつできる。**`Lab_Door.fbx` (硬い面の箱状) と `skinned_beam.fbx` は段が作れない** (例: 3420 index が 2700 までしか減らない / 216 → 216)。ログに届いた数と目標が出る。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜9 を満たした。減点: `lodBias` の向きを仕様の字面から変えた、アセットブラウザの右クリックには出していない。
  正しさ: 4 — 4 構成で決定的な数が一致、オクルージョン ON/OFF とビット一致、golden 30 枚・replay_verify 全 PASS、フレッシュパース / キャッシュ / 再クックのバイト一致を selftest で固定。減点: 実 GPU の見た目 (LOD 切り替えのポップ、法線・UV の崩れ) は未目視、硬い面のメッシュは段が作れない。
  コード品質: 4 — 純関数の選択、ローダ共通の登録地点 1 本、`Mesh::indices` は LOD0 のまま。減点: ShadowPass / ShadowAtlas / Forward / Deferred に同じ `LodRange` 取得が点在、`RenderSystem` のステージ 2 に式が直書き (sub-06 で並列化の整理に合わせて関数へ出せる)。
  テスト: 4 — 選択の境界値、生成の決定性と単調減少、blob 往復と切り詰め、`.meta` 変更での再クック (段数・比・0 段)、フレッシュ / キャッシュ一致、コライダーの LOD0、GPU の IB 連結と復旧、実 FBX / glb。減点: 描画の中身 (indirect 引数の startIndex) は画素一致の間接確認のみ、`ShadowPass` の段ありの run を直接見るテストが無い。

不安・質問:
- (1) `lodBias` の向き (差分 1 件目)。字面どおり (閾値 × bias、> 1 で粗い) に直すなら `SelectLod` の `size = screenSize / lodBias` の 1 行。どちらにするか判断してほしい。
- (2) 硬い面のメッシュ (平らな面と鋭い辺だけの箱・パネル) は、属性の継ぎ目と境界を動かさない既定 (`LockBorder`、非 Permissive) では 0.5 まで減らず段が作れない。`meshopt_SimplifyPermissive` を使えば継ぎ目をまたいで潰せるが、法線・UV が崩れうるので入れていない。三校の素材 (research_wing / underground_lab) に段を付ける運用を想定するなら、比を 0.8 など緩くする / Permissive をオプトインの設定に足す、のどちらかが要る。v1 の範囲外として申し送りにしてよいか。
- (3) Debug selftest の `ServerNetSelfTest` 一過性 FAIL を今回も 1 回見た (3 回目、台帳に記録済みの 2 回に続く)。最終ソースの再実行では出ず。sub-08 前の基点コミットでの切り分けが要る。
- (4) 公式ドキュメントでの裏取り (spec §7 の指示): Unity / UE の LOD 仕様は、この環境の Web 取得で github の API のみ確認でき、公式ドキュメントは未確認。方式は spec の「アセット単位のオプトイン・段ごとの screen-size」のまま実装した (記憶による照合)。食い違いは見つけていない。

触ったファイル:
- 新規: `external\meshoptimizer\` (meshoptimizer.h, allocator.cpp, indexgenerator.cpp, simplifier.cpp, LICENSE.md)、`src\Engine\Engine\Asset\MeshLodBuilder.h/.cpp`、`src\Engine\Engine\Asset\MeshLodSelfTest.h/.cpp`、`src\Engine\Renderer\Mesh\MeshLod.h`、`tools\gen_lod_test_gltf.ps1`、`assets\models\lod_sphere.glb`、`assets\models\lod_sphere.glb.meta`
- 変更: `external\VERSIONS.md`、`tools\gen_project_files.ps1`、`build\Engine.vcxproj` (手書きの external 3 本 + 生成の src 一覧)、`build\Engine.vcxproj.filters` (生成)、`src\Engine\Core\Asset\ImportMetaResolver.h/.cpp`、`src\Engine\Core\Diagnostics\Profiler.h/.cpp`、`src\Engine\Core\Localization\LocalizationTable.inl`、`src\Engine\Engine\App\EngineCli.cpp`、`src\Engine\Engine\App\EngineCliSelfTest.cpp`、`src\Engine\Engine\Asset\AssetDatabase.h/.cpp`、`src\Engine\Engine\Asset\CookedCache.h`、`src\Engine\Engine\Asset\FbxLoader.cpp`、`src\Engine\Engine\Asset\ModelCook.h/.cpp`、`src\Engine\Engine\Asset\ModelLoader.cpp`、`src\Engine\Engine\Demo\DemoContent.cpp`、`src\Engine\Engine\Loop\EngineLoop.h/.cpp`、`src\Engine\Engine\Rendering\RenderStatsDump.cpp`、`src\Engine\Engine\Rendering\RenderStatsSelfTest.cpp`、`src\Engine\Engine\Rendering\RenderSystem.h/.cpp`、`src\Engine\Renderer\Device\GpuResources.h/.cpp`、`src\Engine\Renderer\Mesh\MeshInstancing.h`、`src\Engine\Renderer\Passes\ShadowAtlas.cpp`、`src\Engine\Renderer\Passes\ShadowPass.cpp`、`src\Engine\Renderer\Pipeline\DeferredPath.cpp`、`src\Engine\Renderer\Pipeline\ForwardPath.cpp`、`src\Engine\Renderer\Pipeline\RenderQueue.cpp`、`src\Engine\Renderer\Pipeline\RenderTypes.h`、`src\Editor\App\EditorApp.cpp`、`src\Editor\App\EditorMain.cpp`、`src\Editor\Windows\Debug\ProfilerWindow.cpp`、`src\Editor\Windows\Scene\InspectorWindow.h/.cpp`

申し送り:
- sub-06 へ: LOD の段選択は `RenderSystem::CollectDrawables` のステージ 2 (並列、履歴は読むだけ) に入っていて、履歴の書き込みはステージ 3 (直列)。パレット評価を並列化するときもこの分離を保つこと。URO の「画面に占める大きさ」はここで出している外接球の screen-size を再利用できる (式は `LodScreenSize`)。スキンの LOD 判定にはバインドポーズの AABB を使っている (保守的 AABB は 2.4〜2.9 倍に緩く、段が早く落ちるのを避けるため)。
- sub-08 へ: ADR-029 に書くこと — `.meta` の `"lod"` 形式、blob の並び (先頭に `ModelLodSettings` 28 バイト、メッシュごとに `lodIndices` / `lods`)、段の選択式 (外接球の screen-size × lodBias とヒステリシス 10%)、頂点溶接 + `simplifyWithAttributes` + `LockBorder`、硬い面は段が作れないこと (上の不安 2)、新 CLI `--lod-bias` / `--lod-force`、`tools\gen_lod_test_gltf.ps1`。engine_spec の kCookVersion を 6 へ。
- `meshoptimizer` は `Common.props` の include パス (`$(RepoRoot)external`) 経由で `#include "meshoptimizer/meshoptimizer.h"`。ソースは vcxproj に手書きなので、バージョンを上げるときは `external\VERSIONS.md` と 3 つの .cpp の差し替え。
- render_bench の `cache\render_bench.scene.json` が残っているとシーンを組まずに読む。LOD 球を足したので、古いキャッシュが手元にあれば消すこと (この作業環境には無かった)。
- 作業ツリーの `harness.md` / `sub-04.md` のハッシュ記入だけの未コミット差分は触っていない (触ったファイルにも入れていない)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-09)
  - 受け入れ 1〜8 の根拠と検証欄が揃っている: MeshLod selftest 85 件 PASS、両構成の selftest exit 0、replay_verify 18 ジョブ PASS (kCookVersion 6 でも M51b のビット一致を保つ)、shot_verify 30 枚 PASS (golden の更新なし)、render_bench の 4 構成で counts が一致、LOD 自動 / 強制 0 / 強制 3 の dump、オクルージョン ON/OFF の画素一致 (Deferred / Forward)。
  - 差分の判定: lodBias の向きは spec の中の食い違いなので spec を直した (§8)。UI を Inspector だけにした件、既定値、blob 先頭の LOD 設定、CLI・統計・テスト素材の追加は採用した。LodHistory をシーン切り替えで捨てる処理は sub-06 のやること 8 へ移した。
  - 不安・質問への回答: (1) 今の向き (Unity と同じ) のままでよい。(2) 硬い面のメッシュは v1 の範囲外。spec §7 に記録し、ADR-029 にも書く (sub-08)。(3) ServerNet の一過性 FAIL は M90 と無関係な別件として spec §7 に記録し、sub-08 で基点と比べる。(4) 公式ドキュメントでの裏取りが未実施である旨は spec §7 にあるとおり残す。
  - nit (申し送り): 各パスで `LodRange` の取得が点在している。indirect 引数の startIndex と、ShadowPass の段ありの run は、画素一致による間接確認しかしていない。LOD の切り替えのポップと、法線・UV の見た目はユーザーの目視待ち (scratchpad の lod_on / lod_off / lod_all3 / cut_*.png)。
