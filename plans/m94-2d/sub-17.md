# sub-17: スプライトのサーフェスシェーダ枝、Parallax2D、2D ポスト例

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #25、4.1.11

## やること
1. サーフェスシェーダの枝: `assets\shaders\MyEngineSurfaceEntries.hlsli` に `MyeVSSprite2D` / `MyePSSprite2D` (Sprite2D の頂点レイアウト pos / uv / color、CB は Sprite2D パスの b0 + `MyEnginePerEffect` 相当の Properties CB)。`SurfaceProgram` にスプライト用エントリ (`ShaderManager::CompileSurfaceProgram` が 5 → 6 エントリ目を任意にコンパイル。既存のサーフェスには影響しない = 既存 `.surface.hlsl` の 5 エントリの出力はバイト同一)。作者は `/*@MyEngineProperties ... @*/` + `float4 PSMain(MyeSpriteInput i)` を書く。`Sprite2D.material` (`.mat.json` の shader が `*.surface`) を持つスプライトはそのプログラムで描く (バッチをマテリアルでも切る。ライトバッファは t1 で同じく渡す)。コンパイルエラーは内蔵シェーダへフォールバックし、`surface_error` 相当の表示を Inspector に出す (既存の流儀)。
2. 例 `assets\shaders\SpriteFx.surface.hlsl`: Properties = `FlashColor (Color)`, `FlashAmount (0..1)`, `OutlineColor`, `OutlineWidth (px)`, `Dissolve (0..1)`。`assets\materials\SpriteFx.mat.json`。
3. `Parallax2DComponent` (NoHash、TypeId 末尾): `factor / repeatX / repeatY / repeatSize`。`Sprite2DRenderer` の収集で、同エンティティの Sprite2D の描画位置を `pos + (cam.xy − pos.xy) × (1 − factor)` にし、repeat なら正射影の表示範囲 (透視なら repeat 無し) を覆う枚数 (上限 64) を複製。Catalog。`kSimSnapshotVersion` +1。
4. 2D ポスト例 `assets\shaders\CrtScanline.post.hlsl` (走査線 + わずかな色収差。`MyTint.post.hlsl` の規約) と `assets\CrtScanline.fxstack.json` (AfterTonemap)。エンジン変更なし。
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

## フィードバック履歴
