# sub-05: 2D の経路に視錐台判定を足す

- 依存: sub-02
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
