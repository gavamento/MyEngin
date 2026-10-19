# sub-15: Light2D とライトバッファ

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #14、4.1.9、7. R8

## やること
1. `Light2DComponent` (NoHash、TypeId 末尾、spec 4.1.9 の全フィールド。`points[16]` は Float2 ×16 を個別に登録 (`params[16]` の前例))。Catalog (2D)、Create → 2D → Light2D。`kSimSnapshotVersion` +1。
2. Engine 層 `Sprite2DRenderer` がライトを収集 (type、ワールド位置 / Z 回転、パラメータ、`targetLayerMask`)。描画順に層を走り「その層の bit を含むライトの集合」が前の層と変わる位置で `LightGroup` を切る → Renderer 層へ `[群 (ライト列) → スプライトのバッチ範囲]` の列として渡す。
3. Renderer 層 `Sprite2DPass`: ライトバッファ (RGBA16F、シーン RT と同サイズ、`RenderTexture::Resize` パターン、メンバで持つ)。群ごとに clear(0) → Global を全面加算 → Spot (放射状の減衰 × 角度の減衰、`innerRadius..outerRadius`、`innerAngle..outerAngle`、`falloff` 乗) / Freeform (多角形の耳刈り + `falloffWidth` の外縁グラデーション) / Sprite (テクスチャ付きクアッド) を加算 (ONE / ONE)。その群のスプライト (`lit`) を `color × tex × light[screenUV]` で描く。`lit == false` は従来色。シェーダ `sprite2d_light.hlsl` (ライト形状) と `sprite2d.hlsl` に t1 / s1 (ライトバッファ、linear clamp)。**presence gate**: Light2D が 0 個なら従来経路 (バッファを作らず t1 も張らない)。
4. Scene View ギズモ: type ごとの輪郭 (黄: Spot は内外半径の円弧、Freeform は多角形、Sprite は矩形、Global は小さい太陽マーク)。非 Play の SceneView でもライトが効く。
5. 確認: Global 1 (0.3 灰) + Spot 1 (白、半径 3) + 層 2 種 (`targetLayerMask` で片方だけ) のスクショ。Forward と `--deferred` の 2 枚。

## やらないこと (このサブでは)
- 影・法線 (sub-16)、Volumetric、Blend Style 複数、Light Order / Overlap Operation

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}`、`src\Engine\Renderer\Passes\Sprite2DPass.{h,cpp}`、`assets\shaders\sprite2d.hlsl`、新規 `sprite2d_light.hlsl`
- `SceneViewWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`
- 参考: `DeferredPath.cpp:724-729, 1189-1190` (RT 確保パターン)、`RenderTexture.h:19`

## 受け入れ条件 (このサブ)
1. (spec 27) スクショ `plans\m94-2d\screenshots\light2d.png`: スポット内 > 外 (画素値)、外した層の明度が Global のみ。Light2D 無しの temp シーンのスクショが sub-15 前後で `--tol 0` 同一。
2. (spec 28) Forward / Deferred の maxDiff ≤ 3。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` の selftest PASS、snapshot +1 のみ。
4. **チェックポイント (sub-11〜15 の分、spec 4-5)**: `Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`tools\replay_verify.bat` 全ジョブ PASS、`tools\shot_verify.bat` + 既存 golden 30 枚 `--tol 0` で maxDiff=0。割れたら sub-11〜15 のどれが原因かを特定して SELF_EVAL に書く。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --screenshot` と `--deferred`
**チェックポイント (このサブで回す)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat`、`tools\shot_verify.bat` + 既存 30 枚 `--tol 0`

## 実装メモ (coder が追記)

## フィードバック履歴
