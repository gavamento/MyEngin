# sub-03: 局所影の画面外キャスター

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.6 / 受け入れ条件 4。
- 検証シーン `--culling-demo` (コード生成、`ShowcaseScenes` に登録) を作る: 影付きの点光源 1・スポット 1、どちらの光の範囲にも画面外の不透明キャスター (静的メッシュ 1 + スキン 1)、太陽あり / なしの両方を確かめられるようにする (例 `--culling-demo` は太陽なし。太陽ありは既定ベンチで既に被覆)。後続サブ (sub-04) が物を**末尾に**足す前提で作る。
- **先に修正前の状態を記録する**: このシーンで視錐台 ON と `--no-frustum-culling` の撮影が一致しないこと (差の画素数と場所) を実装メモに書く。
- 画面外キャスターの収集条件を「平行光の影がある」から「平行光の影がある、または影付きの局所光が 1 つでも選ばれた」に広げる。`collectShadowCasters` の意味を分けるなら名前も分ける。
- `ShadowAtlas::Render` に画面外キャスターを渡し、各タイルの視錐台で判定して描く (今の `CulledDraws` の判定と同じ扱い)。面カリング (`RenderSystem.cpp:1905`) の判定箱に画面外キャスターの箱を含める (CSM のフィット AABB `sceneMin/Max` とは**別の箱**を持つ。フィットは変えない)。
- 画面外のスキンのパレットは、どれかのカスケードかタイルが必要とするときだけ評価する (`RenderSystem.cpp:1779-1828` の CSM の経路に合わせる)。

- **sub-02 からの注意**: 視錐台 OFF のとき、視錐台の外の物は `queue_.opaque` に入る (影の候補としては `offscreenCasters_` 側にだけ入る)。アトラスに画面外キャスターを足すとき、ON と OFF で描く集合が同じ (画面内 + 画面外キャスター、重複なし) になるようにする。OFF のときに `queue_` と `offscreenCasters_` の両方から同じ物を二重に描かないこと。

## やらないこと (このサブでは)
- 距離・画面サイズで落とした物の扱い (sub-06 / 07 が「入れない」を実装する)
- CSM 側の変更

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` — `CollectDrawables` の `collectShadowCasters` (:1132) と画面外キャスターの収集 (:1524-1557)、`RenderCascadeShadows` のスキンの扱い (:1779-1828)、`AllocateShadowAtlas` (:1860-1945)
- `src\Engine\Renderer\Passes\ShadowAtlas.{h,cpp}` (`Render` :61)
- `src\Engine\Engine\Demo\DemoContentRender.cpp` / `DemoContent.h` / `ShowcaseScenes.cpp`
- 既存テスト: `src\Engine\Engine\Rendering\ShadowCullSelfTest.cpp`

## 受け入れ条件 (このサブ)
1. `--culling-demo` で、修正前は視錐台 ON/OFF が不一致 (実装メモに記録)、修正後は全画素一致 (tol 0)
2. `--render-stats-dump` の `shadowAtlas.draws` / `culledDraws` / `culledFaces` が、画面外キャスターを描いた分だけ増え、光の範囲外のキャスターは描かない (シーンに範囲外の画面外キャスターを 1 つ置いて確かめる)
3. 既定ベンチ (局所光なし) の `counts` が sub-02 後と一致
4. `tools\shot_verify.bat`: 差が出た golden があれば、その golden のシーンに影付き局所光と画面外キャスターがあることと、視錐台 OFF の絵と一致することを示してから `--update` する (差が無ければ「差なし」)
5. `counts` が Debug / Release / `--warp` で一致

## 検証コマンド
- Debug / Release ビルド
- `Editor.exe --culling-demo --screenshot` (ON / `--no-frustum-culling`) + img-diff
- `Editor.exe --culling-demo --render-stats-dump`
- `MYE_SELFTEST_ONLY=ShadowCull,...` で `Editor.exe --selftest`
- `tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
