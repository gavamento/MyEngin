# sub-16: ShadowCaster2D と法線マップ

- 依存: sub-15
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #15, #16、4.1.9

## やること
1. `ShadowCaster2DComponent` (NoHash、TypeId 末尾): `source / selfShadow / castsShadows / pointCount / points[16]`。Catalog。`kSimSnapshotVersion` +1。
2. 影: `shadows` が立つライトごとに、R8_UNORM の影 RT (シーン RT と同サイズ、1 枚を使い回す) を clear(0) → 全キャスターの多角形 (source 0: 同エンティティの Collider — Box は 4 点、Sphere は 16 角、Polygon2D は点列、それ以外は無し / source 1: points) の各辺を、ライト位置から辺の両端へのベクトルを「RT 対角 × 2」の距離まで延ばした四辺形として 1.0 で描く → `selfShadow == false` のキャスターは自分の多角形を 0.0 で描き戻す → ライト形状の PS (sub-15) が `1 − shadow[screenUV] × shadowStrength` を掛ける。`castsShadows == false` は除外。Global には影無し。
3. 法線: `Sprite2D.normalMap` を持つスプライトを、ライト群の処理前に法線 RT (RGBA8、clear (0.5, 0.5, 1, 1)) へ同じ頂点列で描く (flipX / flipY で法線の x / y を反転。`sprite2d_normal.hlsl`)。形状ライトの PS は `N = normalRT × 2 − 1`、`L = normalize(lightPos.xy − pixelWorld.xy, normalMapDistance)`、`max(0, N·L)` を掛ける。法線を持つスプライトが 0 枚なら法線 RT を作らない (gate)。
4. Scene View ギズモ: ShadowCaster2D の多角形 (紫)。Inspector: `normalMap` の AssetRef ピッカー (テクスチャ)。
5. 確認スクショ: Spot 1 + 箔 (ShadowCaster source 0、selfShadow false) → 影が落ち、箔自身は明るい。法線付きスプライト 1 枚、ライトを左 / 右に置いた 2 枚で明暗が反転。

## やらないこと (このサブでは)
- ソフトシャドウ、Composite Shadow Caster、Trim Edge / Alpha Cutoff (スプライトのシルエットからの影 = スコープ外)

## 触る場所 (planner の見立て)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `Sprite2DRenderer.{h,cpp}` (キャスター収集、法線付きスプライトの抽出)、`Sprite2DPass.{h,cpp}` (影 RT / 法線 RT / 押し出し頂点)、`sprite2d_light.hlsl`、新規 `sprite2d_shadow.hlsl` / `sprite2d_normal.hlsl`
- `SceneViewWindow.cpp`、`InspectorWindow.cpp` (normalMap ピッカー)、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 29) スクショ 3 枚 (`plans\m94-2d\screenshots\shadow2d.png`、`normal2d_left.png`、`normal2d_right.png`) の画素値のログ。
2. ShadowCaster2D も normalMap も無いシーンのスクショが sub-16 前後で `--tol 0` 同一 (gate)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` の selftest PASS、snapshot +1 のみ。重い 3 本は sub-20。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest
- temp シーンの `Runtime.exe ... --screenshot`
チェックポイントではない (重い 3 本は sub-20)。

## 実装メモ (coder が追記)

## フィードバック履歴
