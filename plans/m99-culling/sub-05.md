# sub-05: 2D の経路に視錐台判定を足す

- 依存: sub-02
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 4.1.5 (2D) / 受け入れ条件 5 (2D)。sub-02 の視錐台スイッチに従う。
- `Sprite2DRenderer::Collect` (:1576) の先頭で `OrthoWorldBounds` (:114-141) を 1 回取り (今は Parallax があるときだけ取っている :1607-1618)、`CollectSprites` (:939)・`CollectLines` (:993)・`CollectShapes` (:1014) の各項目のワールド矩形と比べて外れた項目を積まない。
  - Sprite2D: `Resolve` (:211) が作るローカル矩形は Emit 時なので、収集時に同じ矩形 (サイズ・pivot・反転・9 スライス等) を求める関数を共有する (式を 2 箇所に書かない)。4 隅をワールド行列で変換した外接矩形。
  - Line2D: 点列 + 幅の半分。SpriteShape2D: 点列 + 縁スプライトの厚み (一番厚い縁)。
  - z の扱いはタイルマップのチャンク判定 (:1089-1101) と同じ。
  - 落とさない: `OrthoWorldBounds` が false (透視カメラ・行列無し)、Parallax の繰り返し項目、マスク (`CollectMasks`)。
- 落とした項目を `culledNonMesh` に数える。残った項目の描画順 (ソートキーと順序) は変えない。
- 検証シーン `--culling-2d-demo` (コード生成): 正射影カメラ、画面内外の Sprite2D (回転・拡大つきを含む)・Line2D・SpriteShape2D・タイルマップ・Parallax 項目・マスク。

## やらないこと (このサブでは)
- 2D ライト・2D の影の変更、タイルマップの判定の作り直し (スイッチへの接続は sub-02 で済み)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.{h,cpp}`
- `src\Engine\Core\Ecs\Components.h:2503-2523` (Line2D / SpriteShape2D の点列)
- デモ: `DemoContent*.cpp` / `ShowcaseScenes.cpp`

## 受け入れ条件 (このサブ)
1. `--culling-2d-demo` で視錐台 ON/OFF が全画素一致 (tol 0)、ON で `culledNonMesh` > 0、OFF で 0
2. 回転 45° と拡大 3 倍のスプライトが画面端に一部だけ掛かるとき落ちない (矩形の単体テストか、シーン内の配置で確かめる)
3. 既存の `2d.png` / `2d_lights.png` が shot_verify で差なし
4. `counts` が Debug / Release / `--warp` で一致

## 検証コマンド
- Debug / Release ビルド
- `Editor.exe --culling-2d-demo --screenshot` ON / `--no-frustum-culling` + img-diff、`--render-stats-dump`
- 2D の selftest (`MYE_SELFTEST_ONLY` に 2D 系のスイート名)
- `tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-05 (round 1)
実装:
  - Sprite2DRenderer.cpp:Collect — 視錐台の AABB (OrthoWorldBounds、ON かつ正射影のときだけ) を 1 回取り、箱の 1 % + pixelSnapUnit の余裕を足して cullMin_/cullMax_ に保持 (cullActive_)。最後に落とした数を ReportCulled で culledNonMesh へ足す。
  - Sprite2DRenderer.cpp:EmitSprite / EmitLine / EmitShape — 判定は Emit 側 (Resolve 後・頂点を作る前) に置いた。項目は items_ に残るので SwitchLightGroup の呼び出し列とバッチ割りは ON/OFF で同じ = 描画順・光群は不変。Sprite は Parallax の項目を判定しない。マスクは判定しない。落としたスプライトでも、ライトありかつ法線マップありなら anyNormalReady_ を従来どおり更新する (法線 RT を作るかが ON/OFF で変わらないように)。
  - Sprite2DRenderer.cpp:SliceLocalRect / SpriteLocalRect / LocalRectOutsideFrustum / WorldRectOutsideFrustum / AabbOutsideBounds — 矩形は BuildSliceQuads と同じ SliceLocalRect を共有 (式は 1 箇所)。Simple は Resolve の local、flip は鏡映。4 隅をワールド行列で運んだ外接 AABB (回転・非一様スケール対応)、z も範囲判定。
  - EmitLine: 点列の AABB + 帯の張り出し (半幅 x line2d::kMiterLimit) をローカルで足してから行列で運ぶ。EmitShape: 評価済みの輪郭 (smooth の Catmull-Rom の膨らみ込み) のワールド AABB + height / 2 x kSpriteShapeMaxMiterScale。
  - SpriteShape.h/.cpp — マイター上限を kSpriteShapeMaxMiterScale として公開 (cpp の定数はこれを参照)。
  - CollectTilemaps: 既存のチャンク判定が落とした分も culledNonMesh に数えるようにした (判定自体は不変)。
  - DemoContent.cpp/.h:BuildCulling2DShowcaseScene、ShowcaseScenes.cpp — `--culling-2d-demo` (cache\culling_2d_showcase.scene.json)。
  - Sprite2DAssetSelfTest.cpp (j) — 視錐台判定の 11 項目 (範囲内/外、OFF、45 度 x3 の角が掛かる/外れる、Parallax、Line2D 横断、透視 / 行列なし)。
仕様との差分:
  - [逸脱] SpriteShape の張り出しは sub の「縁スプライトの厚み (一番厚い縁)」ではなく profile.height / 2 x マイター上限 (2)。BuildEdgeQuads の帯の厚みは profile.height だけで決まり、縁スプライトの高さは使わないため (実コードで確認)。
  - [追加] 箱へ余裕 (1 % + pixelSnapUnit)。TAA ジッタ (view.proj は非ジッタでない) とピクセルスナップで境界の物を落とさないため。
  - [追加] タイルのチャンク判定が落とした数も culledNonMesh に数える (「落とした項目を数える」の一貫性)。判定は変えていない。
  - [追加] 落としたスプライトでも法線マップ解決 (anyNormalReady_) を行う。理由は上。
  - [追加] 検証シーンの資産は --2d-demo と同じ (assets\2d\) を使う。
検証:
  - Debug / Release の MSBuild (MyEngine.sln) → どちらも成功。
  - `MYE_SELFTEST_ONLY=sprite2dasset,tilemap,line2d,spriteshape,renderstats,pixelperfect2d,twoddemo` で Debug `Editor.exe --selftest` → 全 PASS (2D cull 11 項目を含む)。fast 層全体は未実行。
  - `Runtime.exe --culling-2d-demo --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa` ON と `--no-frustum-culling` (Release) を img-diff --tol 0 → PASS (diffPixels=0 / 518400)。Release と Debug の ON も diffPixels=0。画像で、画面右端に掛かる回転 45 度 x3 のスプライトの角、左下に掛かる回転スプライト、右端の丘、マスクの内側の赤、横断線を確認。
  - `--render-stats-dump`: ON は culledNonMesh 16、OFF は 0。counts は Release warp / Debug warp / Release GPU の 3 通りで一致 (JSON の counts を比較)。
  - `tools\shot_verify.bat` → 2d / 2d_lights を含め PASS、FAIL は flow_title 1 枚のみ (diffPixels=944 maxDiff=62 で既知)。
  - `tools\check_rules.ps1` → 0 error (60 warning は既存)。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜4 を実測。縁の張り出しの式だけ sub と違う (理由は上)
  正しさ: 4 — ON/OFF 全画素一致、回転スプライトは単体テストとシーンの両方で確認。Scene View の 2D (別カメラ) と PixelPerfect の upscaleRT は shot_verify の 2d のみで確認
  コード品質: 4 — 矩形の式は 1 箇所に共有。判定を Emit 側に置いたため、落とした項目も Resolve までは通る (コストは小さい)
  テスト: 4 — 単体 11 項目 + 撮影 + dump。Sliced / Tiled / SpriteShape の判定は単体テストに無く、シーン (ON/OFF 一致) だけで確認
不安・質問:
  - 判定を Collect ではなく Emit に置いた (Resolve 後の矩形が要るため)。落とした項目のテクスチャ読み込み要求は従来どおり出る。収集段階で落とす方が安いなら Resolve 相当の分離が要る (今回は見送り)。
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.h
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\SpriteShape.h
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\SpriteShape.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\Sprite2D\Sprite2DAssetSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\DemoContent.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\DemoContent.h
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\ShowcaseScenes.cpp
  - C:\HAL\MyEngin\plans\m99-culling\sub-05.md (実装メモ)
申し送り:
  - sub-09 の cull_verify.bat に `--culling-2d-demo` を入れる (ON/OFF tol 0)。撮影の再現: `Runtime.exe --culling-2d-demo --warp --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa` (cache\culling_2d_showcase.scene.json が残っていたら先に消す)。
  - Scene View の 2D (Editor のカメラ) は未確認。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 1 (ON/OFF 全画素一致、culledNonMesh 16 / OFF 0)、2 (45 度 × 3 倍のスプライトが右端で落ちない。単体テストと撮影)、3 (2d / 2d_lights PASS。flow_title は既知の別件)、4 (3 構成の counts 一致) を確認。[逸脱] SpriteShape の張り出しは仕様の誤りとして採用 (帯の厚みは profile.height だけで決まる)。[追加] 余裕・タイルチャンクの計上・落とした物の法線マップ解決は採用。タイルを数えたことに合わせ、`culledNonMesh` は CPU プールも含む定義に揃えた (CPU プールの計上は sub-08)。不安への回答: 判定を Emit 側に置いたのは容認。テクスチャの読み込み要求が残るのは小さく、Resolve を分ける作り直しはしない。nit: Sliced / Tiled / SpriteShape の判定に単体テストが無い (シーンの ON/OFF 一致のみ)。足すかは任意。
