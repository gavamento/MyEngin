# sub-03: 局所影の画面外キャスター

- 依存: sub-02
- 状態: OK (コミットは司会)
- 往復: 1

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

SELF_EVAL: sub-03 (round 1)
実装:
  - RenderSystem.cpp:CollectDrawables — `collectShadowCasters` を「影あり && (平行光あり || (enableLocalShadows && 影付き局所光が選ばれた))」へ広げた (`HasLocalShadowLight` を匿名 namespace に追加)。名前は意味が「影のキャスター候補を集める」のままなので変えず、コメントだけ直した。
  - RenderSystem.cpp:AllocateShadowAtlas — `World&` 引数を追加。(1) 面ごとの視錐台と「シーン箱に触れるか」を控える。(2) 画面外キャスター (`shadowCasters_` の offscreenIndex >= 0) ごとに、どのタイルの視錐台に箱 (`sc.aabbMin/Max`、CSM のフィット箱とは別) が入るかを並列に求める (uint16 のビットマスク)。(3) 面カリングは「シーン箱にも画面外キャスターにも触れない面」だけ pixelSize = 0。(4) 画面外キャスターがあるときだけ、描く集合を `shadowCasters_` の順 (画面内 = queue_.opaque[queueIndex]、画面外 = マスク非 0 のもの) で `atlasQueue_` に組んで `ShadowAtlas::Render` へ渡す。画面外キャスターが無いときは従来どおり `queue_`。(5) 選ばれた画面外スキンだけ `AcquireSkinPalette` (CSM と同じキャッシュ) でパレットを評価。
  - RenderSystem.h — `AllocateShadowAtlas` の引数、`atlasQueue_` メンバ。
  - DemoContent.h / DemoContentRender.cpp:BuildCullingShowcaseScene / ShowcaseScenes.cpp — `--culling-demo` (cache\culling_showcase.scene.json)。太陽なし、影付きの点光源 (x=-40) とスポット (x=60) を画面の左右の外に置き、点光源の範囲内に静的メッシュ 2 (CasterStatic = 床へ影が伸びる / CasterPillar = 点光源の -X 面にだけ入る) と範囲外 1 (CasterFar)、スポットの範囲内に画面外のスキン 1 (x=45)。生成順は 光 → 床・箱 → キャスター → スキン (後続は末尾へ足す)。
  - ShadowAtlas.{h,cpp} は変更なし (`const RenderQueue&` を受けるので、渡す側を変えるだけで足りた)。
仕様との差分:
  - [追加] 局所影のアトラスの描画対象を、画面外キャスターがあるフレームでは queue_ ではなく「影の候補 (shadowCasters_) から組んだ別キュー」にした — 視錐台 OFF のとき queue_ には画面外の物も入っており、queue_ をそのまま使うと ON と描く集合 (と run の分け方) がずれる / 画面外キャスターを足すと二重に描くため。画面外が無いフレームは従来どおり queue_。
  - [追加] 面カリングの判定は「合併箱」ではなく「シーン箱に触れる || 画面外キャスターの箱のどれかに触れる」にした (spec の「判定箱に画面外キャスターの箱も含める」と同じ結果で、遠い 1 個が箱を広げて他の面の省略を台無しにしない)。
  - [追加] sub-03.md の「触る場所」にある ShadowAtlas.{h,cpp} は変更不要だった。
  - [未実装] 専用の selftest は足していない。ON/OFF の全画素一致の回帰は sub-09 の tools\cull_verify.bat の予定 (sub-02 の申し送り) に載せる前提。純関数は増えていない (`HasLocalShadowLight` は匿名 namespace)。
検証:
  - 修正前の記録 (最終のシーン、修正前の挙動 = collectShadowCasters を一時的に旧条件へ戻して Release ビルド、`--warp` 960x540 frame 3): ON と `--no-frustum-culling` の撮影は不一致 (diffPixels=12360 / 518400, maxDiff=190, tol=0)。差の場所は画面左端 (x 0..270, y 245..288 の平行四辺形 = CasterStatic の影、ON だけ影が消える) と右端 (x 730..960, y 255..270 の細い帯 = スキンの影)。dump: ON は shadowAtlas draws 6 / culledDraws 4 / culledFaces 2、OFF は draws 9 / culledDraws 11 / culledFaces 2。
  - 修正後 (Release `--warp`): ON と `--no-frustum-culling` の img-diff --tol 0 → PASS (diffPixels=0)。Debug `--warp` も PASS。Release と Debug の ON も一致 (tol 0)。dump: ON / OFF とも shadowAtlas tiles 6 / draws 10 / culledDraws 14 / culledFaces 1 (draws 6→10: 画面外の静的メッシュ + 柱 + スキンを描いた分、culledFaces 2→1: 柱が点光源の -X 面に入った分)。CasterFar (どの光の範囲外) は draws に入らない (全タイルの視錐台に入らない = マスク 0)。
  - 受け入れ 3: 既定ベンチ (`--render-bench-demo --warp`、frame 60) の counts は drawCalls 23 / culled 308 / shadowDrawCalls 44 / shadowCascadeCasters [6, 43, 833] / shadowCasterCandidates 3686 で、sub-02 の実装メモの数字と一致 (局所光なしなのでこの経路を通らない)。
  - 受け入れ 5: `--culling-demo --render-stats-dump` の counts を Release `--warp` / Debug `--warp` / Release (GPU) / Debug (GPU) で JSON 比較 → 4 つとも完全一致。
  - `Editor.exe --selftest` (Debug、fast 層) → 107 suites, 0 failed (shadowcull / render / renderstats PASS)。`tools\check_rules.ps1` → 0 error (60 warning は既存の rule 17)。
  - `tools\shot_verify.bat` → 1 本だけ FAIL: flow_title (diffPixels=944, maxDiff=62, tol=3)。差は画面下の赤い操作説明行 (D-Pad / Arrows ...) の文字だけで、このシーンは影付きのライトが 0 (ログ `shadow casters 0`) = 今回の経路を通らない。影を描くシーンの golden (demo_*, demo_render_*, ...) は全部 PASS = 今回の変更で動いた golden は無い (--update はしていない)。flow_title の差は既存の環境差と思われるが、sub-02 のコミットでの再現は未確認。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜3・5 を実測で確認。4 の shot_verify は変更起因の差が無いことは示したが、flow_title の FAIL の出所は未確認
  正しさ: 4 — 修正前後の撮影・カウントで ON/OFF 一致と「範囲外は描かない」を確認、Debug/Release/WARP/GPU 一致。互いに ON/OFF が崩れうる境界 (スキンの全姿勢箱とアトラスの bind 姿勢箱の判定の差) は下記
  コード品質: 4 — 既存の CSM の経路 (マスク → パレット → 組み立て) に揃えた。AllocateShadowAtlas が長くなった
  テスト: 3 — 専用の selftest は無く、撮影とカウントの手動比較が中心 (回帰化は sub-09)
不安・質問:
  - ShadowAtlas::Render は各描画物の箱を「メッシュの bind 姿勢の AABB」で判定する (既存)。画面外キャスターの選別は `sc.aabbMin/Max` (スキンは全姿勢を包む箱、余白あり) で行う。スキンの全姿勢箱がタイルに触れるが bind 姿勢箱が触れない場合は、パレットは評価されるが描かれない (安全側)。逆 (bind 箱だけ触れる) は全姿勢箱が bind を包む前提で起きないはずだが、保証は確認していない。
  - 既存の制限: `hasScene` が false (画面内の不透明がゼロで、スキンも無い) だと局所影のアトラスは丸ごと作られない。画面外キャスターだけのシーンでは出ない。今回は変えていない (ON/OFF で同じ挙動)。変えるかは planner の判断。
  - flow_title の FAIL: sub-02 以前から出ていたかの確認を planner / 司会に頼みたい (私の変更を通らない経路)。
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\RenderSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Rendering\RenderSystem.h
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\DemoContent.h
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\DemoContentRender.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Demo\ShowcaseScenes.cpp
  - C:\HAL\MyEngin\plans\m99-culling\sub-03.md (実装メモ)
申し送り:
  - sub-04 以降が `--culling-demo` へ物を足すときは DemoContentRender.cpp の BuildCullingShowcaseScene の**末尾** (スキンの後) に。カメラは (0,5,-10) +Z 向き、960x540 で z=10 の画面幅は x=±20。
  - 検証の撮影は Runtime.exe (`--warp --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa`)。Start-Process -Wait で起動しないと PowerShell ツールは終了を待たない。
  - sub-09 の cull_verify.bat に `--culling-demo` の ON/OFF 全画素一致を入れる (今回の修正前は 12360 画素ずれる = 回帰を捕まえられる)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 1 (修正前 12360 画素の不一致 → 修正後 0、Release/Debug の --warp)、2 (draws 6→10、CasterFar は描かない、culledFaces 2→1)、3 (既定ベンチの counts が sub-02 と一致)、5 (4 構成の counts 一致) を確認。受け入れ 4 は影を描く golden が全部 PASS で、変更起因の差は無い。`flow_title` の FAIL は影付きライト 0 のシーンでこの経路を通らないので sub-03 の指摘にしない (sub-09 で出所を切り分ける)。[追加] 3 件 (atlasQueue_、面カリングを「どれかに触れる」で判定、ShadowAtlas 無変更) は容認。atlasQueue_ は ON/OFF で描く集合をそろえるために必要で、sub-02 の申し送りどおり。不安への回答: スキンの全姿勢の箱は bind 姿勢を含む (engine_spec §6.14 の作り方) ので、選別で取りこぼすことはない。アトラスがスキンを bind 箱で判定する件と `hasScene == false` の件は既存の制限として spec 7. に記録し、M99 では直さない。nit: `AllocateShadowAtlas` が長くなったので、画面外キャスターのマスク計算を関数に切り出すのは任意。
