# sub-04: 影のカスケードごとのカリングとスキンの保守的 AABB

- 依存: sub-01
- 状態: OK (commit df7b1d3)
- 往復: 2

## やること
spec §4.1.3、§2 #7 #8。
1. CSM のキャスター候補を、カメラの視錐台で落とす前の全不透明 (ステージ 1 の候補) から取る。カスケードごとにライトの直交視錐台 (奥行き方向はシーン AABB まで伸ばす) で判定し、カスケード別の描画リストで描く。
   - CSM のフィット AABB (`sceneMin/sceneMax` の取り方、`RenderSystem.cpp:1289-1300`) は**変えない**。変えると解像度配分が変わり golden が動く。変える必要が出たら「仕様との差分」に出す。
   - 画面外のキャスターも影の描画に必要なデータ (ワールド行列、スキンならパレット) を持つこと。スキンのキャスターが画面外のときのパレットは sub-04 では毎回評価してよい (間引きは sub-06)。
   - (sub-01 VERDICT から) 現状、影のキャスターは `queue_.Sort()` の前の収集順で `ShadowPass::Render` に渡り、CSM では run が組めずバラ描き (render_bench で 1 カスケード 3364 draw)。**カスケード別のリストは本描画のキューと同じキー (material → mesh) で並べ、`BuildInstanceRuns` でインスタンシングする**。深度だけのパスなので並び替えで深度の結果は変わらない想定。golden が動いたら原因を断定して報告。受け入れ条件 2 の draw 数は、この並べ替えによる減少と、カスケード別カリングによる減少を分けて報告する。
2. スキンの保守的 AABB: モデル登録時に、全クリップの全キーフレームについて「ボーンごとの頂点包絡 (バインド空間、ウェイト > 0 の頂点)」をそのフレームの骨行列で変換した和集合 + 余白をモデル空間 AABB として `SkinnedModel` に持つ。クックしない (kCookVersion に関係しない)。決定的に計算する。
   - `RenderableInFrustum` のスキン分岐をこの AABB で判定する。ラグドール作動中は従来どおり常に可視。
   - 余白の値と根拠をコメントに書く (IK の到達、ブレンドの補間)。
3. 統計: カスケード別の影 draw、パレット評価数 (sub-01 の欄)。

## やらないこと (このサブでは)
- 影へのオクルージョン、LOD、並列化 (sub-06)。

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (`CollectDrawables` のステージ 1〜3、`RenderCascadeShadows`)
- `src\Engine\Renderer\Passes\ShadowPass.cpp` (キューを受け取る形 → カスケード別リスト。`BuildInstanceRuns` の呼び方)
- `src\Engine\Renderer\Pipeline\FrustumCull.h` (`RenderableInFrustum`)
- `src\Engine\Renderer\Mesh\Skeleton.*` / `SkinnedModel` の登録 (`SkinnedModelLibrary`)
- 前例: `ShadowAtlas` のタイル単位カリング

## 受け入れ条件 (このサブ)
1. `render_bench` の「画面外キャスター」のカメラで影が描かれる (変更前は消える)。前後のスクショを報告。 — `--screenshot`
2. dump のカスケード別 draw が、全カスケード共通の候補数より少ない (少なくとも 1 カスケードで)。
3. selftest: テスト用スキンモデルの全クリップの全フレームを CPU スキニングし、全頂点が保守的 AABB の中にある。
4. 画面外のスキンキャラ (影も画面に落ちない位置) の描画とパレット評価が 0 (dump)。
5. 既存 golden が全部 PASS。動いたら原因を断定して報告 (影が増えた = 正しい修正による差かどうか。memory「golden が動いたときの切り分け」の 4 点計測)。`--update` で黙って塗らない。
6. `tools\check_rules.ps1` PASS。
7. (round 1 VERDICT で追加) ライト側へ離れた画面外キャスター (CSM の zNear より光源側にはみ出す / 丸ごと外にある物) の影が消えない。カスケードの判定は近平面を除く 5 面、CSM の描画は深度クランプ (spec §4.1.3)。 — selftest (判定関数: 近平面より光源側の箱が残り、xy の外の箱は落ちる) + スクショ (render_bench か一時シーン) + golden 全 PASS

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、`render_bench` の撮影と dump

## 実装メモ (coder が追記)

### SELF_EVAL: sub-04 (round 1)

実装:
- `src/Engine/Renderer/Mesh/SkinBounds.h/.cpp` (新規) — `ComputeSkinnedLocalAabb` (ボーンごとの頂点包絡 × 全クリップの全キー時刻 + キー間 1/30 秒刻み + バインド、骨行列 `IB * jointGlobal` で 8 隅を送った和集合 + 余白 = 最長辺の半分・下限 5cm)、`SkinBoundsCache` ((モデル, メッシュ) の組ごと、revision で無効化)。
- `Skeleton.h/.cpp`・`GpuResources.h/.cpp` — `SkinnedModel::revision` / `Mesh::revision` (Register ごとの通番)。
- `FrustumCull.h` — `RenderableInFrustum` の第 5 引数を `skinned` から `alwaysVisible` へ (意味: 箱で判定できない物だけ常に可視)。
- `RenderSystem.cpp/.h` — ステージ 1 でスキンの箱 (ラグドール作動中・箱なしは常に可視) を `CullCand` へ、ステージ 2 は箱で判定。ステージ 3 で CSM の候補 `shadowCasters_` (画面内は `queue_.opaque` の添字、画面外は `offscreenCasters_`) を集め、`RenderCascadeShadows` がカスケードごとに `BuildFrustum(lightVP)` + `WorldAabbInFrustum` で判定 → 画面外のスキンは入ったカスケードがあるときだけパレット評価 → カスケード別の `RenderQueue` を `Sort()` (本描画と同じキー) して `ShadowPass` へ。パレット評価は `EvaluateSkinPalette` へ切り出し (中身は無変更)。`paletteEvaluated` を加算。
- `ShadowPass.cpp/.h` — `Render` がカスケード別のキュー配列を受け、run はカスケードごと・ワールド行列は全カスケード分を 1 本のバッファへ。単一キュー版のオーバーロードを残した (既存 selftest 用)。
- `Profiler.h/.cpp`・`RenderStatsDump.cpp`・`RenderStatsSelfTest.cpp` — `shadowCasterCandidates` / `shadowCascadeCasters[3]` を追加 (dump の `counts` 節)。
- `SkinBoundsSelfTest.h/.cpp` (新規)・`EditorMain.cpp` — 手組みの鎖・実アセット 2 本 (anim_test.glb / CesiumMan.glb) の全クリップの全 tick を CPU スキニングして包含、ブレンド姿勢、決定性、キャッシュ無効化、壊れた入力。
- `tests/golden/demo_forward.png` / `demo_deferred.png` / `demo_forward_fxaa.png` — 更新 (下記の差分と原因)。
- `build/Engine.vcxproj(.filters)` — gen_project_files の更新 (新規 4 ファイル)。

仕様との差分:
- [逸脱] スキンの保守的 AABB は「モデル登録時に SkinnedModel へ持つ」ではなく「最初に描くとき (モデル, メッシュ) の組ごとに計算してキャッシュ」。理由: メッシュとスケルトンは別々に登録され (ModelLoader / FBX / cook 再生 / 手組みで経路が違う)、組はエンティティでしか決まらない。クックはしない (kCookVersion 不変)。登録の通番 (revision) でホットリロードに追従。
- [追加] CSM のフィット AABB (`sceneMin/Max`) を変えないため、画面外のスキンもバインドポーズの world AABB で sceneMin/Max へ加え続ける (従来はスキンが常に可視 = 入っていた)。画面外の非スキンは従来どおり入れない。
- [追加] `viewZ` を画面外の候補にも求める (影のキューのソートキー)。画面内の値は無変更。
- [追加] `Mesh::revision` / `SkinnedModel::revision` と `ShadowPass::Render` の単一キュー版オーバーロード。
- [追加] 統計 `shadowCasterCandidates` / `shadowCascadeCasters` (受け入れ 2 の「共通の候補数」の出所)。
- [逸脱] GPU オクルージョンの判定箱 (`hasWorldAabb`) は、スキンには今回も載せていない (sub-02 の「sub-04 まで判定せず」は保守的 AABB が姿勢に収まる保証が IK / ブレンドで無いので据え置き)。sub-06 以降で載せるか判断してほしい。
- [未実装] なし。ただし受け入れ 1 の「画面外キャスターの奥行き」は、フィットの近平面 (`zNear = min(minZ, sceneMinZ) - 1`) がシーン AABB までしか引き戻されないので、シーン AABB (画面内の物 + 画面外スキン) よりライト側に離れた画面外キャスターはカスケードの近平面でクリップされる (従来と同じ。render_bench は 500x500 の地面が AABB に入っていて Tower は届く)。

検証:
- Release / Debug ビルド (`MyEngine.sln` x64) → 成功 (警告・エラーなし)。
- `bin\x64\Release\Editor.exe --selftest` → exit 0 (77 秒)。`Skin bounds self test: PASS (0 failures)` (25 項目)、`Render stats self test: PASS`。
- `bin\x64\Debug\Editor.exe --selftest` → [FAIL] 無し、`Skin bounds` / `Render stats` PASS (ServerNetSelfTest は今回は一過性 FAIL 出ず)。終了コードはログでは未取得 (Start-Process のハンドルを別シェルで失った)。
- `tools\check_rules.ps1` → 0 error / 50 warning (既存の rule 7 のみ、増減なし)。
- 実アセットの箱: anim_test.glb (4 クリップ 272 姿勢、バインド高 1.850 → 箱高 5.363 = 2.9 倍)、CesiumMan.glb (1 クリップ 122 姿勢、1.138 → 2.740 = 2.4 倍)。全 tick で全頂点が箱の中。箱はかなり緩い (余白が最長辺の半分 x 両側 + クリップのルート移動)。
- `render_bench` dump (`Runtime.exe --render-bench-demo --deferred ... --shot-frame 30 --render-stats-dump`) の `counts` が Release / Release --warp / Debug / Debug --warp の 4 構成で完全一致。
  - 変更前 (HEAD、同じ Release 実機): drawCalls 16、culled 307、shadowCascadeDraws [3364,3364,3364]、shadowTriangles 3,967,884、paletteEvaluated 0。
  - 変更後: drawCalls 15、culled 308 (カメラ背後のスキンが落ちた)、shadowCasterCandidates 3678、shadowCascadeCasters [4,37,825]、shadowCascadeDraws [4,10,14]、shadowTriangles 333,096、paletteEvaluated 6 (画面内 6 体。背後の 1 体は影もどのカスケードにも入らず評価されない)。
  - 影の draw 減少の内訳 (受け入れ 2): 並べ替え + インスタンシングだけ (カスケード判定を一時的に全通し) で 3364 → [17,17,17]。カスケード別のカリングで 17 → [4,10,14] (候補 3678 → [4,37,825])。triangles は 4,305,528 → 333,096。
- スクショ (受け入れ 1): 前 `C:\HAL\MyEngin\bin\x64\Release\shots\m90d_shadow_before.png` (床に Tower の影なし)、後 `...\m90d_shadow_after.png` (カメラ背後の Tower の影が床と壁に落ちる)。
- 画面外のスキンの影 (受け入れ 4 の補助): 一時コード (DemoContent を一時改変: 背後のキャラを 8 倍・太陽 20 度・Tower 除去。コミットしない、元に戻して確認済み) で、画面外のスキンが画面へ影を落とす配置にすると paletteEvaluated が 7 になり (drawCalls 15 = 本体は描かれない)、スキンの形の影が床に出ることを目視 (`%TEMP%\m90d\big2.png`)。この経路を通す常設テストは無い (GPU の画素検査が要る)。
- golden (受け入れ 5): `tools\shot_verify.bat` を更新前に回して 3 枚 FAIL (demo_forward / demo_deferred: maxDiff 133、1553 / 1549 画素 (tol 3)、demo_forward_fxaa (tol 0): 2181 画素)。他 27 枚は PASS。**原因の断定 (4 点)**:
  1. 差の向き: FAIL した画素は y=306 以下の手前側全域で、deferred 1549 画素すべて暗くなった (明るくなった画素 0)。影が増えただけ。fxaa 版は暗 1898 / 明 18 (FXAA の縁)。
  2. 画面外のキャスターの収集だけを一時的に止めて (env ゲート、コミットしない) 同じ撮影をすると、差は 1〜2 画素 (maxDiff 6〜8) まで消える。= 増えた影はカメラの視錐台の外にあるデモの立方体の影 (仕様どおり直った動作)。
  3. さらにカスケードの並べ替え (Sort) も止める (カスケード別のカリングだけ残す) と tol=0 で完全一致 (maxDiff 0)。= カスケード別カリングは絵を 1 ビットも変えない。残る 1〜2 画素 (6〜8 レベル) は並べ替えで連続 run の組み合わせが変わり、インスタンス版の深度 VS (`world * lightVP` をシェーダ内で掛ける) と非インスタンス版 (CPU で掛ける) の丸め差が出たもの。
  4. golden を撮り直した後の `shot_verify.bat` → 30 枚 PASS (exit 0)。更新は 3 枚だけを同じ撮影条件 (`--warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3`、fxaa 版以外は `--no-fxaa`、`--deferred` は demo_deferred のみ) で個別に撮り直した (`--update` で全 30 枚を塗っていない)。
- 未実施: `tools\replay_verify.bat` (sim・ABI・snapshot に触れていないため。描画だけの変更)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜6 を満たした。減点: 箱を「登録時」ではなく初回描画時に計算 (理由つきの逸脱)、スキンをオクルージョンの判定箱に載せていない、golden 3 枚を更新した (原因は断定済みだが判断は planner)。
  正しさ: 4 — 4 構成で決定的な数が一致、golden 30 枚 PASS、画面外スキン経路を一時コードで目視。減点: 画面外スキンの影の経路に常設テストが無い、箱が緩い (2.4〜2.9 倍)。
  コード品質: 4 — パレット評価を関数へ切り出して本描画と影で共有、収集・判定・描画の段が分かれている。減点: `ShadowCaster` を RenderSystem.h に置いた、sceneMin を保つためのスキン特例がある。
  テスト: 4 — 箱の包含を実アセットの全 tick で検査 (締まっていることの逆テストつき)、キャッシュ無効化、統計。減点: ShadowPass の per-cascade 経路 (run のずれ) と画面外スキンのカスケード判定を直接検査するテストが無い (dump と golden 経由のみ)。

不安・質問:
- (planner へ) golden 3 枚の更新を了承してほしい。却下するなら `git checkout tests/golden/demo_forward.png tests/golden/demo_deferred.png tests/golden/demo_forward_fxaa.png` で戻せる (その場合 shot_verify は 3 枚 FAIL)。差の中身は上記 4 点で「手前の床へ画面外の立方体の影が増えた」のみ。
- (planner へ) スキンの箱の余白 (最長辺の半分) は IK / ブレンドの根拠なしの安全側の値。実アセットの全クリップのキーフレームだけなら余白ゼロでも収まる (ブレンドとは別) が、IK の到達は箱の外へ出うる。緩い (2.4〜2.9 倍) ので、画面端の判定は甘い。必要なら sub-06 で詰める。
- (planner へ) 画面外キャスターの奥行き: 近平面の引き戻しを変えていない (§4.1.3 の「奥行き方向はシーン AABB まで」を既存の `zNear` のままとした)。シーンの AABB が小さくライトの手前に離れた画面外キャスターはクリップされる。直すなら `ComputeCascadeVPs` に画面外キャスターの AABB を渡す必要があり、フィットが変わりうる。

触ったファイル:
- build/Engine.vcxproj, build/Engine.vcxproj.filters (gen_project_files の生成物だがコミットに要る)
- src/Editor/App/EditorMain.cpp
- src/Engine/Core/Diagnostics/Profiler.h, Profiler.cpp
- src/Engine/Engine/Rendering/RenderStatsDump.cpp, RenderStatsSelfTest.cpp
- src/Engine/Engine/Rendering/RenderSystem.h, RenderSystem.cpp
- src/Engine/Engine/Rendering/SkinBoundsSelfTest.h, SkinBoundsSelfTest.cpp (新規)
- src/Engine/Renderer/Device/GpuResources.h, GpuResources.cpp
- src/Engine/Renderer/Mesh/Skeleton.h, Skeleton.cpp
- src/Engine/Renderer/Mesh/SkinBounds.h, SkinBounds.cpp (新規)
- src/Engine/Renderer/Passes/ShadowPass.h, ShadowPass.cpp
- src/Engine/Renderer/Pipeline/FrustumCull.h
- tests/golden/demo_forward.png, tests/golden/demo_deferred.png, tests/golden/demo_forward_fxaa.png
- plans/m90-render-optimization/sub-04.md (実装メモ)

申し送り:
- sub-06 (URO・並列化): 影の候補は `RenderSystem::shadowCasters_` / `offscreenCasters_`、カスケード判定は `RenderCascadeShadows` の直列ループ (候補 × 3 カスケードの AABB 判定。並列化はここ)。画面外スキンのパレットは入ったカスケードだけ毎回評価 (間引きは未)。`EvaluateSkinPalette` がパレット評価の唯一の入口。
- スキンの箱のキャッシュはステージ 1 (直列) で `SkinBoundsCache::Get` を引く。並列段へ持ち込まないこと。
- `Runtime.exe` を引数なし (`--frames` なし) で起動すると終了しない。一時スクリプトで起動した Runtime が残るとビルドが exe をロックする (今回 1 回踏んだ)。
- AllocateShadowAtlas (局所ライトの影) は従来どおり画面内の `queue_.opaque` から取る (画面外キャスターの局所影は今回も出ない。仕様は CSM のみ)。

### SELF_EVAL: sub-04 (round 2)
- #1: FrustumCull.h に WorldAabbInFrustumNoNear / CascadeCasterMask (近平面 planes[4] を除く 5 面) を足し、RenderCascadeShadows の判定を差し替え。ShadowPass の CSM 用ラスタライザ (CullBack / CullNone の 2 本) を DepthClipEnable=FALSE (局所影アトラス・CSM のフィットは無変更)。
- #2: ShadowCullSelfTest (新規) — 5 面判定 (光源側の箱が残る / xy・遠平面の外は落ちる / 6 面判定との対照 / 一方のカスケードだけ)、画面外スキンのパレット評価 (どこにも入らない 2 体は 0、入る 1 体だけ評価)、ShadowPass のカスケード別 run (並べ替え前 12/1/0 draw、後 2/1/0 draw、和 3)。パレット判断は OffscreenCasterNeedsPalette に切り出して RenderSystem と共有 (RenderSystem 全体を回す画素テストではない)。
- #3: Debug --selftest 終了コード 0、Release も 0。
検証: Release / Debug ビルド成功。check_rules 0 error / 50 warning。shot_verify 30 枚 PASS (深度クランプで golden は動かず、round 1 で更新した 3 枚のまま)。render_bench dump の counts は Debug --warp / Release --warp / round 1 と一致。スクショ (受け入れ 7): 一時コード (Tower を z=-300・高さ 400 に変更、コミットせず復元) で、カメラ背後でシーン AABB (地面 z>=-150) よりライト側に離れた Tower の影が床と壁に落ちる (%TEMP%\m90dar.png)。
触ったファイル (round 2 追加): FrustumCull.h, RenderSystem.cpp, ShadowPass.cpp, EditorMain.cpp, ShadowCullSelfTest.h/.cpp (新規), build/Engine.vcxproj(.filters)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-09)
  1. [must] ライト側へ離れた画面外キャスターの影が消える。`RenderSystem.cpp:1444-1451` は `BuildFrustum(lightVPs[c])` の 6 面で判定しており、zNear (`RenderSystem.cpp:459`。画面内の物とカメラのスライスだけで決まる) より光源側にある画面外キャスターは丸ごと落ちる。一部だけはみ出す物は、`ShadowPass.cpp:129` の `DepthClipEnable = TRUE` でクリップされる。どちらも spec §1 の 4 (「画面外のキャスターの影が消えない」) を満たさない (街の高い建物がカメラの背後にある場合など)。修正: カスケードの判定は近平面を除く 5 面で行う。CSM 用だけ `DepthClipEnable = FALSE` のラスタライザで描く (pancaking。局所影アトラスは変えない)。フィットは変えない。受け入れ 7 を参照。golden が動いたら原因を断定して報告する。
  2. [should] 画面外スキンのカスケード経路 (入ったカスケードがあるときだけパレットを評価する) と ShadowPass のカスケード別 run を、常設の selftest で押さえる。#1 の判定関数のテストと同じ場所でよい。最低限、「カスケード別キューの run 数と描画数」と「入ったカスケードが無い画面外スキンはパレット評価 0」の 2 つを検査する。
  3. [should] Debug の `--selftest` の終了コードを取り直して報告する (round 1 は未取得)。
  - 仕様差分の判定: 箱を初回描画時のキャッシュにしたこと、画面外スキンを sceneMin/Max に入れ続けること、revision と統計の追加は、いずれも仕様側の誤りまたは妥当な追加として採用し、spec §8 に記録した。スキンをオクルージョンの判定箱に載せない件は M90 の間は据え置き (spec §7)。
  - 不安・質問への回答: (a) golden 3 枚の更新は了承する (4 点計測で原因を断定できているため)。#1 の修正で動いたら、また同じ手順で断定すること。(b) 箱の余白はこのままでよい。正しさ側に倒れており、詰めるのは計測で効果が見えてから (spec §7)。(c) 画面外キャスターの奥行きは #1 で直す。ComputeCascadeVPs にキャスターの AABB を渡す案は、フィットが変わって golden と解像度配分が動くので採らない。
- round 2: VERDICT OK (planner、2026-10-09)。#1: 判定を近平面を除く 5 面 (CascadeCasterMask) にし、CSM の ShadowPass だけを DepthClipEnable=FALSE にした。ShadowAtlas.cpp:113 は TRUE のまま、フィットも無変更。6 面判定との対照を含む selftest があり、一時シーンのスクショでも確認した。golden は動かず 30 枚 PASS。#2: ShadowCullSelfTest で判定境界・パレット評価の判断・カスケード別 run (WARP) を常設化した。#3: 両構成の selftest が exit 0。nit (申し送り): WorldAabbInFrustumNoNear は WorldAabbInFrustum のループを複製している (面の除外を引数にすれば 1 本にできる)。RenderSystem::Render を通した画面外スキンの影の画素テストは無い。
