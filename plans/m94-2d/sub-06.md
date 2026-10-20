# sub-06: Sliced / Tiled、SortingGroup2D、SpriteMask2D (ステンシル)

- 依存: sub-04
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.3、7. R3

## やること
1. `drawMode` 1 Sliced: border (px → UV と単位) で 9 分割、角は原寸 (border / ppu)、辺と中央は `size` に合わせて伸縮。`size` が角の合計より小さいときは角を縮める (Unity と同じ崩れ方で可)。border 全 0 なら Simple と同じ 1 枚。
2. `drawMode` 2 Tiled: 中央と辺を原寸で `size` の中に繰り返し、端のタイルは UV を切る (Continuous)。上限 4096 頂点 / スプライト (超えたら切って警告 1 回)。
3. `SortingGroup2DComponent` (NoHash、TypeId 末尾): `sortingLayerId / orderInLayer`。Sprite2DRenderer の収集で、各 Sprite2D の最も近い祖先の SortingGroup2D を探し (HierarchyComponent の親リンクを上る)、キーを (グループの層順, グループの order, グループの entity.index, 自分の層順, 自分の order, 自分の index) にする。グループ無しは (自分の層順, 自分の order, 0, 0, 0, index) ではなく、Unity と同じく「グループはグループのキー位置に 1 つの塊として入る」: キー = (層順, order, index) をグループなら**グループの値**、塊の内側は自分の値で 2 次ソート。
4. `SpriteMask2DComponent` (NoHash、TypeId 末尾): `sprite / alphaCutoff (0.5) / sortingLayerId / orderInLayer / rangeBack / rangeFront (int32、既定 INT_MIN / INT_MAX 相当の大きい値)`。Sprite2DPass の冒頭でステンシルを 0 でクリア (`ClearDepthStencilView(D3D11_CLEAR_STENCIL)` — 深度は触らない。読み取り専用 DSV に差し替わる前 (`RenderSystem.cpp:2097` の前) に Sprite2D パスが走ることを確認し、ステンシル書き込み可能な DSV を使う)。マスクは描画順にスプライトと一緒に並び、自分の位置で「alpha ≥ cutoff の画素にステンシル ref 1 を書く (色は書かない)」。`maskInteraction` 1 = Equal 1、2 = Equal 0 でテスト。マスクの `rangeBack / rangeFront` は「この order 範囲のスプライトだけマスクされる」= 範囲外のスプライトはステンシルテスト無しで描く。複数マスクの重なりは OR (ref 1 を書き重ねる) で可。
5. Scene View のギズモ: SortingGroup2D は無し、SpriteMask2D は矩形 (紫)。EditorComponentCatalog に 2 行。`kSimSnapshotVersion` +1 (型 2 つ追加で 1 回)。
8. sub-05 の追補 (planner VERDICT sub-05): (a) `src\Engine\Renderer\ImGui\` の ImGui バックエンドに「Point サンプリングで描く」draw callback (`ImDrawList::AddCallback` で s0 を point / linear に切り替える関数を Renderer 層が公開) を足し、`SpriteEditorWindow` のキャンバスが使う (Editor 層は D3D を触らない)。拡大 4x で市松の境界が 2 値になるエディタスクショ 1 枚。(b) `AssetBrowserWindow` のテクスチャのダブルクリックは stb が読める形式 (.png / .jpg / .bmp / .tga など `SpriteEditModel` が開ける拡張子) だけスプライトエディタへ、それ以外 (.dds 等) は従来の OS 既定アプリへ。
7. 色の契約 (spec 4.1.3): temp シーンのカメラに `CameraPostFx.tonemapMode = 0` を付け、(128,128,128) と (200,50,10) の 1×1 テクスチャ (srgb auto) の Sprite2D を `--screenshot` → 画素が ±1 で一致することをログに残す。Inspector の sprite 欄に「テクスチャの .meta が srgb=on」の警告行を足す (sub-04 の未解決警告の隣、`Tr()`)。
6. 確認スクショ: Sliced (border 4 px の 16×16 を 3×1 単位)、Tiled (同じ絵を 3×2)、SortingGroup (グループ A の子 2 枚の間にグループ外の 1 枚が割り込まない)、Mask (丸マスクの VisibleInside / Outside)。

## やらないこと (このサブでは)
- Tile Mode = Adaptive、ソフトマスク、Sprite Sort Point

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp` (SortingGroup2D / SpriteMask2D)、`EditorComponentCatalog.cpp`
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp` (9 分割 / タイル / キー)、`src\Engine\Renderer\Passes\Sprite2DPass.cpp` (ステンシル状態 3 種: 書き / Equal1 / Equal0 / 無し)
- `RenderSystem.cpp:2047-2100` (DSV の順序)、`SceneViewWindow.cpp` (ギズモ)
- `SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`
- 参考: `src\Engine\Renderer\Device\RenderTexture.cpp:55-59` (D24S8)、`SwapChain.cpp:76-86`

## 受け入れ条件 (このサブ)
1. (spec 15) 4 枚のスクショ (`plans\m94-2d\screenshots\sprite2d_sliced_tiled.png`、`sprite2d_group_mask.png`) で、角の画素幅が原寸 (4 px × 画面倍率)、Tiled の繰り返し数、グループの塊、マスクの内外が期待どおり (画素座標のログ)。
2. SceneView と GameView と Runtime の 3 経路でマスクが同じに出る (SceneView のエディタスクショ 1 枚)。
2b. (spec 4.1.3 色の契約) tonemapMode 0 で (128,128,128) / (200,50,10) が ±1 (画素値のログ)。
2c. (sub-05 追補) スプライトエディタの 4x 拡大で市松の境界が 2 値 (エディタスクショ)。.dds のダブルクリックが OS 既定アプリへ行く (コードで示す)。
3. (spec 1-3) 0 警告、check_rules 0、snapshot +1 のみ。ステンシルクリアは Sprite2D が 0 枚なら実行しない (コードで示す)。重い 3 本は sub-10。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --screenshot`
チェックポイントではない (重い 3 本は sub-10)。

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-06 (round 1)
実装:
  - src/Engine/Core/Ecs/Components.{h,cpp}:SortingGroup2DComponent (TypeId 82) / SpriteMask2DComponent (TypeId 83) — どちらも kComponentNoHash。mask は sprite / alphaCutoff 0.5 / sortingLayerId / orderInLayer / rangeBack / rangeFront (既定 INT32_MIN / INT32_MAX)
  - src/Engine/Engine/Replay/SimSnapshot.h + AcousticAudioSelfTest.cpp:129 — kSimSnapshotVersion 53 -> 54 (1 回だけ)、定数も追随
  - src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.{h,cpp}:
      * ソートキーを (外側の層順, order, index, 内側の層順, order, index, isMask) に拡張。SortingGroup2D は自分を含む最も近い祖先を HierarchyComponent の親リンクで辿る (上限 64 段。グループがシーンに無ければ辿らない)
      * BuildSliceQuads (public static の純関数、GPU 非依存): border で 3x3 に割る。Sliced = 辺と中央を伸縮、Tiled = 辺と中央を原寸で並べて端を切る (左 / 下基準、上 / 右が切れる)。角は常に原寸、size が角の合計より小さければ比例して縮める。border 全 0 は Simple と同じ 1 枚。1 スプライト 4096 頂点で切って警告 1 回 (MYE_LOG_WARN)
      * Collect はマスクも集めて一緒に整列し、EmitMask が「alpha >= cutoff に ref 1 を書く」バッチ (stencil = kWriteMask) を出す。EmitSprite は maskInteraction 1 / 2 のスプライトに、描画順でここまでに出たマスクの range (orderInLayer) に入るものがあるときだけ kTestInside / kTestOutside を付ける
  - src/Engine/Renderer/Passes/Sprite2DDrawList.h:Sprite2DBatch に stencil / alphaCutoff、sprite2dstencil 定数、UsesStencil()
  - src/Engine/Renderer/Passes/Sprite2DPass.{h,cpp}:Render に stencilDsv を追加。UsesStencil のときだけ OMGetRenderTargets で元の RT / DSV を控え、書き込み可能な DSV へ差し替え、ClearDepthStencilView(D3D11_CLEAR_STENCIL) (深度は触らない)、終わりに元へ戻す。DS 状態 3 種 (マスク書き = 深度テストなし REPLACE / Equal ref 1 / Equal ref 0) + 色を書かないブレンド + PS 用 CB (b1)。Sprite2D が 0 枚なら従来どおり早期 return (D3D を一切触らない)。マスクを描くには stencilDsv が要るので、null のときはマスクを描かずマスク対象は素通り
  - src/Engine/Engine/Rendering/RenderSystem.cpp — Render に view.dsv を渡すだけ (DrawParticlesAndDebug は read-only DSV 差し替え (パーティクル) より前。元の DSV は Pass が戻すので後段は不変)
  - assets/shaders/sprite2d.hlsl — cbuffer b1 (gMaskClip) とクリップ。t1 / s1 は空きのまま
  - src/Engine/Engine/Rendering/Sprite2D/SpriteLibrary.{h,cpp}:SpriteDef.texSrgbOn (.meta の tex.srgb == 1)
  - src/Editor/Windows/Scene/InspectorWindow.cpp + LocalizationTable.inl:Insp_Sprite2DSrgbOn (Tr、英日) — sprite 欄の未解決警告の隣に srgb = on 警告行
  - src/Editor/Widgets/EditorComponentCatalog.cpp — SortingGroup2D / SpriteMask2D の 2 行 (カテゴリ 2D)
  - src/Editor/Windows/Scene/SceneViewWindow.cpp:DrawSprite2DGizmos — 全 SpriteMask2D の矩形 (紫)。Sprite2D の矩形は Sliced / Tiled のとき size を使う。SortingGroup2D のギズモは無し
  - src/Engine/Renderer/ImGui/ImGuiRenderer.{h,cpp}:ImGuiDrawListSetPointSampling(ImDrawList*, bool) — バックエンドが既に持つ platform_io の DrawCallback_SetSamplerNearest / Linear を積むだけ (Renderer 層が公開、呼び手は D3D を触らない)。SpriteEditorWindow のキャンバスが AddImage を Point で挟む (追補 8 a)
  - src/Editor/Windows/Sprite/SpriteEditModel.{h,cpp}:spriteedit::CanOpenInSpriteEditor (.png .jpg .jpeg .bmp .tga、大文字小文字無視)。AssetBrowserWindow のテクスチャのダブルクリックは true のときだけ窓へ、.dds 等は既存の else (ShellExecuteW = OS 既定アプリ) へ落ちる (追補 8 b)
  - src/Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.cpp 節 (f) (TypeId 82/83・NoHash・既定、Sliced 9 枚 / 角 0.25 / 中央 2.5x0.5、border 0 = 1 枚、角の縮み、Tiled 3x2 = 6 枚、端の UV 切り、4096 頂点の上限、size<=0)、節 (b) に texSrgbOn。src/Editor/SelfTest/SpriteEditorSelfTest.cpp 節 (h) (ダブルクリックの拡張子判定)
仕様との差分:
  - [追加] マスクの効き方 — マスクは自分の描画順の位置より後のスプライトにだけ効き、range (orderInLayer) は「描画順でここまでに出たマスクのどれかの range に入るか」で判定する。該当マスクが 1 つも無い maskInteraction 付きスプライトはステンシルテスト無しで描く (Unity は「マスクが無い VisibleInside = 見えない」。spec の「範囲外は無テスト」を素直に延ばした)。ref 1 の 1 枚しか無いので、range の違う複数マスクが同じ画素へ書いた場合の区別はできない (spec の「重なりは OR で可」の範囲)
  - [追加] ステンシルのクリアは「Sprite2D が 0 枚なら実行しない」より厳しく、描画リストにステンシルを使うバッチ (マスク) が 1 つでもあるときだけ (UsesStencil)。マスクの無い 2D シーンは DSV 差し替えもクリアもしない
  - [追加] Pass が OMGetRenderTargets で元の RT / DSV を控えて戻す (spec は「読み取り専用 DSV に替わる前に走る」としか書いていない。元が ro DSV でも書き込み DSV でも後段が同じ状態になる)。RenderSystem から view.dsv を渡す
  - [追加] SortingGroup2D は自分と同じエンティティのスプライトも塊に入れる (Unity と同じ)。マスクはグループに入れない (自分の層 / order で並ぶ)
  - [追加] ImGui の Point 化は新規 callback ではなく、バックエンドが既に持つ platform_io.DrawCallback_SetSamplerNearest / Linear を積む 1 関数 (spec の「AddCallback で切り替える関数」と同じ結果。D3D コードは増えない)
  - [追加] SpriteDef.texSrgbOn、Scene View のマスク矩形は全マスクに引く (描かれないので選択中だけにしない)、Sprite2D の矩形ギズモが Sliced / Tiled で size を使う、BuildSliceQuads を public static にして selftest から直接叩く
  - [追加] Tiled の基準は左 / 下 (端のタイルは右 / 上が切れ、y は絵の下半分が出る)。spec は「端は UV を切る」だけ
  - [未実装] なし (やること 1〜8、受け入れ条件 1 / 2 / 2b / 2c / 3 は下の検証で確認)
検証:
  - msbuild MyEngine.sln Debug / Release /p:MyeWarnAsError=true -> 0 警告 0 エラー (各 1 回目は既知の SelfTestUnused.kernel.hlsl C4100 で落ち、同じコマンドの再実行で通る。最終ビルドは両構成とも通した)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest (Debug / Release) -> exit 0。Sprite2DAsset ALL PASS (節 (f) 11 項目 + srgb 2 項目)、SpriteEditor PASS (0 failure、節 (h) 2 項目)、Physics / UI ALL PASS
  - pwsh tools\check_rules.ps1 -> 3 error / 50 warning (着手前と同数・同内容 = EditorMain.cpp の #ifdef _DEBUG と engine_dist .bat 2 本の LF。M94 由来の新規なし)
  - pwsh tools\gen_project_files.ps1 -> 実行 (ファイルの追加・削除は無し。build\ に差分なし)
  - demo_forward (Runtime.exe --warp ... --no-fxaa) -> Editor.exe --img-diff --tol 0 -> PASS maxDiff=0 diffPixels=0 (Sprite2D 無しのシーンが不変)
  - 一時シーン 3 本 (assets\_tmp_m94f の PNG 6 枚 + シーン JSON。カメラは正射影 orthoSize 4.21875 = 64 px/unit、CameraPostFx tonemapMode 0 / bloom 0 / fxaa 0) を Runtime.exe (Release) --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --screenshot で撮り、自前の PNG デコーダで画素を照合 (61 項目 PASS / 0 FAIL)。背景は (2,2,3)
    * sprite2d_sliced_tiled.png (16x16 の絵: 角 赤 4px / 上下辺 緑 / 左右辺 青 / 中央 黄・水色の 2x2 市松、Point、ppu 16、border 4)
      - Simple (-6,2): 角は x 64..79 の 16 px (= 4 px x 4 倍)
      - Sliced 3x1 (-2.5,2): y=118 の run = 赤 224..239 (16) / 緑 240..399 (160) / 赤 400..415 (16)。中央 x 240..399 は 80 px 幅の市松 2 マス x 16 px 高 2 段に伸びる (y=134: 黄 240..319 / 水色 320..399、y=150 は逆)
      - Tiled 3x2 (border 無し): 領域内の赤 (255,0,0) = 6144 px = 6 タイル x 4 角 x 16x16。6 タイルの左上角と上辺が期待位置に出る
      - Tiled 3x2 (border 4): 赤 = 1024 px (四隅のみ)、中央は 32 px 周期の原寸タイル
      - Tiled 2.5x1.5 (端が切れる): 右の 0.5 幅タイルは赤い角が 576 まで (16 px)・その右は上辺の緑、x=592 は背景。上の 0.5 高タイルは絵の下半分が出る
    * sprite2d_group_mask.png
      - グループ A (order 5) の子 a1 (0) / a2 (10) と外の L (7): 3 点 (-4.4,-0.8) (-3.7,-0.5) = L の青 (L は 2 枚の上)、(-5,-0.9) = a2 の緑。グループ無しの同配置は (-0.4,-0.8) (0.3,-0.5) = a2 の緑 (L が間に入る) -> グループは塊として 1 つの位置に入る
      - マスク: 丸マスク + VisibleInside = 円の中心 (4,2) だけ橙 (255,128,0)、円の外 (5.2,2) (2.8,3) は背景。VisibleOutside = 中心 (4,-2) が背景 (穴)、外 (5.2,-2) (2.7,-0.7) は紫 (153,51,229)
      - range: マスクの range (-50..50) の外の order 100 の VisibleInside (6.8,0) は無テストで全面が見える。range 内の order 0 の VisibleInside を円の外に置いた (6.8,-1.5) は背景 = 見えない
    * 色の契約 (2b、一時シーン C、撮影 png は除去): (128,128,128) の 1x1 -> 画素 (128,128,128)、(200,50,10) の 1x1 -> (200,50,10) (tonemapMode 0、srgb auto、差 0)
  - 受け入れ 2 (3 経路): 同じマスクシーンを Runtime.exe (上) / Editor.exe の GameView (sprite2d_mask_gameview.png) / SceneView 2D (sprite2d_mask_sceneview.png。imgui.ini を退避、一時プローブで 2D を ON) で撮影。3 つとも円だけ見える橙、穴の開いた紫、S3 が見え S4 が隠れる (SceneView はエディタカメラが ACES で色味が違う = 橙が黄に、紫が桜色に見えるが、マスクの形は同じ。マスク矩形ギズモの紫も出ている)
  - 受け入れ 2c: Editor.exe (Release) のスプライトエディタ窓で 48x40 の 4 px 白黒市松 PNG を開く -> sprite_editor_point.png (Zoom: x4)。キャンバス内の画像領域の distinct 色は 2 値 (0,0,0) / (255,255,255) のみ、行の run は 16 px 周期 (= 4 px x 4)。.dds のダブルクリックは AssetBrowserWindow.cpp の `isTexture && spriteedit::CanOpenInSpriteEditor(path)` が false になり、既存の最後の else (ShellExecuteW で OS 既定アプリ) へ落ちる (コード + selftest の拡張子判定)
  - 後始末: assets\_tmp_m94f (PNG・.meta 全部)、スクラッチのシーン JSON、色の確認 png、一時プローブ (EditorApp.cpp に足してすぐ戻した。git diff に無し) は除去済み。imgui.ini は撮影前の内容へ復元済み
  - 未実行 (ユーザー判断 9): 全体 --selftest、replay_verify.bat、shot_verify.bat と golden 30 枚、Debug ビルドでのスクショ (撮影は Release のみ)
  - 未確認: Deferred 経路でのマスク (撮影は Forward 既定のみ。Pass は view.dsv を渡し元の DSV を戻すだけで経路に依らない設計だが、深度 SRV と書き込み DSV の同時束縛が D3D 側で黙って外れる前提)。Inspector の srgb 警告行と Create / Add Component の GUI の実クリック (selftest は texSrgbOn のフラグまで)。SceneView のマスク矩形の選択ハイライト等
自己採点 (1-5):
  仕様適合: 4 — やること 1〜8 と受け入れ 1 / 2 / 2b / 2c / 3 を実装・実撮影で確認。マスクの range の解釈 (描画順で先に出たマスクのみ) と、マスク無しのときの素通りは spec の文言を延ばした解釈なので 5 にしない
  正しさ: 4 — 61 項目の画素照合と selftest 両構成 PASS、demo_forward は tol 0。Deferred と Debug スクショは未確認
  コード品質: 4 — 分割は GPU 非依存の純関数 + selftest、Pass は元の RT / DSV を戻す。EmitSprite / Collect はやや長い (マスク + グループ + 分割の 3 責務が同居)
  テスト: 4 — Sliced / Tiled / キャップ / 型 / 拡張子判定は selftest、描画経路は実撮影。ステンシルとグループの描画順は selftest 化していない (GPU とテクスチャが要るため撮影で代用)
不安・質問:
  - Q1 マスクの効き方を「描画順で先に出たマスクの range に入るときだけ」にした。Unity は「マスクの位置と無関係に range 内のスプライト全部」で、VisibleInside なのにマスクが無ければ見えない。Unity 寄せ (マスクを常に最初に描き、range に入る全マスクをテスト対象にし、マスク 0 個の VisibleInside は隠す) にするか、今の spec 準拠のままか
  - Q2 range が違う複数マスクが重なると、ref 1 の 1 枚だけなので「range 外のマスクにまで影響される」(spec の OR で可の範囲)。必要なら sub-17 以降でステンシルの bit を分ける
  - Q3 Scene View の SpriteMask2D 矩形は全マスクに出す (選択中だけにしない) 仕様にした。選択中だけがよければ 1 行で変えられる
触ったファイル:
  assets/shaders/sprite2d.hlsl
  src/Editor/SelfTest/SpriteEditorSelfTest.cpp
  src/Editor/Widgets/EditorComponentCatalog.cpp
  src/Editor/Windows/Asset/AssetBrowserWindow.cpp
  src/Editor/Windows/Scene/InspectorWindow.cpp
  src/Editor/Windows/Scene/SceneViewWindow.cpp
  src/Editor/Windows/Sprite/SpriteEditModel.cpp
  src/Editor/Windows/Sprite/SpriteEditModel.h
  src/Editor/Windows/Sprite/SpriteEditorWindow.cpp
  src/Engine/Core/Ecs/Components.cpp
  src/Engine/Core/Ecs/Components.h
  src/Engine/Core/Localization/LocalizationTable.inl
  src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp
  src/Engine/Engine/Rendering/RenderSystem.cpp
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DAssetSelfTest.cpp
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.cpp
  src/Engine/Engine/Rendering/Sprite2D/Sprite2DRenderer.h
  src/Engine/Engine/Rendering/Sprite2D/SpriteLibrary.cpp
  src/Engine/Engine/Rendering/Sprite2D/SpriteLibrary.h
  src/Engine/Engine/Replay/SimSnapshot.h
  src/Engine/Renderer/ImGui/ImGuiRenderer.cpp
  src/Engine/Renderer/ImGui/ImGuiRenderer.h
  src/Engine/Renderer/Passes/Sprite2DDrawList.h
  src/Engine/Renderer/Passes/Sprite2DPass.cpp
  src/Engine/Renderer/Passes/Sprite2DPass.h
  plans/m94-2d/screenshots/sprite2d_sliced_tiled.png
  plans/m94-2d/screenshots/sprite2d_group_mask.png
  plans/m94-2d/screenshots/sprite2d_mask_sceneview.png
  plans/m94-2d/screenshots/sprite2d_mask_gameview.png
  plans/m94-2d/screenshots/sprite_editor_point.png
  plans/m94-2d/sub-06.md
申し送り:
  - sub-15 (Light2D): sprite2d.hlsl の PS は b1 (マスクのクリップ) を使用済み。t1 / s1 は空きのまま。Sprite2DBatch に stencil / alphaCutoff が増えたので、ライトのバッチ分割はこれと同じ Push の比較に足す。Pass の DSV 差し替えは UsesStencil のときだけ
  - sub-07 / sub-10: Sprite2DRenderer の Item キーは外側 / 内側の 6 整数 + isMask。タイルマップのチャンクを同じ整列に混ぜるなら Item に種別を足す
  - スプライトエディタの Point 表示は ImGuiDrawListSetPointSampling(dl, true) と (dl, false) で AddImage を挟む。ImGui 内の他の画像にも使える
  - 一時シーンの 64 px/unit は orthoSize = 高さ / 2 / 64 = 4.21875。画素照合の手順 (px = 480 + 64x, 270 - 64y) は sub-23 のデモの座標確認にも使える
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。スクショ 3 枚 (`sprite2d_sliced_tiled.png`: 角の原寸・辺の伸縮・Tiled の原寸繰り返しと端切り / `sprite2d_group_mask.png`: グループの塊、VisibleInside の円、VisibleOutside の穴、range 外の無テスト / `sprite_editor_point.png`: 4x 拡大で 2 値の市松) で受け入れ条件 1 / 2c を確認。2 (3 経路) と 2b (128,128,128 / 200,50,10 が差 0) はログで確認。Q1 (マスクの効き方) は現行 (描画順で先に出たマスクの range) を v1 として採用し spec 4.1.3 に明記、Unity 互換は後回しに記録 — `[ユーザーに聞ける]`。Q2 (1 bit) は後回し。Q3 (全マスクの矩形ギズモ) は採用。Deferred のマスクは sub-15 の temp シーンで確認する項目を足した。
