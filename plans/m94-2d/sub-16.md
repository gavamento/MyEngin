# sub-16: ShadowCaster2D と法線マップ

- 依存: sub-15
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #15, #16、4.1.9

## やること
1. `ShadowCaster2DComponent` (NoHash、TypeId 末尾): `source / selfShadow / castsShadows / pointCount / points[16]`。Catalog。`kSimSnapshotVersion` +1。
2. 影: `shadows` が立つライトごとに、R8_UNORM の影 RT (シーン RT と同サイズ、1 枚を使い回す) を clear(0) → 全キャスターの多角形 (source 0: 同エンティティの Collider — Box は 4 点、Sphere は 16 角、Polygon2D は点列、それ以外は無し / source 1: points) の各辺を、ライト位置から辺の両端へのベクトルを「RT 対角 × 2」の距離まで延ばした四辺形として 1.0 で描く → `selfShadow == false` のキャスターは自分の多角形を 0.0 で描き戻す → ライト形状の PS (sub-15) が `1 − shadow[screenUV] × shadowStrength` を掛ける。`castsShadows == false` は除外。Global には影無し。
3. 法線: `Sprite2D.normalMap` を持つスプライトを、ライト群の処理前に法線 RT (RGBA8、clear (0.5, 0.5, 1, 1)) へ同じ頂点列で描く (flipX / flipY で法線の x / y を反転。`sprite2d_normal.hlsl`)。形状ライトの PS は `N = normalRT × 2 − 1`、`L = normalize(lightPos.xy − pixelWorld.xy, normalMapDistance)`、`max(0, N·L)` を掛ける。法線を持つスプライトが 0 枚なら法線 RT を作らない (gate)。
4. Scene View ギズモ: ShadowCaster2D の多角形 (紫)。Inspector: `normalMap` の AssetRef ピッカー (テクスチャ)。
5b. (sub-15 の持ち越し) Freeform ライト (5 点の凸多角形 + falloffWidth) と Sprite ライト (丸いテクスチャ) を同じ temp シーンに 1 個ずつ置き、形の内側が明るく外側が Global のみになる画素値をログ (sub-15 は selftest の頂点数まで)。
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

SELF_EVAL: sub-16 (round 1)
実装:
  - Components.h/.cpp: ShadowCaster2DComponent (TypeId 86、NoHash、source / selfShadow / castsShadows / pointCount / points[16] を Float2 個別登録)。SimSnapshot.h 58 -> 59、AcousticAudioSelfTest.cpp:129 を 59 に追随。EditorComponentCatalog に 2D 分類で登録。
  - Sprite2DDrawList.h: Light2DInstance に shadows / shadowStrength / normalMapDistance、Shadow2DCaster / Shadow2DPoint、Sprite2DNormalBatch、shadowCasters / shadowPoints / shadowFill / normalVertices / normalBatches。
  - Sprite2DRenderer: CollectShadowCasters (shadows の非 Global ライトがあるシーンだけ、entity 順)、BuildShadowPolygon (Box 4 点 / Sphere 16 角 / Polygon2D 点列 / points。ギズモと共用)、PushNormal / ResolveNormalMap / NormalTransform (回転と flip を畳んだ 2x2 を頂点色へ)。法線マップを持つ lit スプライトがあるシーンだけ lit スプライトとタイルを法線バッチへ積み、使える法線マップが 0 枚なら捨てる (gate)。
  - Sprite2DPass: BuildNormalBuffer (RGBA8、clear (0.5,0.5,1,1)、色のアルファでブレンド)、RenderShadowMap (R8、ライトごとに clear 0 -> 全キャスターの辺の押し出し四辺形を 1 -> selfShadow false の多角形を 0)、BuildLightBuffer を LightContext 受けにして影 / 法線 SRV を t2 / t1 へ (nit 3: ライトバッファ構築部を 3 関数に割った)。Upload は法線頂点をスプライト頂点の後ろに同じ VB へ。
  - assets/shaders: sprite2d_light.hlsl (gFx / gScreen、1 − shadow × strength と max(0, N·L) をフラグが立つときだけ掛ける)、新規 sprite2d_shadow.hlsl / sprite2d_normal.hlsl (+ .meta)。
  - SceneViewWindow: DrawShadowCaster2DGizmos (紫)。InspectorWindow.DrawAssetRef: normalMap をテクスチャのピッカー / ドロップ対象に (名前に "tex" が無く総当たり一覧に落ちていた)。
  - Sprite2DAssetSelfTest.cpp 節 (h): TypeId / NoHash / 既定、presence gate、Box / Sphere / Polygon2D / points / 上限 / castsShadows / Global / 対応外の形、法線の 2x2 変換 5 項目 (26 項目)。
仕様との差分:
  - [逸脱] 影の押し出し距離は「RT 対角 × 2」の固定ではなく「頂点から画面の中心まで + 画面の対角」。画面外の遠いキャスターの影も欠けないことが式から言える (spec の式だと遠方キャスターで画面内に先端が見える)。
  - [追加] 法線 RT には法線マップを持たない lit スプライトとタイルも平らな法線 (0,0,1) で描く。無いと手前の平らなスプライトが奥の法線マップ付きスプライトの法線で照らされる (法線マップ付きが 1 枚も無いシーンでは積まない)。
  - [追加] 法線の回転: 法線は flip の符号に加えてスプライトの Z 回転でも回す (頂点色に 2x2 を載せる。spec は flip だけ)。回転した法線付きスプライトが逆向きに照らされるのを避ける。スケール負の軸は見ない。
  - [追加] 法線 RT は色のテクスチャのアルファでブレンドする (透明な画素は奥の法線を残す)。ステンシルマスクは法線 RT には効かない (マスクで隠れた画素にも法線は書かれる)。
  - [追加] normalMap は Sprite2D.normalMap の AssetID = テクスチャ (sRGB はオフ前提。.meta の srgb=2 にしておくこと。Inspector の警告行は未実装)。緑 = 上 (OpenGL 流)。
  - [追加] 法線 RT が有効なシーンでは、法線マップの無い lit スプライトも N·L (L.z = normalMapDistance) が掛かるので、ライトの真下以外は法線マップ付きが 0 枚のシーンより暗くなる (Unity の Normal Maps quality 有効時と同じ。spec 4.1.9 どおり)。
  - [追加] Light2DInstance.center を全形状でライトの位置にした (Freeform / Sprite も N·L の L に使う)。normalMapDistance の下限 1e-3、shadowStrength を 0..1 に丸める。
  - [追加] ShadowCaster2D の Create メニューは作っていない (Add Component のみ)。ギズモ色は 0xA040E0FF (SpriteMask の紫より濃い)。
  - [未実装] なし (やること 1〜5 / 5b)。
検証:
  - msbuild Debug / Release /p:MyeWarnAsError=true -> 0 警告 0 エラー (Debug 初回は既知の SelfTestUnused C4100 で落ち、再実行で通る)
  - MYE_SELFTEST_ONLY=m94 Editor.exe --selftest (Debug / Release) -> exit 0。Sprite2DAsset ALL PASS (節 (h) 26 項目)
  - pwsh tools\check_rules.ps1 -> 3 error / 50 warning (着手前と同じ既存分)、pwsh tools\gen_project_files.ps1 -> 実行 (シェーダ 2 本の追加。build\ に git 差分なし)
  - 基点 = 現 HEAD (4181a6d、Release の Runtime) で撮った 2 枚 (Light2D 5b 一式 / ライト無し) を、新 exe の同シーンと --img-diff --tol 0 -> 両方 maxDiff=0。新コンポーネント入りの 3 シーン (ShadowCaster2D だけ置いた / ライトが shadows=true でキャスター無し / ライト無しで normalMap と ShadowCaster2D を置いた) も基点と maxDiff=0 (presence gate)
  - 受け入れ 1 (画素。temp シーン assets\_tmp_m94p、960x540、64 px/unit、tonemap 0、床 0.5 灰、Global 0.2):
    * shadow2d.png (Spot (-3,0) 白 半径 9 + 箔 Box 0.6x3 selfShadow false): 箔の左 (-1.5,0) = 153 (= 0.5 x 1.2)、箔の背後 (3,0) (3,2) (3,-2) = 25 (Global のみ)、影の楔の外 (3,±3.9) = 153、箔自身 (0,0) = (255,61,61) = 明るいまま。selfShadow true は箔 (46,10,10) (暗い)、shadows=false は (3,0) = 153
    * source 1 の三角形と Collider Sphere (selfShadow true): 背後 25、横 153。三角形の内側も 25
    * normal2d_left.png / normal2d_right.png (半球ドームの法線マップ 4x4 単位、Spot 白 半径 12、normalMapDistance 1): 解析式 (radial x N·L) と全 14 点が ±1 で一致。左ライト: 左 (-1,0) = 177 / 中央 60 / 右 (1,0) = 0、右ライト: 左 0 / 右 178 (左右で反転)
    * flipX 付き (ライト左): 左 177 / 右 0 (絵ごと反転しても法線 x を反転しているので明暗は反転なしと同じ)、Z 回転 90 度でも同じ明暗 (回転を畳んでいる証拠)、手前の平らなスプライト (0.2,0.2) = 55 (平らな法線の解析値 56)
    * 5b (gate_light 同シーン): Freeform 五角形 (中心 (0,2)) 内側 (0,2) (0,1) = 140 (= 0.5 x 1.1)、外縁帯 (0,0.6) = 89、外 (0,0) (2.5,2) = 13 (Global のみ)。Sprite ライト (丸テクスチャ、中心 (4,-1.5)) 中心 = 137、(4,-0.5) = 77、円の外 (5.8,-0.2) = 13
  - Forward と --deferred: shadow2d / normal2d_left とも --img-diff --tol 0 で maxDiff=0
  - スクショ: plans\m94-2d\screenshots\shadow2d.png / normal2d_left.png / normal2d_right.png / light2d_shapes.png (5b)
  - 未実行: 全体 selftest / replay_verify / shot_verify / build_managed (sub-20 のチェックポイント、依頼どおり)。Scene View のギズモと Inspector の normalMap ピッカーは実操作未確認 (ビルドのみ)。AcousticAudio の selftest 本体は m94 suite に含まれず未実行 (値の追随だけ)
版: snapshot 58 -> 59 のみ。ABI / Scene kDocVersion / kCookVersion は不変。hash 対象フィールドの append なし (kExpected 再採取不要)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-22)。スクショ `shadow2d.png` で箔の背後の楔が Global のみに暗く、楔の外と箔自身は明るいことを確認。画素値は解析式と全点 ±1 (影・selfShadow・source 1・Sphere、法線の左右反転・flipX・Z 回転・平らなスプライト、5b の Freeform / Sprite ライト)。gate は 5 シーンで基点と tol 0、Forward = Deferred。押し出し距離の式の逸脱は採用 (spec 4.1.9)、法線無しスプライトへの N·L は Unity 準拠で維持 (`[ユーザーに聞ける]`)、normalMap の srgb 警告は sub-17 4b へ。
