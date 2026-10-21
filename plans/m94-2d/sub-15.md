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
5. 確認: Global 1 (0.3 灰) + Spot 1 (白、半径 3) + 層 2 種 (`targetLayerMask` で片方だけ) + **SpriteMask2D 1 個 (sub-06 で Deferred 経路のマスクが未確認なので、この temp シーンに入れて Forward / Deferred の両方で同じ形になることを見る)** のスクショ。Forward と `--deferred` の 2 枚。

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

SELF_EVAL: sub-15 (round 1)
実装:
  - Components.h/.cpp: Light2DComponent (TypeId 85、NoHash、spec 4.1.9 の全欄、points[16] は Float2 個別登録)。light2dtype 定数。
  - SimSnapshot.h: kSimSnapshotVersion 57 -> 58。AcousticAudioSelfTest.cpp:129 を 58 に追随。
  - Sprite2DDrawList.h: Sprite2DBatch.lit、Light2DInstance / Light2DVertex / Sprite2DLightGroup、groupLights、HasLights()。
  - Sprite2DRenderer.{h,cpp}: CollectLights / BuildSpot・Freeform・SpriteLight / LightSetOf / SwitchLightGroup。描画順に走り「層 bit を含むライト集合」が変わる位置で群を切りバッチを割る (Push の比較に lit と forceNewBatch_)。Freeform は TriangulatePolygon2D + 外縁ストリップ。
  - Sprite2DPass.{h,cpp}: RGBA16F ライトバッファ (RenderTexture::Resize、メンバ)。群ごとに Global 合計色で clear -> 非 Global を加算 (ONE/ONE) -> lit バッチを描く。presence gate (lightGroups 空なら従来経路、バッファも t1 も張らない)。終了時に RT/DSV/ビューポート/PS b0 を戻す。
  - assets/shaders/sprite2d.hlsl (t1/s1、gLit、gInvScreen)、新規 sprite2d_light.hlsl。
  - SceneViewWindow.{h,cpp}: DrawLight2DGizmos (黄、Spot 円弧 + 扇の半径線、Freeform 多角形、Sprite 矩形、Global 太陽)。EditorComponentCatalog / CreateMenu (Create -> 2D -> Light 2D) / LocalizationTable.inl (Create_Light2D)。
  - Sprite2DAssetSelfTest.cpp: 節 (g) Light2D 19 項目。
仕様との差分:
  - [追加] Global は「クリア色に合計を入れる」実装 (clear(0) + 全面加算と同じ結果、全面描画 1 回を省く)。
  - [追加] タイルマップは lit 欄が無いのでライトのあるシーンでは常に lit。ライトが効く層の判定は Tilemap レイヤーの sortingLayerId。
  - [追加] どのライトにも照らされない層 (集合が空) の lit スプライトは黒になる (Unity と同じ)。表に無い層 id は Default 扱い。
  - [追加] 形状を作れないライト (Spot 半径 0、Freeform 3 点未満、Sprite 未解決) は数えない。
  - [追加] ライト頂点の Z はライトのエンティティ位置 (Z) を使う。透視カメラではスプライトと平面がずれうる (2D は正射影前提)。
検証:
  - Debug / Release /p:MyeWarnAsError=true -> 0 警告 (初回は既知の SelfTestUnused C4100 で落ち、再実行で通る)
  - MYE_SELFTEST_ONLY=m94 (Debug) -> exit 0、Light2D 節 全 PASS
  - check_rules -> 3 error / 50 warning (着手前と同じ既存分)
  - temp シーン (Global 0.3 + Spot 白半径 3 + 2 層 + lit=false + SpriteMask2D 円盤): D 近傍 136 / D 遠方 38 / 外した層 F 38 (Global のみ) / lit=false 128 / マスク内 (118,24,24)・外は背景。Forward と --deferred は maxDiff=0 (Deferred のマスクも同じ形)。Light2D 無し版は HEAD(044b508) の Runtime と --tol 0 で maxDiff=0。スクショ plans\m94-2d\screenshots\light2d.png / light2d_deferred.png。
  - チェックポイント 3: Editor.exe --selftest 全体 Debug / Release -> exit 0・FAIL 0・ERROR 0。replay_verify -> 20 ジョブ全 PASS + rules のみ既存 3 error で FAIL (全文 cache\m94\replay_verify_cp3.log)。shot_verify PASS (30 枚)、golden --tol 0 は 29 枚 maxDiff=0、demo_terrain_deferred は基点の絵と maxDiff=0。build_managed.bat Release -> 成功 0 警告。
版: snapshot 57 -> 58 のみ。ABI / Scene kDocVersion / kCookVersion は不変。hash 対象フィールドの append なし (kExpected 再採取不要)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-21、チェックポイント 3 = sub-11〜15)。スクショ `light2d.png` で Spot 内 > 外、外した層は Global のみ、lit=false は従来色、マスク円盤の内側だけ赤を確認。Forward = Deferred maxDiff 0 (sub-06 持ち越しの Deferred マスク確認も消化)、Light2D 無しは基点とビット一致。チェックポイント 3: 全体 selftest 両構成 FAIL 0、replay_verify 20 ジョブ個別 PASS (rules は既存 3 error)、shot_verify 30 枚 PASS + tol 0 は 29 枚 0 と terrain は基点と 0、build_managed Release 成功。Freeform / Sprite ライトの画素確認は sub-16 5b へ。不安 1 (空集合は黒) は `[ユーザーに聞ける]`、2・3 は spec 4.1.9 に記録。
