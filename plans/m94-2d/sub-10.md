# sub-10: タイルパレット / タイルマップ資産、`Tilemap`、チャンク描画

- 依存: sub-04
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #7, #28、4.1.7、4.2.1、4.2.3

## やること
1. `.tilepalette.json` / `TilePaletteLibrary` と `.tilemap.json` / `TilemapLibrary` (spec 4.1.7 の形式。GUID キー、`PhysMatLibrary` の流儀、`AssetType` append ×2、`ClassifyPath` 最長サフィックス、`RegisterAssetLibraries`、ReloadHub (tilemap は「全チャンク dirty」を通知する口を持つ)、Project 窓 Create ×2、`kTypeFilters` / `TileLabel` / `FileTypeIcon`、Inspector の簡易表示 (サイズ・レイヤー数・チャンク数))。`rule` / `anim` のフィールドも**今読み書きする** (使うのは sub-13)。
2. `TilemapComponent` (hash 対象、TypeId 末尾): `tilemap / layer / mask / friction / physMaterial / showCollision (kFieldNoHash)`。登録表、Catalog (2D)、Create → 2D → Tilemap。`kSimSnapshotVersion` +1。
3. Engine 層 `src\Engine\Engine\Tilemap\TilemapSystem.{h,cpp}`: アセットとエンティティから「実効セル」を返す `GetCell(e, layer, cx, cy)` (override 表は sub-11 で足す。今は無条件にアセットの値)、チャンク列挙、座標変換 `WorldToCell / CellToWorld` (原点 = エンティティのワールド位置、スケール無視)。
4. 描画: `Sprite2DRenderer` の収集に Tilemap を足す。チャンクごとに頂点列 (セルごとに 1 クアッド、`tileId → palette → sprite → SpriteDef`、flip / 回転ビット、`destroyed` 等は無し) をキャッシュし、dirty (アセットのリロード、初回、sub-11 の override 変更、sub-13 のアニメコマ変化) のときだけ焼き直す。レイヤーの `sortingLayerId / orderInLayer` でキーを作り、同じキーの Sprite2D と並ぶ (タイブレークは entity.index → レイヤー index → チャンク)。カメラの正射影範囲外のチャンクは出さない (AABB で切る。透視のときは切らない)。
5. Scene View ギズモ: Tilemap のグリッド範囲 (使用チャンクの AABB、灰) と、2D モードでセルの格子 (薄い灰、カメラ範囲内のみ)。
6. selftest `TilemapSelfTest` (新規): (a) 2 レイヤー 3 チャンクの往復で同一 JSON、空チャンクは書かれない (b) `GetCell` / 座標変換 (c) パレットの id 割り当て。
7. 確認スクショ: 2 レイヤー (地面 / 前景) + Sprite2D 1 枚 (地面と前景の間の order) → 層順で写る、flip / 回転の 4 変種が見える。

## やらないこと (このサブでは)
- 衝突・override・破壊 (sub-11)、パレット窓 (sub-12)、ルール / アニメの評価と UI (sub-13)

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Tilemap\TilemapAssets.{h,cpp}` (2 ライブラリ)、`TilemapSystem.{h,cpp}`、`TilemapSelfTest.{h,cpp}`
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp` (収集にタイル経路)
- `AssetDatabase.{h,cpp}`、`DemoContent.cpp:3186-3269`、`ReloadHub.{h,cpp}`、`AssetBrowserWindow.cpp`、`AssetOps.{h,cpp}`、`EditorWidgets.cpp`、`InspectorWindow.cpp`
- `SceneViewWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`EditorMain.cpp`、`LocalizationTable.inl`、vcxproj

## 受け入れ条件 (このサブ)
1. (spec 20) スクショ `plans\m94-2d\screenshots\tilemap_basic.png` で層順と flip / 回転が期待どおり (画素座標のログ)。selftest (a)(b)(c) PASS。
2. 100×50×3 層のタイルを temp で作り、`--frames 300` の平均フレーム時間を Release / WARP でログ (spec 4.4 の 16 ms 予算の内訳。超えても今は REWORK にしないが記録する)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `TilemapSelfTest` を足して PASS、snapshot +1 のみ。
4. **チェックポイント (sub-06〜10 の分、spec 4-5)**: `Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`tools\replay_verify.bat` 全ジョブ PASS、`tools\shot_verify.bat` + 既存 golden 30 枚 `--tol 0` で maxDiff=0。割れたら sub-06〜10 のどれが原因かを特定して SELF_EVAL に書く。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --screenshot` と `--frames 300`
**チェックポイント (このサブで回す)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat`、`tools\shot_verify.bat` + 既存 30 枚 `--tol 0`。replay_verify は全文ログを `cache\m94\replay_verify_cp2.log` に残し、ジョブごとの PASS 行を SELF_EVAL に写す (sub-05 VERDICT should 3)。`demo_terrain_deferred` は golden でなく基点 `25fb355` の絵と `--tol 0` で比べる (spec 5. 条件 4 の例外)

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-10 (round 1)
実装:
  - src/Engine/Engine/Tilemap/TilemapAssets.{h,cpp}: TilePalette / TilePaletteLibrary / TilemapAsset / TilemapLibrary (GUID キー、Register のたびに Epoch が増える)、tilecell:: (セルのビット)、ChunkKey (cy, cx)、床除算、tilepalette:: / tilemap:: の注入口、ToJson / FromJson / Sanitize
  - src/Engine/Engine/Tilemap/TilemapSystem.{h,cpp}: AssetOf / OriginOf / GetCell / GetChunkCells / EnumerateChunks / WorldToCell / CellToWorld (セル中心) / MarkAllDirty / DirtyEpoch (静的関数のみ。override 表は sub-11)
  - src/Engine/Engine/Tilemap/TilemapSelfTest.{h,cpp}: (a) 往復・空チャンク (b) GetCell・座標変換 (c) パレット id・Sanitize (d) flip / 回転 UV (e) 正射影の視錐台 AABB
  - Components.{h,cpp}: TilemapComponent (TypeId 84、hash 対象、showCollision だけ kFieldNoHash)。SimSnapshot.h v56、AcousticAudioSelfTest の版照合を 56 へ
  - Sprite2DRenderer.{h,cpp}: Collect を CollectSprites / CollectMasks / CollectTilemaps に割り (planner nit)、Item に isTile + (tileLayer, tileChunk) のタイブレーク、BakeTileChunk (チャンク焼き) / EmitTileChunk (キャッシュ + 原点加算)、TileCornerUv、OrthoWorldBounds (public static)。RenderSystem は view / proj を渡す
  - AssetDatabase / SimInit / EngineLoop / HeadlessSim / DemoContent (RegisterAssetLibraries) / ReloadHub (ReloadTilePalette / ReloadTilemap、MarkAllDirty) / AssetOps (Create ×2、SplitAssetName) / AssetBrowserWindow (Create ×2、kTypeFilters、TileLabel) / EditorWidgets (FileTypeIcon) / EditorComponentCatalog / CreateMenu (2D → Tilemap) / LocalizationTable.inl (両言語) / InspectorWindow (tilemap ピッカー + D&D、Tilemap の注意欄、資産の簡易表示呼び出し) / TileAssetInspector.{h,cpp} (新規、読み取り専用の要約) / SceneViewWindow (DrawTilemapGizmos) / EditorMain (m94 と全体に TilemapSelfTest)
  - NavAgentSelfTest.cpp:52 kExpectedYardHash を 0x8581BEE4A676337C へ再採取 (下の「仕様との差分」と検証を参照)
仕様との差分:
  - [追加] チェックポイントで見つかった既存の赤: NavAgentSelfTest の「庭のワールドハッシュが焼いた期待値と一致」が FAIL。原因は sub-09 (CharacterController の 2D 欄 = hash 対象の append で庭の CC のバイトが増えた)。Tilemap の登録を一時的に kComponentNoHash にして同じ selftest を回しても値は同じ 8581BEE4A676337C (Debug == Release) = sub-10 の型は無関係 (実験コードは戻し済み)。他の NavAgent の項目は全部 PASS (挙動は不変) なので、sub-01 (86dec43) と同じ流儀で定数を焼き直した。sub-09 を差し戻すかは planner 判断
  - [追加] タイルの絵は「パレットのスプライトの矩形をセル全面 (cellSize) へ伸ばす」。spec はセルへの当て方を書いていない。Unity のようにスプライトの原寸 + pivot 配置にすると、16 px の絵を 1 セルに置く標準ケースでは同じで、違うサイズの絵は原寸ではみ出す。最小で可逆な解釈として全面伸縮を採った
  - [追加] flip を先に、回転をその後 (反時計回り、90 度単位)。ジオメトリはセルのまま UV の隅だけを入れ替えるので非正方形セルでもセルを埋める。TileCornerUv の selftest と実撮影 (7 変種 x 4 隅) で固定
  - [追加] セル配列の行 = ローカル y (行 0 が下)、チャンクは (cy, cx) 昇順で書く。spec は行の向きを書いていない。座標系 (y 上) と同じ向きにした
  - [追加] 予約 bit20-23 は読み書きでそのまま保つ。FromJson は種別キー ("tilemap" / "tilepalette") 無しを false にする (shape2d と同じ)
  - [追加] CellToWorld はセル中心 (z = 原点の z) を返す。spec は「CellToWorld」とだけ。sub-14 の ABI で左下角が要るなら足す
  - [追加] Collect に view / proj を引数で足した (既定 nullptr = 切らない)。spec は「カメラの正射影範囲外のチャンクは出さない」だけで、収集がカメラを知る口が無かった。正射影は proj._34 == 0 && proj._44 == 1 で判定し、透視は切らない
  - [追加] 非同期ロード中のスプライトを含むチャンクは次のフレームも焼き直す (complete = false)。SpriteLibrary には版が無いので、スプライトエディタの Apply 後は Tilemap / TilePalette の再登録か TilemapSystem::MarkAllDirty まで古い UV のまま
  - [未実装] 衝突 / override / 破壊 (sub-11)、パレット窓 (sub-12)、rule / anim の評価 (sub-13)。rule / anim は読み書きのみ。showCollision の描画は sub-11
  - [未実装] SceneView ギズモ (使用チャンクの AABB / 2D モードのセル格子) の実画面確認。コードとビルドだけ (spec 受け入れ条件に画素確認は無い)
検証:
  - msbuild MyEngine.sln Debug / Release /p:MyeWarnAsError=true -> 0 警告 0 エラー (Debug の最初の 1 回は既知の SelfTestUnused.kernel.hlsl C4100、同じコマンドの再実行で通る)。ログ cache\m94\s10\build_*.log
  - pwsh tools\gen_project_files.ps1 -> 実行 (Editor / Engine の vcxproj + filters に Tilemap 6 ファイル + TileAssetInspector 2 ファイル)
  - pwsh tools\check_rules.ps1 -> 3 error / 50 warning (着手前 3 / 50 と同数・同内容 = EditorMain の #ifdef _DEBUG と engine_dist .bat 2 本の LF)。replay_verify の rules ジョブも同じ
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest (Debug / Release) -> 両方 exit 0。Tilemap self test ALL PASS (両構成同じ項目)、AssetOps / AssetDatabase / ReloadHub の追加行 PASS
  - 撮影 (Release Runtime、temp シーン = assets\_tmp_m94j の PNG 4 枚 + palette + tilemap + scene。撮影後に除去): plans\m94-2d\screenshots\tilemap_basic.png (960x540、64 px/unit、tonemapMode 0)。自前の PNG デコーダで 35 項目 PASS (cache\m94\s10\tilemap_basic_pixels.log)
      * 4 隅 [左上, 右上, 左下, 右下] が (赤, 緑, 青, 黄) の 16x16 を 7 変種: 無変換 RGBY / flipX GRYB / flipY BYRG / 90 度 GYRB / 180 度 YBGR / 270 度 BRYG / flipX+90 度 RBGY。全 28 隅が期待の色 (±1)
      * 層順: 地面のみ (3.5,-1.5) = (128,128,128)、Sprite2D (order 5) が地面の上 (4.5,-1.5) = (255,128,0)、前景 (order 10) が Sprite2D の上 (5.5,-1.5) = (60,120,255)、前景のみ (6.5,-1.5) = (60,120,255)。背景 (2,2,3)
  - 受け入れ 2 (100x50 x 3 層 = 15000 セル、Release / WARP / 960x540、--frames 300 --no-vsync、--render-stats-dump の gpuMs.frame。cache\m94\s10\stats_*.json)
      * 全チャンクが画面内 (orthoSize 28.2): 8.6〜9.6 ms (2 回)。空シーン (カメラだけ) は 2.3〜2.8 ms = タイルの GPU 増分 約 6.5 ms
      * 画面内が 15 x 8 セルだけ (orthoSize 4.2、カメラ (50,25)): 6.1〜7.0 ms (3 層が全面を塗る = WARP のフィル負荷。正射影のチャンク切りで頂点は 1/20 以下)
      * wall time (起動 6.1 s 込み): 300 フレーム 空 6.07〜6.20 s / 全面 6.49〜6.58 s / 一部 6.27〜6.29 s。CPU 側の増分は全面で 1 フレーム約 1.1 ms (原点を足した頂点コピーが主)。16 ms 予算 (spec 4.4) の内訳: タイル全面 約 9 ms + 他。Sprite2D 2000 枚は sub-23 で足して再測する
      * 切りの確認: orthoSize 4.2 のスクリーンショットは四隅まで前景タイルで塗られる (切りすぎ無し)。OrthoWorldBounds は selftest で数値固定
  - 【チェックポイント 2 (sub-06〜10)】
      * Editor.exe --selftest 全体 (Debug / Release 並走) -> 両方 exit 0。1 回目は NavAgent の庭ハッシュ 1 件が FAIL (上記、sub-09 起因)、再採取後の 2 回目は FAIL 0 (ログ cache\m94\s10\full2_*.log)
      * tools\replay_verify.bat (MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true、全文 cache\m94\replay_verify_cp2.log) -> 20 ジョブ PASS + rules だけ FAIL。rules の赤は check_rules の既存 3 error (M94 外) のみ。ジョブごとの行:
          mp PASS / physics PASS / demo PASS / flow PASS / acoustic PASS / parts PASS / fracture PASS / joints PASS / ui PASS / nav PASS / perception PASS / anim PASS / bt PASS / ttrelease PASS / ttdebug PASS / whatifrelease PASS / compute PASS / whatifdebug PASS / computeab PASS / jobsab PASS / rules FAIL (既存 3 error)
      * tools\shot_verify.bat (Release、30 枚、cache\m94\s10\shot_verify.log) -> [PASS]
      * golden 30 枚を Editor.exe --img-diff --tol 0 で照合 (cache\m94\s10\golden_tol0.log): 29 枚 maxDiff=0 diffPixels=0。demo_terrain_deferred だけ golden と maxDiff=5 / 2 画素 (基点でも同じ、spec 5. 条件 4 の例外)。基点 25fb355 を git archive -> Release Runtime をビルドして同じ条件で撮った絵 (cache\m94\baseline_terrain.png) と今回の actual を --tol 0 -> PASS maxDiff=0 diffPixels=0
      * 割れた箇所は NavAgent の庭ハッシュ 1 件だけで、原因は sub-09。sub-06 / 07 / 08 / 10 に起因する割れは無し。replay の不一致 (hash-diff を要する構造変化) は無かった
  - 版: SimSnapshot v55 -> v56 (Tilemap の新型 = TypeId 84、World 節のカラム生バイト)。ABI / Scene kDocVersion / kCookVersion は触っていない
  - 未実行: Editor の GUI 実操作 (Create → 2D → Tilemap、Project 窓の Create ×2、Inspector のピッカー / D&D、ギズモ) とエディタスクショ。Deferred 経路でのタイル撮影 (Sprite2D と同じ Pass だが未確認)。Debug ビルドでの撮影
自己採点 (1-5):
  仕様適合: 4 — やること 1〜7 と受け入れ 1〜4 を実装・撮影・チェックポイントで確認。セルへの絵の当て方 / 行の向き / CellToWorld の基準は spec に無く解釈で埋めたので 5 にしない。ギズモの実画面確認が無い
  正しさ: 4 — selftest (往復・座標変換・id・UV・視錐台) を Debug / Release で PASS、実撮影で 7 変種 x 4 隅と層順を画素確認、golden 30 枚 tol 0 と replay_verify 20 ジョブ PASS。SpriteLibrary の更新 (スプライトエディタ Apply) を検知しない弱点が残る
  コード品質: 4 — 資産 / 座標 (Engine) / 描画の収集 / Inspector・ギズモ (Editor) の層を分け、Collect を種別ごとの小関数に割った。BakeTileChunk がセル走査 + 解決 + 頂点生成で長め。タイルの頂点は毎フレーム原点を足して複製する (計測上 CPU 約 1 ms)
  テスト: 4 — 資産 / 座標 / UV / 視錐台は selftest、描画は実撮影の画素照合 (一時シーンで自動テスト化していない)、重い 3 本を通した。GUI は未確認
不安・質問:
  - Q1 NavAgentSelfTest の kExpectedYardHash を再採取して直した (sub-09 起因)。sub-09 を差し戻すか、このまま sub-10 のコミットに含めるか (別コミットにするか) は planner 判断
  - Q2 タイルの絵をセル全面へ伸ばす解釈 (spec に無い) でよいか。原寸 + pivot 配置を望むなら BakeTileChunk のクアッド寸法を LocalRect 基準へ変える (sub-13 の rule / anim の前に決めたい)
  - Q3 SpriteLibrary に版が無いので、スプライトエディタの Apply 直後は焼いたチャンクが古い UV のまま。sub-12 (パレット窓) で Apply 後に MarkAllDirty を呼ぶ形でよいか
触ったファイル:
  build/Editor.vcxproj
  build/Editor.vcxproj.filters
  build/Engine.vcxproj
  build/Engine.vcxproj.filters
  plans/m94-2d/screenshots/tilemap_basic.png (新規)
  plans/m94-2d/sub-10.md (実装メモ)
  src/Editor/App/EditorMain.cpp
  src/Editor/Asset/AssetOps.cpp
  src/Editor/Asset/AssetOps.h
  src/Editor/Asset/AssetOpsSelfTest.cpp
  src/Editor/Widgets/CreateMenu.cpp
  src/Editor/Widgets/EditorComponentCatalog.cpp
  src/Editor/Widgets/EditorWidgets.cpp
  src/Editor/Windows/Asset/AssetBrowserWindow.cpp
  src/Editor/Windows/Scene/InspectorWindow.cpp
  src/Editor/Windows/Scene/SceneViewWindow.cpp
  src/Editor/Windows/Scene/SceneViewWindow.h
  src/Editor/Windows/Scene/TileAssetInspector.cpp (新規)
  src/Editor/Windows/Scene/TileAssetInspector.h (新規)
  src/Engine/Core/Ecs/Components.cpp
  src/Engine/Core/Ecs/Components.h
  src/Engine/Core/Localization/LocalizationTable.inl
  src/Engine/Engine/Asset/AssetDatabase.cpp
  src/Engine/Engine/Asset/AssetDatabase.h
  src/Engine/Engine/Asset/AssetDatabaseSelfTest.cpp
  src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp
  src/Engine/Engine/Demo/DemoContent.cpp
  src/Engine/Engine/HotReload/ReloadHub.cpp
  src/Engine/Engine/HotReload/ReloadHub.h
  src/Engine/Engine/HotReload/ReloadHubSelfTest.cpp
  src/Engine/Engine/Loop/EngineLoop.cpp
  src/Engine/Engine/Loop/HeadlessSim.cpp
  src/Engine/Engine/Loop/SimInit.cpp
  src/Engine/Engine/Loop/SimInit.h
  src/Engine/Engine/Navigation/NavAgentSelfTest.cpp
  src/Engine/Engine/Rendering/RenderSystem.cpp
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.cpp
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h
  src/Engine/Engine/Replay/SimSnapshot.h
  src/Engine/Engine/Tilemap/TilemapAssets.cpp / .h (新規)
  src/Engine/Engine/Tilemap/TilemapSystem.cpp / .h (新規)
  src/Engine/Engine/Tilemap/TilemapSelfTest.cpp / .h (新規)
申し送り:
  - sub-11: Tilemap の衝突は TilemapSystem::GetChunkCells / EnumerateChunks にアセットの値が出る。override を足すときは GetCell / GetChunkCells で重ね、EnumerateChunks に override だけのチャンクも含め、書き換え / 復元で MarkAllDirty を呼ぶ (描画側は DirtyEpoch の変化で全チャンクを焼き直す)。showCollision の描画は DrawTilemapGizmos に足す (緑)。静的形状は PhysicsSystem::AddStaticShapeSource (sub-08 の供給口)
  - sub-12: 窓から塗るときは TilemapLayer::SetCell (空になったチャンクは消える) と TilemapLibrary::ToJson / Register。Register で Epoch が増え、描画のキャッシュは自動で焼き直る。SceneView の 2D モードのセル格子は選択中の Tilemap だけ、1 方向 256 本を超える密度では引かない
  - sub-13: パレットの rule / anim は読み書きのみ。評価は BakeTileChunk の「def->sprite を引く」1 箇所に差し込む (隣接は実効セル、アニメは tick から。アニメを含むチャンクの焼き直しは dirtyEpoch か専用キーで)
  - sub-14: C++ 側のセル座標 API は TilemapSystem の静的関数。CellToWorld はセル中心
  - check_rules の基準値は 3 error / 50 warning のまま。replay_verify の rules ジョブは既存 3 error で毎回赤
  - cache\m94\base25fb355 (基点の git archive + Release Runtime、約 0.5 GB) を残してある。次のチェックポイントの demo_terrain_deferred 比較に使える (cache は git 管理外)。cache\m94\baseline_terrain.png が基点の絵
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20、チェックポイント 2 = sub-06〜10)。スクショ `tilemap_basic.png` で 7 変種 × 4 隅と層順を確認、selftest (a)〜(e) 両構成 PASS、性能ログ (15000 セル全面で GPU 約 9 ms、CPU 約 1 ms)。チェックポイント: 全体 selftest 両構成 exit 0、replay_verify 20 ジョブ PASS (rules は既存 3 error)、shot_verify 30 枚 PASS、`--tol 0` 29 枚 0 + `demo_terrain_deferred` は基点の絵と 0。割れたのは NavAgent の庭ハッシュ 1 件 = sub-09 の CC 欄 append (挙動は不変、値だけ) → 再採取を採用し、**sub-09 の追補として別コミット**に切り出す (Q1)。Q2 (セル全面へ伸ばす) 採用、Q3 (SpriteLibrary の版) は sub-12 のやること 7 へ。spec 4.4 に「hash 対象の append は焼いた定数を同じサブで再採取」の規則を追加。
