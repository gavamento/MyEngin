# sub-17: スプライトのサーフェスシェーダ枝、Parallax2D、2D ポスト例

- 依存: sub-06
- 状態: OK (commit 3e54872)
- 往復: 1
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #25、4.1.11

## やること
1. サーフェスシェーダの枝: `assets\shaders\MyEngineSurfaceEntries.hlsli` に `MyeVSSprite2D` / `MyePSSprite2D` (Sprite2D の頂点レイアウト pos / uv / color、CB は Sprite2D パスの b0 + `MyEnginePerEffect` 相当の Properties CB)。`SurfaceProgram` にスプライト用エントリ (`ShaderManager::CompileSurfaceProgram` が 5 → 6 エントリ目を任意にコンパイル。既存のサーフェスには影響しない = 既存 `.surface.hlsl` の 5 エントリの出力はバイト同一)。作者は `/*@MyEngineProperties ... @*/` + `float4 PSMain(MyeSpriteInput i)` を書く。`Sprite2D.material` (`.mat.json` の shader が `*.surface`) を持つスプライトはそのプログラムで描く (バッチをマテリアルでも切る。ライトバッファは t1 で同じく渡す)。コンパイルエラーは内蔵シェーダへフォールバックし、`surface_error` 相当の表示を Inspector に出す (既存の流儀)。
2. 例 `assets\shaders\SpriteFx.surface.hlsl`: Properties = `FlashColor (Color)`, `FlashAmount (0..1)`, `OutlineColor`, `OutlineWidth (px)`, `Dissolve (0..1)`。`assets\materials\SpriteFx.mat.json`。
3. `Parallax2DComponent` (NoHash、TypeId 末尾): `factor / repeatX / repeatY / repeatSize`。`Sprite2DRenderer` の収集で、同エンティティの Sprite2D の描画位置を `pos + (cam.xy − pos.xy) × (1 − factor)` にし、repeat なら正射影の表示範囲 (透視なら repeat 無し) を覆う枚数 (上限 64) を複製。Catalog。`kSimSnapshotVersion` +1。
4. 2D ポスト例 `assets\shaders\CrtScanline.post.hlsl` (走査線 + わずかな色収差。`MyTint.post.hlsl` の規約) と `assets\CrtScanline.fxstack.json` (AfterTonemap)。エンジン変更なし。
4b. (sub-16 の申し送り) Inspector の `Sprite2D.normalMap` 欄に「テクスチャの .meta が srgb=on」の警告行 (sub-06 の sprite 欄の警告と同じ形、`Tr()`)。
5. 確認スクショ: flash 1.0 で白、dissolve 0.5 で穴、Parallax factor 0.5 でカメラ x を 0 → 2 に動かした 2 枚で背景が 1 動く、CRT を載せた 1 枚。

## やらないこと (このサブでは)
- Deferred のスプライトサーフェス、スキンメッシュ、ライトバッファ以外のライティング

## 触る場所 (planner の見立て)
- `assets\shaders\MyEngineSurfaceEntries.hlsli`、`MyEngineSurface.hlsli` (MyeSpriteInput)、新規 `SpriteFx.surface.hlsl`、`CrtScanline.post.hlsl`、`assets\*.fxstack.json`、`assets\materials\`
- `src\Engine\Renderer\Shader\ShaderManager.cpp:1049-1074`、`SurfaceProgram.h:44-74`、`Sprite2DPass.{h,cpp}` (マテリアル別バッチ、`BindSurfaceNamedCB/SRV/Sampler`)、`Sprite2DRenderer.{h,cpp}` (material の解決、Parallax)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`、`InspectorWindow.cpp` (material ピッカーは既存 "material" 分岐で足りるはず)、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`
- 参考: `plans\m79-project-surface-shaders\` の spec、`ForwardPath.cpp:796-859` (DrawSurfaceItem)、`SurfaceDrawBind.h`

## 受け入れ条件 (このサブ)
1. (spec 30) スクショ 4 枚 (`plans\m94-2d\screenshots\spritefx_flash.png`、`spritefx_dissolve.png`、`parallax_a.png` / `parallax_b.png`、`crt.png`) と位置差のログ。
2. (spec 31) `SpriteFx.surface.hlsl` を壊して保存 → フォールバックと表示 (エディタスクショ 1 枚) → 直して戻る。
3. 既存 `ToonFlat.surface.hlsl` / `WaterGerstner.surface.hlsl` を使う golden のうち `demo_render_forward` / `demo_render_deferred` の 2 枚だけを `Runtime.exe --render-demo [--deferred] ... --screenshot` → `--img-diff --tol 0` で maxDiff=0 (エントリ追加の無害を証明。30 枚全部は sub-20)。
4. (spec 1-3) 0 警告、check_rules 0 (規則 9 を含む)、`MYE_SELFTEST_ONLY=m94` の selftest PASS、snapshot +1 のみ。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- `Runtime.exe --render-demo --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa --screenshot tests\actual\demo_render_forward.png` (と `--deferred`) → `--img-diff --tol 0`
- temp シーンの `Runtime.exe ... --screenshot`
チェックポイントではない (重い 3 本は sub-20)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-17 (round 1)
実装:
  - assets\shaders\MyEngineSurfaceSprite.hlsli (新規) — 作者向け: MyeSpriteInput / gMyeSpriteTex・gMyeSpriteSamp / MyeSampleSprite / MyeSpriteTexel
  - assets\shaders\MyEngineSurfaceSpriteEntries.hlsli (新規) — 生成エントリ MyeVSSprite2D / MyePSSprite2D (CB MyEngineSprite2D = viewProj + lit / 1/寸法、ライトバッファ gMyeLightBuffer を PSMain の後に乗算)
  - ShaderManager.cpp:CompileSurfaceProgram / InstantiateSurface — 作者ソースが MyEngineSurfaceSprite.hlsli を include していればスプライト 2 エントリを足す (MyEngineSurface.hlsli を include していなければスプライト専用)。blob は 5 / 2 / 7 本。従来のメッシュ用の combined テキストは 1 バイトも変わらない
  - ShaderCache.cpp (blob 数の検査を 2 / 5 / 7)、SurfaceProgram.h (spriteVS/PS/InputLayout/Reflect、hasMesh / hasSprite)
  - GpuResources.{h,cpp}:GetOrBuildSurfaceState(forSprite) — 使い道に合わないシェーダ (スプライト専用をメッシュへ、メッシュ専用をスプライトへ) は ready にしない。スプライト用リフレクションも MyEnginePerMaterial のパックに使う。SurfaceShaderName (Inspector 用)
  - Sprite2DDrawList.h (Batch.material、UsesMaterial) / Sprite2DRenderer (Push の比較に material、Parallax の収集) / Sprite2DPass (DrawSurfaceBatch、BindBatchOutputState に分割 = sub-16 nit 2、Render に MaterialLibrary 引数) / RenderSystem.cpp (1 行)
  - Components.{h,cpp}:Parallax2DComponent (TypeId 87、NoHash)、EditorComponentCatalog、SimSnapshot.h (v60)、AcousticAudioSelfTest.cpp (60)
  - Sprite2DRenderer.cpp:EmitSprite / ParallaxRepeatRange — 描画位置 pos + (cam.xy - pos.xy) * (1 - factor)、repeat は正射影の表示範囲を覆う枚数 (片軸 64、両軸 8 x 8)、透視カメラは繰り返さない
  - InspectorWindow.cpp / LocalizationTable.inl — Sprite2D.material の注意 (サーフェス無し / コンパイル失敗 / スプライト用でない)、normalMap の sRGB 警告 (4b)。両言語を埋めた
  - assets\shaders\SpriteFx.surface.hlsl、assets\materials\SpriteFx.mat.json、assets\shaders\CrtScanline.post.hlsl、assets\CrtScanline.fxstack.json (+ .meta)
  - テスト: SurfaceMaterialSelfTest.cpp:TestSprite2DSurface (コンパイル / キャッシュ / 使い道の振り分け / Sprite2DPass の実描画 4 枚)、Sprite2DAssetSelfTest.cpp 節 (i) (Parallax と material のバッチ)。EditorMain.cpp の m94 suite に Surface 3 本 (Shader / Material / Deferred) を追加
仕様との差分:
  - [逸脱] スプライト用エントリは MyEngineSurfaceEntries.hlsli / MyEngineSurface.hlsli へ足さず、新規 2 ファイル (MyEngineSurfaceSprite.hlsli / MyEngineSurfaceSpriteEntries.hlsli) に置いた — 既存の 2 ファイルを 1 バイトも変えないので、既存 .surface.hlsl の combined テキストもキャッシュキーも変わらない (バイト同一の条件が構成上自明になる)
  - [逸脱] 「6 エントリ目を任意に」ではなく、スプライト 2 エントリ (VS / PS) を任意にした。見分けは作者ソースの include 名 (MyEngineSurfaceSprite.hlsli の有無、メッシュ用は MyEngineSurface.hlsli の有無)。両方 include すれば 1 ファイルでメッシュとスプライトを兼ねる (PSMain はオーバーロード)
  - [追加] SpriteFx の Properties 名は M79 の流儀に合わせて `_FlashColor` / `_FlashAmount` / `_OutlineColor` / `_OutlineWidth` / `_Dissolve` (spec の `FlashColor` 等に `_` を付けた)
  - [追加] GetOrBuildSurfaceState に forSprite 引数 / MaterialLibrary::SurfaceShaderName / Sprite2DPass::Render に MaterialLibrary 引数 / ShaderCache の blob 数 2・7 を許可 (形式の版は据え置き)
  - [追加] m94 suite に RunSurfaceShaderSelfTest / RunSurfaceMaterialSelfTest / RunSurfaceDeferredSelfTest を足した (メッシュ用のサーフェス経路が変わっていないことを毎サブ見るため)
  - [追加] Parallax の repeat は片軸 64 枚、両軸のときは 8 x 8 (上限 64)。上限を超えたら表示範囲の中央付近だけ描いて警告を 1 回出す
検証:
  - Debug / Release 各 `/p:MyeWarnAsError=true` → 0 警告 (非 WX を 1 回挟んでから WX を 2 回重ねた。初回だけ SelfTestUnused.kernel.hlsl の C4100 既知の癖で落ちる)
  - `MYE_SELFTEST_ONLY=m94` Debug / Release → どちらも exit 0、FAIL 0、PASS 2373。新規: TestSprite2DSurface (スプライト専用 / 併記 / メッシュ専用 / 規約違反の 4 本コンパイル、キャッシュヒットで blob 数 2・7・5 が復元、CB レイアウト、使い道の振り分け、Sprite2DPass 実描画 A 材質あり = 緑 / B 材質なし = 赤 / C 壊れた材質 = 内蔵 (青) / D メッシュ専用の材質 = 内蔵 (黄))、Parallax / material の収集 (factor 0.5 でカメラ x 2 → 1、繰り返し 9 / 4 / 64 枚、透視は 1 枚)
  - `pwsh tools\check_rules.ps1` → 3 error / 50 warning (基準どおり。M94 由来の増減なし)
  - 既存サーフェスが変わらないこと: (1) 既存 MyEngineSurface.hlsli / MyEngineSurfaceEntries.hlsli は git 上で無変更 (2) `Runtime.exe --render-demo` (Forward / --deferred) の `demo_render_forward` / `demo_render_deferred` を golden と `--img-diff --tol 0` → 2 枚とも maxDiff=0 (3) ただしこの 2 枚のデモは ToonFlat / WaterGerstner を使わないので、追加で flow_game の MeshRenderer 7 個を ToonFlat.surface にした temp シーンを基点 edb87ac の Release Runtime (cache\m94\s15\base_head に HEAD を展開してビルド) と今の Release Runtime で Forward / --deferred とも撮り `--tol 0` → 2 組とも maxDiff=0 (ToonFlat は色 / 影 / 速度の 5 エントリを全部通る)
  - 新コンポーネント / マテリアル無しのシーンが従来経路とビット一致: temp 2D シーン 2 本 (素のスプライト 4 体 / Light2D + lit スプライト 4 体) を基点の Release Runtime と今の Release Runtime で Forward / --deferred とも `--tol 0` → 4 組とも maxDiff=0
  - スクショ (plans\m94-2d\screenshots\、Release Runtime、960x540): spritefx_flash.png (左から 内蔵 / SpriteFx 何もしない / flash 1.0 = 真っ白 / outline 2px の水色)、spritefx_dissolve.png (dissolve 0.5 で穴)、parallax_a.png / parallax_b.png (カメラ x 0 → 2 で背景 -135 px = 1.0 unit、前景の目印 -270 px = 2.0 unit。parallax_log.txt)、parallax_repeat.png (repeatX で継ぎ目なく覆う)、crt.png (走査線 + 色収差、AfterTonemap の fxstack)
  - 受け入れ 2 (spec 31): SpriteFx.surface.hlsl を壊した状態で Editor.exe (Release、--select Hero2) を起動 → 内蔵シェーダで描かれ (flash されない)、Inspector の Sprite2D の下に赤字「Shader error (SpriteFx.surface): サーフェス規約 (スプライト): float4 PSMain(MyeSpriteInput i) : SV_Target -- drawn with the built-in sprite shader.」、右下のトースト / ステータスバーにも同じ文面 (spritefx_error.png)。壊れたまま Runtime で走らせ、コンパイル失敗のログを待って 3 秒後にファイルを直す → `[reload] surface shader recompiling` で復帰し、撮った絵は flash 白 + 水色の輪郭に戻る (hot reload の invalid → valid)。**走っている最中に壊したときは M79 の既存挙動どおり旧プログラムを維持する** (フォールバックにならない)
  - 追加で確認: lit + 材質 (Light2D の Global + Spot の下で左右対称に置いた内蔵 / SpriteFx で鏡映が一致、目の位置の非対称を除く、Forward / --deferred) と、マスク (VisibleInside) + 材質 (材質あり / なしの 2 枚が maxDiff=0)
  - 4b: temp シーンで normalMap に srgb = on のテクスチャを指すと、Inspector に橙の警告行 (両言語、折り返し付き) が出る (Editor Release、1600x1500 で撮影して目視)
  - ABI 変更なし、snapshot 59 → 60 のみ。TypeId 87
  - 未実行: 全体 selftest / replay_verify / shot_verify / build_managed (次は sub-20)。hash 対象フィールドの append は無い (再採取不要)
自己採点 (1-5):
  仕様適合: 4 — やること 1〜4b・5 と受け入れ 1〜4 を満たした。逸脱 2 点 (新規 2 ファイル、エントリ 2 本を任意) は「既存出力はバイト同一」の条件を強めるための選択で、差分に書いた
  正しさ: 4 — 実描画 (selftest 4 枚 + スクショ + lit / マスク / 復帰) と、基点との --tol 0 (Toon 含む) で確認。透視カメラでの Parallax の繰り返し無し・非ゼロ回転 / スケールの親付きスプライト・大量バッチの混在は実描画で未確認
  コード品質: 4 — Pass のバッチ描画を DrawSurfaceBatch / BindBatchOutputState に割った (sub-16 nit 2)。Render のバッチループ自体はまだ長い
  テスト: 4 — CPU (Parallax / batch)・コンパイル / キャッシュ / 振り分け・GPU 実描画を m94 に入れた。Inspector の表示はスクショの目視だけ (ユーザー目視は sub-24)
不安・質問:
  - 作者の `MyEngineSurface.hlsli` の include 名で「メッシュ用を持つか」を決めている。コメントや文字列に同名が書いてあっても誤検出する (影響は「メッシュ用エントリの要求が増えてコンパイルエラーになる」だけで、静かに壊れはしない)。明示のマーカー行 (例 `// @MyEngineTarget sprite`) に変えたほうがよいか
  - スプライトのサーフェスで輪郭線は矩形の内側にしか描けない (頂点を広げない)。SpriteFx のコメントに書いた。広げるには作者に VS を書かせる必要があり、v1 では見送り
触ったファイル:
  - src\Editor\App\EditorMain.cpp、src\Editor\Widgets\EditorComponentCatalog.cpp、src\Editor\Windows\Scene\InspectorWindow.cpp
  - src\Engine\Core\Ecs\Components.h、Components.cpp、src\Engine\Core\Localization\LocalizationTable.inl
  - src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp、src\Engine\Engine\Replay\SimSnapshot.h、src\Engine\Engine\Rendering\RenderSystem.cpp
  - src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h、Sprite2DRenderer.cpp、Sprite2DAssetSelfTest.cpp
  - src\Engine\Renderer\Device\GpuResources.h、GpuResources.cpp
  - src\Engine\Renderer\Passes\Sprite2DDrawList.h、Sprite2DPass.h、Sprite2DPass.cpp
  - src\Engine\Renderer\Shader\ShaderCache.cpp、ShaderManager.h、ShaderManager.cpp、SurfaceProgram.h、SurfaceMaterialSelfTest.cpp
  - assets\shaders\MyEngineSurfaceSprite.hlsli (+.meta)、MyEngineSurfaceSpriteEntries.hlsli (+.meta)、SpriteFx.surface.hlsl (+.meta)、CrtScanline.post.hlsl (+.meta)
  - assets\materials\SpriteFx.mat.json (+.meta)、assets\CrtScanline.fxstack.json (+.meta)
  - plans\m94-2d\screenshots\spritefx_flash.png / spritefx_dissolve.png / spritefx_error.png / parallax_a.png / parallax_b.png / parallax_repeat.png / parallax_log.txt / crt.png
  - plans\m94-2d\sub-17.md (この節)
申し送り:
  - sub-24 engine_spec: スプライトのサーフェスは MyEngineSurfaceSprite.hlsli を include して `float4 PSMain(MyeSpriteInput i)` を書く (頂点シェーダ・ライトの乗算・ブレンドはエンジン)。メッシュ用と併記可。使えない材質 (サーフェスでない / コンパイル失敗 / スプライト用でない) は内蔵シェーダで描かれ Inspector に理由が出る。走行中に壊した場合は旧プログラムが残る。輪郭線は矩形の内側だけ、法線 RT / 影 RT には材質は効かない (材質は色パスだけ)。Parallax2D の repeat は正射影だけ・片軸 64 枚 / 両軸 8 x 8、factor は描画位置だけでシミュレーションに影響しない
  - sub-20 以降: Sprite2DPass::Render の引数に MaterialLibrary が増えた (呼び元は RenderSystem.cpp の 1 箇所)。Sprite2DBatch に material が増え、Push の比較に入っている。m94 suite に Surface 3 本が入ったので所要時間が少し増える
  - `cache\m94\s15\base_head` は基点 edb87ac (HEAD の archive) を Release ビルドした状態に更新した (約 0.5 GB、git 管理外)。次の基点比較に流用できる
  - 今回の確認用 temp 資産 (assets\*\_s17_* の 42 ファイル) は全部消した。git に残るのは新規の SpriteFx / CrtScanline / MyEngineSurfaceSprite* とスクショだけ

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22)。スクショ `spritefx_flash.png` (内蔵 / 何もしない / flash 白 / outline 水色) ほか 7 枚と位置差ログ (背景 −135 px = 1.0 unit、前景 −270 px = 2.0 unit) を確認。既存サーフェスの不変は (1) 既存 2 hlsli が git 無変更 (2) demo_render 2 枚 tol 0 (3) ToonFlat の temp シーンを基点と Forward / Deferred で tol 0、の 3 重。フォールバック表示と invalid → valid のホットリロード復帰、4b の srgb 警告も確認。逸脱 2 点 (新規 2 ファイル、2 エントリ任意) は採用 (spec 4.1.11)。不安 1 (include 名検出) は現行維持 (誤検出は大きな音で壊れる)、不安 2 (輪郭線は矩形内) は v1 の限界。
