# sub-04: `Sprite2D` と `Sprite2DPass` (Simple / flip / 整数ソート / Point・Linear)

- 依存: sub-02, sub-03
- 状態: OK (commit は台帳参照)
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #1, #12, #24、4.1.3、4.3、4.4 (レイヤー)

## やること
1. `Sprite2DComponent` (TypeId は着手時の末尾、**kComponentNoHash**。フィールドは spec 4.1.3 の全部を最初から定義する — `drawMode / size / maskInteraction / material / normalMap / lit` は後続サブが使うが、フィールドだけ今置いて後で生バイトを伸ばさない)。登録表、`EditorComponentCatalog.cpp` に「2D」カテゴリと 1 行、`kSimSnapshotVersion` +1。
2. Engine 層 `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}`: World を走査し、active な Sprite2D ごとに `SpriteDef` を解決 (`spritelib::Library()->Get`。nullptr なら内蔵 16×16 マゼンタ。`IsFullTexture()` なら `TextureLibrary` の Texture の width / height を rect にする — sub-03 の契約)、`SortingLayers::Get().Load(assetsRoot)` を EngineLoop の初期化 (`TagNames` の Load と同じ場所) に接続し、ワールド行列 × (rect / ppu、pivot、flip) で 4 頂点を作り、ソートキー (層順 ← `SortingLayers::OrderOf`、orderInLayer、グループキー (sub-06 で埋める、今は 0)、entity.index) で**安定ソート**して頂点列 + バッチ (SRV、filter) を Renderer 層に渡す。viewZ は計算しない。
3. Renderer 層 `src\Engine\Renderer\Passes\Sprite2DPass.{h,cpp}`: 動的 VB (`MAP_WRITE_DISCARD`、1.5 倍成長)、point / linear の 2 サンプラ (自前で作る。VfxRenderer の契約は VfxRenderer 内の話)、ブレンド SRC_ALPHA / INV_SRC_ALPHA、ZTest LEqual / ZWrite off、CULL_NONE、`assets\shaders\sprite2d.hlsl` (b0 = viewProj、t0 = テクスチャ、s0 = サンプラ。ライトバッファ用の t1 / s1 は sub-15 で足すので今は触らない)。呼び出しは `RenderSystem::DrawParticlesAndDebug` (`RenderSystem.cpp:2047`) の `vfx->Render` の直前。SceneView / GameView / Runtime の全部が通る。
4. Inspector: `DrawAssetRef` (`InspectorWindow.cpp:4214-4393`) に "sprite" 分岐 (`SpriteLibrary::Enumerate`)、テクスチャの D&D で全面スプライト。`sortingLayerId` はコンボ (SortingLayers の名前)。
5. Scene View のギズモ: 選択中 Sprite2D の矩形 (白) と pivot (黄の十字、`AddLine` ×2)。2D モードでなくても描く。
6. Create メニュー: Hierarchy の Create → 2D → Sprite (Sprite2D + LocalTransform)。
7. 確認: temp シーンに層 2 種 × order 2 種の 4 枚 (重なる配置)、1 枚 flipX、1 枚 color 赤、1 枚 point filter の 2×2 市松テクスチャ → `--screenshot`。市松の境界画素が 2 値 (にじみ無し)。

## やらないこと (このサブでは)
- Sliced / Tiled、SortingGroup2D、SpriteMask2D (sub-06)、ライト (sub-15)、material (sub-17)

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp` (末尾登録)、`src\Editor\Widgets\EditorComponentCatalog.cpp`
- 新規 `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}`、`src\Engine\Renderer\Passes\Sprite2DPass.{h,cpp}`、`assets\shaders\sprite2d.hlsl`
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` (:2047-2160 DrawParticlesAndDebug、所有と Init)
- `src\Editor\Windows\Inspector\InspectorWindow.cpp:4214-4393`、`src\Editor\Windows\Scene\SceneViewWindow.cpp:725-735` (DrawSpriteGizmos の隣)、Hierarchy の Create メニュー
- `SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`、vcxproj
- 参考: `src\Engine\Engine\Vfx\VfxRenderer.cpp:147-227, 401-421, 480-490` (VB / バッチ / サンプラ切替の前例)、`src\Engine\Engine\UI\UIRenderer.cpp:235-277` (3 段キーの安定ソート)

## 受け入れ条件 (このサブ)
1. (spec 13) 4 枚のスクショで描画順・flip・color・Point が期待どおり (画素値のログを SELF_EVAL に)。スクショは `plans\m94-2d\screenshots\sprite2d_basic.png` に保存。
2. Sprite2D を持たないシーンで Sprite2DPass が Draw を 1 回も出さない (バッチ 0 で早期 return) — コードで示し、`demo_forward` 1 枚だけ `Runtime.exe --screenshot` → `--img-diff --tol 0` で確認 (30 枚全部は sub-05)。
3. (spec 1-3) 0 警告、check_rules 0、snapshot +1 のみ。重い 3 本は sub-05 (NoHash 型の追加は hash 不変)。
4. D3D 型が `src\Engine\Renderer\` の外に出ていない (`Sprite2DRenderer.h` に `ID3D11*` が無い)。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- `Runtime.exe --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --screenshot tests\actual\demo_forward.png` → `Editor.exe --img-diff tests\golden\demo_forward.png tests\actual\demo_forward.png --tol 0`
- `Runtime.exe --scene <temp> --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --screenshot plans\m94-2d\screenshots\sprite2d_basic.png`

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - src/Engine/Core/Ecs/Components.{h,cpp}: Sprite2DComponent (TypeId 81、kComponentNoHash)。spec 4.1.3 の全フィールドを最初から定義 (flipX / flipY / lit は末尾にまとめて padding を最小化)
  - src/Engine/Engine/Replay/SimSnapshot.h / AcousticAudioSelfTest.cpp: kSimSnapshotVersion 52 -> 53 (履歴 v53 を追記、T1 の期待値を追随)
  - src/Engine/Renderer/Passes/Sprite2DDrawList.h (新規): D3D 非依存の頂点 (36B) / バッチ (AssetID + point フラグ) / 描画リスト。収集 (Engine) と描画 (Renderer) の受け渡し
  - src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.{h,cpp} (新規): Collect = active な Sprite2D を走査し SortingLayers::OrderOf / orderInLayer / groupKey(0) / entity.index の整数キーで stable_sort、SpriteDef を解決 (nullptr かファイル無しはマゼンタ 16x16、IsFullTexture は Texture の幅高)、flip は pivot 鏡映、ワールド行列 x ローカル矩形で 4 頂点、バッチ化。LocalRect を static 公開 (描画とギズモが同じ寸法を使う)
  - src/Engine/Renderer/Passes/Sprite2DPass.{h,cpp} (新規): 動的 VB (MAP_WRITE_DISCARD、1.5 倍 + 512)、point / linear 2 サンプラ、SRC_ALPHA / INV_SRC_ALPHA、ZTest LEqual・ZWrite off、CULL_NONE。list が空なら D3D の状態を触らず return 0
  - assets/shaders/sprite2d.hlsl (+ .meta): b0 = viewProj、t0、s0。ライト用 t1 / s1 は触っていない
  - src/Engine/Engine/Rendering/RenderSystem.{h,cpp}: sprite2dRenderer_ / sprite2dPass_ を所有 (遅延 Init、ReleaseGpu でリセット)。DrawParticlesAndDebug の vfx->Render の直前で Collect -> 空でなければ Init / Render。SceneView / GameView / Runtime 全部が通る
  - src/Engine/Engine/Loop/EngineLoop.cpp: InitSimProjectState の直後に SortingLayers::Get().Load(assetsRoot) を接続 (EngineLoop 側のみ。HeadlessSim / Server は描画しないので共有部へは置かない)
  - src/Engine/Renderer/Device/GpuResources.h: TextureLibrary::IsPending(AssetID) を追加 (非同期ロード中の白 1x1 プレースホルダで寸法を決めないため)
  - src/Editor/Widgets/EditorComponentCatalog.cpp: カテゴリ "2D" (ラベル / 色 / 並び) と Sprite2D の 1 行
  - src/Editor/Windows/Scene/InspectorWindow.cpp: DrawAssetRef に "sprite" 分岐 (SpriteLibrary::Enumerate)、テクスチャ D&D は全面スプライト (SpriteLibrary::Register + TextureLibrary::IdForFile)、未解決なら警告行。DrawField に Sprite2D.sortingLayerId のコンボ (表に無い id は「未定義の id」項目で保持)
  - src/Editor/Windows/Scene/SceneViewWindow.{h,cpp}: DrawSprite2DGizmos (選択中の矩形 = 白、pivot = 黄の十字 AddLine x2、2D モードでなくても描く。flip 反映)
  - src/Editor/Widgets/CreateMenu.{h,cpp}: Create -> 2D -> Sprite (Sprite2D + LocalTransform)
  - src/Engine/Core/Localization/LocalizationTable.inl: Create_2D / Create_Sprite2D / Insp_SortingLayerUndefined / Insp_Sprite2DUnresolved (英日)
  - src/Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.cpp: 節 (e) = TypeId 81 / NoHash / LocalRect の 2 ケース
  - build/Engine.vcxproj(.filters): gen_project_files.ps1 の出力
仕様との差分:
  - [追加] src/Engine/Renderer/Passes/Sprite2DDrawList.h — Renderer 層が Engine 層へ逆依存せず、かつ Sprite2DRenderer.h に D3D 型を出さないための受け渡し用 POD (spec のファイル一覧に無い)
  - [追加] TextureLibrary::IsPending — 非同期ロード中のテクスチャはプレースホルダ (1x1) を返すので、全面スプライトの寸法が嘘になる。ロード中は描かない
  - [追加] 未ロードのテクスチャは Sprite2DRenderer が assetguid::ResolvePath -> RequestLoadFileAsync で頼む (sub-03 の「GPU ロードは参照側」の契約どおり)。パスが引けないテクスチャはマゼンタ
  - [追加] 内蔵マゼンタは ppu 16 (1 ワールド単位角) と置いた — 既定 100 では 0.16 単位で見えないため
  - [追加] 未解決スプライトの警告 (Inspector の "sprite" 欄の下、空参照も対象) と sortingLayerId の未定義 id 項目 — 仕様の「警告」を最小で実装
  - [追加] "2D" カテゴリの色 (薄紅) — 既存カテゴリと見分けるため
  - [未実装] SortingGroup の groupKey は常に 0 (sub-06)。SpriteMask / Sliced / Tiled / ライト / material は未着手 (仕様どおり)
検証:
  - msbuild Release / Debug (MyEngine.sln、/clp:ErrorsOnly;WarningsOnly) -> 0 警告 0 エラー (C4100 の既知癖は今回は出ず)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest (Release / Debug) -> Sprite2DAsset ALL PASS (新規 4 項目を含む)、UI 他も PASS
  - Runtime.exe --warp ... --no-fxaa --screenshot tests\actual\demo_forward.png -> Editor.exe --img-diff tests\golden\demo_forward.png ... --tol 0 -> PASS maxDiff=0 diffPixels=0 (受け入れ 2。コード上も Collect が空なら Pass は Init もしない)
  - 一時シーン (assets\_tmp_m94d に 2x2 市松 point / 2x2 市松 linear / 2x2 4 色 / 1x1 白、project_settings に層 Front を一時追加、撮影後に全て原状復帰・削除) を Runtime.exe --scene ... --no-fxaa --screenshot plans\m94-2d\screenshots\sprite2d_basic.png (Release)
    - 市松 point 領域 (180x180 画素全部) の distinct 色は 2 値のみ: (231,231,231) と (0,0,0)。境界: row y=90 の x=657..663 = 231,231,231,0,0,0,0 / col x=610 の y=131..137 = 231,231,231,231,0,0,0 (にじみ無し)。同じ絵の linear 版は 162 値
    - 描画順 (Default / Front の 2 層 x order): 作成順 E1 (Front,0,緑) E2 (Default,5,赤) E3 (Default,1,青) E4 (Front,-3,黄) -> 重なりの画素は緑 (0,231,0) が最上、黄 (231,231,0) が赤・青の上、赤 (231,0,0) が青 (0,0,231) の上 = 期待どおり (層 -> order の整数キー)
    - flipX: 4 色 R,G / B,Y の通常 = (231,0,0),(0,231,0),(0,0,231),(231,231,0)、flipX = (0,231,0),(231,0,0),(231,231,0),(0,0,231) = 左右反転
    - color 赤: 白 1x1 に (1,0,0,1) -> (231,0,0)。FXAA 有りの初回撮影でも境界行は 2 値だった (外周と十字の交点だけ FXAA の中間値)
  - pwsh tools\check_rules.ps1 -> 3 error (既存: EditorMain.cpp の #ifdef _DEBUG、Editor.bat / Setup.bat の LF) / 50 warning。変更ファイル由来の新しい error / warning は目視で無し (InspectorWindow / GpuResources の rule 7 は既存の unordered 走査で、今回の差分に unordered 走査は無い)。着手前の件数との直接比較は未実施
  - pwsh tools\gen_project_files.ps1 -> 実行済み (build\Engine.vcxproj / .filters が変化)
  - replay_verify / shot_verify / golden 30 枚 / 全体 selftest -> 未実行 (ユーザー判断 9 で sub-05 へ)
  - Editor の GUI (Inspector の sprite ピッカー・D&D・sortingLayerId コンボ・警告行、Scene View の矩形 / pivot ギズモ、Create -> 2D -> Sprite、2D カテゴリのアイコン) -> 未確認 (コンパイルのみ。スクショ無し)
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1-4 を満たす。GUI 部 (Inspector / ギズモ / Create) は未確認のため 5 にしない
  正しさ: 4 — 描画順 / flip / color / point の画素値を実撮影で確認、demo_forward は tol 0 一致。非同期ロード中・デバイス復旧・複数ビューの実動作は未検証
  コード品質: 4 — 収集 (Engine) と描画 (Renderer) の分離、LocalRect をギズモと共有。IsPending の追加は仕様外の小変更
  テスト: 3 — selftest は TypeId / NoHash / LocalRect の純関数まで。描画結果は一時シーンの撮影で確認しただけで回帰テスト (golden) には未登録 (sub-23 の 2d_* で)
不安・質問:
  - 全面スプライトのテクスチャは .meta の srgb が auto のとき RequestLoadFileAsync により UNORM でロードされる (既存の VFX スプライトと同じ)。2D の色味は sub-23 の golden で確定する想定でよいか
  - Sprite2D.normalMap / material の Inspector ピッカーは既存の汎用分岐 (総当たり / MaterialLibrary) のまま。sub-16 / sub-17 で専用分岐を入れる前提
触ったファイル:
  src/Engine/Core/Ecs/Components.h, src/Engine/Core/Ecs/Components.cpp, src/Engine/Core/Localization/LocalizationTable.inl,
  src/Engine/Engine/Replay/SimSnapshot.h, src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp,
  src/Engine/Renderer/Passes/Sprite2DDrawList.h, src/Engine/Renderer/Passes/Sprite2DPass.h, src/Engine/Renderer/Passes/Sprite2DPass.cpp,
  src/Engine/Renderer/Device/GpuResources.h,
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h, src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.cpp, src/Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.cpp,
  src/Engine/Engine/Rendering/RenderSystem.h, src/Engine/Engine/Rendering/RenderSystem.cpp, src/Engine/Engine/Loop/EngineLoop.cpp,
  src/Editor/Widgets/EditorComponentCatalog.cpp, src/Editor/Widgets/CreateMenu.h, src/Editor/Widgets/CreateMenu.cpp,
  src/Editor/Windows/Scene/InspectorWindow.cpp, src/Editor/Windows/Scene/SceneViewWindow.h, src/Editor/Windows/Scene/SceneViewWindow.cpp,
  assets/shaders/sprite2d.hlsl, assets/shaders/sprite2d.hlsl.meta,
  build/Engine.vcxproj, build/Engine.vcxproj.filters,
  plans/m94-2d/screenshots/sprite2d_basic.png, plans/m94-2d/sub-04.md (実装メモ節のみ)
申し送り:
  - sub-06: Sprite2DRenderer の Item.groupKey (今は 0) へ SortingGroup2D を入れる。Emit は Simple 前提で drawMode / maskInteraction を読んでいない。LocalRect が Simple の寸法の正本
  - sub-15: sprite2d.hlsl の t1 / s1 は空き。Sprite2DPass::Render はバッチごとに SRV t0 だけを張り、最後に剥がす
  - 一時シーン生成スクリプトは scratchpad の gen.py (assets\_tmp_m94d は削除済み。project_settings.json は git diff で原状復帰を確認済み)
  - 未ロードのテクスチャは最初の 1 フレームは描かれない (非同期ロード)。--shot 系は決定的撮影が drain するので影響なし

### round 1 SELF_EVAL (全文、司会が転記)

```
SELF_EVAL: sub-04 (round 1)
実装:
  - Components.h/.cpp: Sprite2DComponent (TypeId 81、kComponentNoHash)。spec 4.1.3 の全フィールドを最初から定義
  - SimSnapshot.h / AcousticAudioSelfTest.cpp: kSimSnapshotVersion 52 -> 53
  - Renderer/Passes/Sprite2DDrawList.h (新規): D3D 非依存の頂点 (36B)・バッチ (AssetID + point フラグ)・描画リスト
  - Rendering/Sprite2D/Sprite2DRenderer.{h,cpp} (新規): Collect = active な Sprite2D を走査 -> (SortingLayers::OrderOf, orderInLayer, groupKey=0, entity.index) で stable_sort -> SpriteDef 解決 (nullptr かファイル無しは内蔵マゼンタ 16x16、IsFullTexture は Texture の幅高) -> flip は pivot 鏡映、ワールド行列 x ローカル矩形で 4 頂点 -> バッチ化。LocalRect を static 公開
  - Renderer/Passes/Sprite2DPass.{h,cpp} (新規): 動的 VB (MAP_WRITE_DISCARD、1.5 倍 + 512)、point/linear の 2 サンプラ、SRC_ALPHA/INV_SRC_ALPHA、ZTest LEqual・ZWrite off、CULL_NONE。list が空なら D3D 状態に触れず return 0
  - assets/shaders/sprite2d.hlsl (+ .meta): b0 = viewProj、t0、s0。t1/s1 は未使用
  - RenderSystem.{h,cpp}: sprite2dRenderer_ / sprite2dPass_ を所有 (遅延 Init、ReleaseGpu でリセット)。DrawParticlesAndDebug の vfx->Render 直前で Collect -> 空でなければ Init/Render
  - EngineLoop.cpp: InitSimProjectState 直後に SortingLayers::Get().Load(assetsRoot) を接続
  - GpuResources.h: TextureLibrary::IsPending(AssetID) 追加
  - EditorComponentCatalog.cpp: カテゴリ "2D" と Sprite2D 1 行。InspectorWindow.cpp: DrawAssetRef の "sprite" 分岐、テクスチャ D&D = 全面、未解決の警告行、sortingLayerId コンボ。SceneViewWindow: DrawSprite2DGizmos (矩形=白、pivot=黄の十字)。CreateMenu: Create -> 2D -> Sprite
  - LocalizationTable.inl: 4 本 (英日)。Sprite2DAssetSelfTest.cpp: 節 (e) = TypeId 81 / NoHash / LocalRect 2 ケース
仕様との差分:
  - [追加] Renderer/Passes/Sprite2DDrawList.h — Renderer が Engine へ逆依存せず、Sprite2DRenderer.h に D3D 型を出さないための POD
  - [追加] TextureLibrary::IsPending — 非同期ロード中は白 1x1 のプレースホルダなので全面の寸法が嘘になる。ロード中は描かない
  - [追加] 未ロードのテクスチャは assetguid::ResolvePath -> RequestLoadFileAsync で頼む。パスが引けないテクスチャはマゼンタ
  - [追加] 内蔵マゼンタは ppu 16 (1 ワールド単位角)
  - [追加] 未解決スプライトの警告と sortingLayerId の未定義 id 項目
  - [追加] "2D" カテゴリの色 (薄紅)
  - [未実装] groupKey は常に 0 (sub-06)。SpriteMask / Sliced / Tiled / ライト / material は未着手
検証:
  - msbuild Release / Debug -> 0 警告 0 エラー
  - MYE_SELFTEST_ONLY=m94 (Release / Debug) -> Sprite2DAsset ALL PASS (新規 4 項目含む)
  - demo_forward: --warp 960x540 --frames 6 --shot-frame 3 --no-fxaa -> --img-diff --tol 0 -> PASS maxDiff=0 diffPixels=0
  - 一時シーン (2x2 市松 point / linear、2x2 4 色、1x1 白、層 Front を一時追加。撮影後に原状復帰) -> plans\m94-2d\screenshots\sprite2d_basic.png
    - 市松 point 領域 180x180 の distinct 色は 2 値のみ (231,231,231) と (0,0,0)。境界 row y=90, x=657..663 = 231,231,231,0,0,0,0。linear 版は 162 値
    - 描画順: 緑 (Front,0) > 黄 (Front,-3) > 赤 (Default,5) > 青 (Default,1) = 層 -> order の整数キーどおり
    - flipX: 4 色の並びが左右反転。color 赤: 白 1x1 に (1,0,0,1) -> (231,0,0)
  - check_rules.ps1 -> 3 error (既存) / 50 warning。変更ファイル由来の新規は無し。着手前との件数比較は未実施
  - gen_project_files.ps1 -> 実行済み
  - replay_verify / shot_verify / golden 30 枚 / 全体 selftest -> 未実行 (ユーザー判断 9 で sub-05 へ)
  - Editor の GUI (ピッカー・D&D・コンボ・警告行・ギズモ・Create メニュー) -> 未確認 (コンパイルのみ)
自己採点 (1-5):
  仕様適合: 4 / 正しさ: 4 (非同期ロード中・デバイス復旧・複数ビューは未検証) / コード品質: 4 / テスト: 3 (描画結果は一時シーン撮影のみ、golden は sub-23)
不安・質問:
  - 全面スプライトは .meta の srgb が auto のとき UNORM でロードされる。2D の色味は sub-23 の golden で確定する想定でよいか
  - Sprite2D.normalMap / material のピッカーは汎用分岐のまま (sub-16 / sub-17)
申し送り:
  - sub-06: Item.groupKey (今は 0) へ SortingGroup2D を入れる。Emit は Simple 前提。LocalRect が Simple の寸法の正本
  - sub-15: sprite2d.hlsl の t1 / s1 は空き。Render はバッチごとに SRV t0 だけを張り、最後に剥がす
  - 未ロードのテクスチャは最初の 1 フレームは描かれない。--shot 系は決定的撮影が drain するので影響なし
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。スクショ `sprite2d_basic.png` で描画順 (緑 > 黄 > 赤 > 青 = 層 → order)、flipX、color、Point の 2 値境界を確認。`Sprite2DRenderer.h` / `Sprite2DDrawList.h` に D3D 型が無いことを grep で確認。`demo_forward` の `--tol 0` PASS。[追加] 6 件 (DrawList POD、IsPending、非同期ロード要求、マゼンタ ppu 16、警告 2 種、2D カテゴリ色) は採用。不安 1 (srgb auto = UNORM の色味) は spec 4.1.3 の「色の契約」として裁定: UNORM + `tonemapMode 0` (Passthrough) で元画素が ±1 で出る。確認は sub-06 の 7 / 2b、デモは 4.1.14。不安 2 (normalMap / material ピッカー) は sub-16 / sub-17 の既存項目どおり。
