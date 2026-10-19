# sub-04: `Sprite2D` と `Sprite2DPass` (Simple / flip / 整数ソート / Point・Linear)

- 依存: sub-02, sub-03
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #1, #12, #24、4.1.3、4.3、4.4 (レイヤー)

## やること
1. `Sprite2DComponent` (TypeId は着手時の末尾、**kComponentNoHash**。フィールドは spec 4.1.3 の全部を最初から定義する — `drawMode / size / maskInteraction / material / normalMap / lit` は後続サブが使うが、フィールドだけ今置いて後で生バイトを伸ばさない)。登録表、`EditorComponentCatalog.cpp` に「2D」カテゴリと 1 行、`kSimSnapshotVersion` +1。
2. Engine 層 `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}`: World を走査し、active な Sprite2D ごとに `SpriteDef` を解決 (無ければ内蔵 16×16 マゼンタ)、ワールド行列 × (rect / ppu、pivot、flip) で 4 頂点を作り、ソートキー (層順 ← `SortingLayers::OrderOf`、orderInLayer、グループキー (sub-06 で埋める、今は 0)、entity.index) で**安定ソート**して頂点列 + バッチ (SRV、filter) を Renderer 層に渡す。viewZ は計算しない。
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

## フィードバック履歴
