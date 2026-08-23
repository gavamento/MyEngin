# M54〜M58: 描画ロードマップ — 局所ライト影 / TAA / SSR・プローブ / ボリュメトリック / 地形

## Context

M52 (決定論の再利用) 全 9 サブ完遂・push 済み・CI 全緑 (`9e72931`) の次。
挙がっていた描画候補 7 件を、依存関係で 5 マイルストーン (計 28 サブ) に割り振って一括計画したもの。

対象 7 件: ①点光源/スポットの影 ②TAA ③SSR ④ローカル反射プローブ/ライトプローブ
⑤フロクセル・ボリュメトリック ⑥デカール ⑦地形システム。

すべて描画専用 = ワールドハッシュ非対象なので `.rep` 互換の作業は原則ゼロ。
代わりに **ピクセル回帰 (`tools\shot_verify.bat`) が唯一の自動被覆**になる。

### 計画時の調査で判明した前提の訂正 (3 点)

1. **`M53` は既に使用済み。** 本計画の起草時点で未コミットだった作業ツリー
   (`src\Editor\AssetPreviewCache.*` / `Windows\InspectorWindow.*` / `Renderer\GpuResources.*` /
   `LocalizationTable.inl` / `AssetOpsSelfTest.cpp` の 9 ファイル、+431 行) が
   「マテリアルのライブプレビュー (M53)」で、`a69c78d` としてコミット済み。
   **描画ロードマップは M54 から始める。**

2. **「TAA — モーションブラー用の速度バッファが既にある」は誤りだった。**
   GBuffer は 4 枚 (albedo / normal / **position** / material) で velocity が無い。
   モーションブラーは深度からのカメラ再投影のみ (`PostProcess.cpp:352-354` が
   「オブジェクト velocity 対象外 = v1 制限」と明記)。カメラジッタも皆無。
   ただし **RT のテンポラル蓄積 (`RtPasses::RtHistory`) は完成済み**で、
   viewKey 別 4 スロット ping-pong + serial 連続性判定 + disocclusion 判定がそのまま流用できる。

3. **★最重要: このままだと新機能はすべてピクセル回帰の被覆ゼロで着地する。**
   - golden 5 シーン全部が `sun.AddComponent<LightComponent>()` の既定 type=0 = **平行光 1 本のみ**
     (`DemoContent.cpp:87, 326, 488, 693, 790`)。点光源もスポットも 1 個も無い。
   - CI 判定の 5 枚中 **4 枚が Forward** (`--deferred` を渡すのは `demo_deferred` だけ)。
     6 枚目 `demo_forward_fxaa` は tol=0 のローカル限定 (`MYE_SHOT_SKIP_FXAA`)。
   - → 局所ライト影・デカール・SSR・プローブ・フロクセル・地形はどれも既定 off にする以前に
     **勝手にピクセル不変**。「golden を緑に保つ」が自明に達成できてしまい、代わりに
     「機能は動いた、しかし壊れても誰も気づかない」が 28 セッション積み上がる。
   - **対策 = M54a でショーケースシーンを先に作る。これがロードマップ最初のコード。**

---

## 実装開始時の手順

1. 1 サブ = 1 コミット (`M54a:` 形式)。進捗の一次情報は git log、
   下の進捗表には計画外の事実・罠・申し送りのみ書く。
2. **マイルストーンを並列で実装する場合は、先に本ファイル末尾の
   「付録: 並列実装の統合契約」を読むこと。** TypeId / シェーダレジスタ / golden スロット /
   `CameraPostFx` の append 順 / ローカライズ接頭辞は**全部そこで予約済み**で、
   表の外の番号を勝手に取ると統合で無言の上書きになる (食い違ってもコンパイルは通る)。

済み: M53 (マテリアルのライブプレビュー) = `a69c78d` / エージェント向けガイド = `4a11e70` /
切り落とし 3 項目の仕様記載 = `engine_spec.md §6.5`。

## 再開手順 (セッション跨ぎ用)

1. `git log --oneline -5` で最後に完了した M5x を確認。
2. 本ファイルの該当サブの節を読んで着手。
3. 全サブ共通検証: Debug/Release ビルド 0 警告 → `Editor.exe --selftest` 全 PASS →
   `pwsh -File tools\check_rules.ps1` 0 error → `tools\shot_verify.bat` PASS。
   ソース追加時は `pwsh -File tools\gen_project_files.ps1`。
4. **受入基準は「機能 off で直前コミットと tol=0 (ビット一致)」**。tol=3 で通しただけでは
   「機種差の余裕 1 レベル」を食い潰した可能性が消えない。差分の数値はコミット本文に残す。
5. `replay_verify.bat` は sim を触らないので原則無風。**割れたら設計を間違えている**
   (描画専用のつもりが sim に漏れた)。`LightComponent` を触る **M54b だけは必ず回す**。

## 進捗表

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M54a ショーケースシーン + golden 2 枚 | 済 | (本コミット — ハッシュは `git log --oneline --grep=M54a` で引く。自己参照になるため表には焼けない) | **WARP 撮影の壁時計 = 1 枚あたり 3.7s** (シーンの重さではなくプロセス起動 + シェーダ実行時コンパイルが支配項 — 既存 `demo_forward` も新 `demo_render_deferred` も同じ 3.7s)。`shot_verify.bat` 全体 = **36.5s / 8 枚** (CI は FXAA を飛ばして 7 枚 ≈ 33s)。**予約 3 の 9 枚 (CI) でも約 41s で CI 予算に収まる** = 8/9 の降格は不要。Forward と Deferred の差は maxDiff=30 (SSAO 分。ADR-007 の主張は「同じライティング式」であって同一画像ではない)。反射床は IBL 無し + SSR 未実装なので**今は黒い板に見える** — M56d/M56f が入るとここが光る (before/after が同じ 1 枚に写る)。同条件 2 回撮影で maxDiff=0 (決定的撮影モードの再現性を実測) |
| M54b ライト選別の決定論化 | 済 | (本コミット — `git log --oneline --grep=M54b`) | **計画の心配 (「ソート順を変えると加算順が変わって画が動く」) は外れた** — `--render-demo` の 5 本は並びが (Sun,SpotL,SpotR,P1,P2) → (Sun,P1,P2,SpotL,SpotR) に**実際に入れ替わっている**のに golden 8 枚すべて maxDiff=0 (ビット一致)。加算順が変われば必ず 1 レベル動く、ではない。カリングも 0 件 (`Lights: 5 selected (culled 0, dropped 0, shadow casters 0)` を毎フレームではなく構成変化時だけログする)。★**既定平行光のフォールバック条件は「選別後 0 本」ではなく「候補 0 本」でなければならない** — 前者にすると画面外の点光源 1 本だけのシーンで、カメラを振った瞬間に太陽が湧いて消える。★`BuildFrustum` の平面は**正規化されていない** (`AabbInFrustum` は p-vertex の符号しか見ないので不要だった) — 球判定で法線長で割り忘れると半径の単位が合わず、遠くのライトが軒並み消える。selftest に 49/51 の境界で歯を入れてある。★**worktree には `MyeScripting.dll` が無い** → C# の `FlowMenu` (タイトルのヒント文字点滅) が動かず `flow_title` が maxDiff=62 / 1073px で落ちる。ライト選別とは無関係の環境差。**各 worktree で先に `tools\build_managed.bat Debug` と `Release` を回すこと**。実測 shot_verify 8 枚 = C# 無効 41.2s → 有効 82.9s だが、この計測は 3 マイルストーンを同時に走らせている最中の値なので CPU 取り合いの分が乗っている (M54a の 36.5s とは条件が違う。CI 予算の判断には使えない)。`kMaxShadowLights=4` と `RenderSystem::lightSelection` の `shadowSlot` は**まだ誰も読んでいない** (M54c で配線) |
| M54c シャドウアトラス + スポット | 済 | (本コミット — `git log --oneline --grep=M54c`) | ★**計画本文の「per-light パラメータは StructuredBuffer で t7/t13」は付録の予約表と食い違う** (予約表では t13 = M56d の SSR)。予約表を正として **CB の末尾 append** (`ShadowTile gShadowTiles[16]` = 1.5KB) に変更した。M54 が取った SRV は Deferred `t12` の 1 本だけ。★**Forward 3 本 (`forward_lit` / `_instanced` / `forward_skinned`) を 1 文字も触らずに済ませた** — `ApplyLighting` に `float localShadow[MAX_LIGHTS]` を足した新版を作り、**旧シグネチャを全要素 1.0 で呼ぶオーバーロードとして残した**。1.0 の乗算は IEEE で厳密なので恒等が保証される。実際 `castShadow=1` を仮に立てた probe ビルドで `demo_render_forward` が **maxDiff=0** (Deferred だけが動く) を実測。★**深度バイアスは CSM から流用できない**(計画の警告どおり)。専用ラスタライザ `DepthBias=120 / SlopeScaled=3.0 / DepthBiasClamp=0.002` + シェーダ定数 0.00015。D32_FLOAT の DepthBias 1 単位は「プリミティブ最大深度の指数から決まる ULP」で、透視では深度が 1.0 近くに寄るので ≒2^-23 — CSM の 800 をそのまま当てると 20 単位先の床で world 1.5 単位ぶん浮く。定数項をほぼ捨てて傾斜依存に寄せるのが正解。★**アトラスは遅延 Init** (4096² R32 = 64MB)。影を投げる局所ライトが 1 本も現れないシーン (既存 golden 6 枚 / AssetPreview) では確保もしない。★スポットの目印球 (`_Marker`) は**ライト位置とちょうど同心**だが、光源視点からは球の内側 = 全面が back-facing で `CULL_BACK` に消えるため自己遮蔽しない (偶然に助けられている。M54d で点光源に同じ形が出たら要確認)。★`SampleShadowAtlas` を計画より 1 サブ早く `common.hlsli` に新設した (M54c で既にサンプルが要るため)。`SampleShadowPCF` の削除は計画どおり M54d に残してある。★probe 実測 (spot 2 本に `castShadow=1` を仮設): Deferred が maxDiff=87 / diffPixels=1591 / anyDiff=2425 で、差分は**全部スポットの光溜まりの中の柱の影**。アクネもピーターパンも出ていない。**golden は 8 枚とも不変** (`castShadow` は既定 0 のまま = M54e で初めて立てる) |
| M54d 点光源 (キューブ 6 面) | 済 | (本コミット — `git log --oneline --grep=M54d`) | ★**計画が要求した「シーン AABB との面カリング」はこのシーンでは 1 面も落とさない** — `--render-demo` の床は 200x200 で遠景も z=88 まであるので、range 18 の点光源の 6 面は全部シーン AABB に触れる。効くのは**タイル毎の視錐台カリング** (計画に無い) の方で、実測 **406 → 117 draws (289 draws 省略)**。面カリングは残してあるが、統計上の意味は「狭いシーンでの保険」。★**カリングされた面もタイル枠は確保したまま `pixelSize=0` にする** — 詰めると `shadowTile + 面番号` の連番が崩れて隣のライトの深度を読む。描かれないタイルはクリア値 1.0 (最遠) のまま = サンプルしても「影なし」に落ちるので、これで正しい。★**面 VP の fov はちょうど 90 度ではない** — `CubeFaceIndex` は絶対値最大の軸でちょうど 90 度で切り替えるので、90 度で焼くと境界画素の 3x3 タップが `SampleShadowAtlas` の clamp に当たって面の継ぎ目に線が走る。`tan(fov/2) = 1 + 2*2/1024` (= 90.22 度) で 2 テクセルの余白を作ってある。★**タイルが足りないライトは `break` でなく `continue`** — 6 枚要る点光源が入らなくても後ろの 1 枚のスポットはまだ入る。選別順に前詰めという規則は保たれるので割当は決定論のまま (スポットしか無かった M54c では break と等価だった)。★probe 実測 (点 2 + スポット 2 に `castShadow=1` を仮設): Deferred が **maxDiff=200 / diffPixels=48972 / anyDiff=77387** で、差分は「点光源から放射状に伸びる楔形の落ち影」。アクネも継ぎ目も出ていない。M54c の spot 単独 (maxDiff=87 / 1591px) から一桁増えるのは、点光源が全方位に影を落とすため。**Forward は maxDiff=0** (M54e まで未配線)。★`SampleShadowPCF` を削除 (計画どおり。M38d の CSM 化以来 5 マイルストーン誰も呼んでいなかった)。★**撮影の壁時計では影のコストが測れない** — 6 フレーム撮影は 4.4〜4.8s でほぼプロセス起動 + シェーダ実行時コンパイル。200 フレームまで伸ばして OFF 最良 17.3s / ON 最良 20.0s (= +13.5ms/frame 相当) だが、**3 マイルストーン同時稼働で分散が 21.9〜30.2s と重なる**ので、この数字は上限の目安でしかない。`GpuTimer` は `ShadowPass` (csm) と `ShadowAtlas` の両方に付けて ProfilerWindow へ出したので、単独稼働時に読めば実値が取れる |
| M54e 3 経路への配線 + UI | 済 | (本コミット — `git log --oneline --grep=M54e`) | ★**Forward 3 本 (`forward_lit` / `_instanced` / `forward_skinned`) は結局 1 文字ずつ触った** — M54c が「オーバーロードで無改造のまま済ませた」のはサンプルしない間だけの話で、配線するには CB の末尾 append + `t6` + `ResolveLocalShadows` の 3 点が要る。代わりに**解決ループを `common.hlsli::ResolveLocalShadows` 1 本に畳んだ** (M54c/M54d が deferred_light に直書きしていたものを移設) — 4 経路に同じループを写経すると「1 箇所だけ面選択を忘れる」がコンパイルを通ってしまう。★`ShadowTileCB` と転置は `RenderTypes.h::FillShadowTilesCB` へ引き上げ (Deferred 光パス / Forward / Deferred 透明後段の 3 者が同じ変換を要求する)。★**Deferred の透明後段は golden で被覆できない** — `--render-demo` に半透明が 1 つも無い。使い捨て probe (床上 16x12 の α=0.9 スラブ) で **atlas ON vs OFF = maxDiff=124 / 13724px**、差分は全部スラブの上の影 = 配線が生きていることを実走確認した (probe は revert 済み)。★**Forward と Deferred の一致 (ADR-007)**: M54e 前 maxDiff=30 / 1046px → 後 **maxDiff=33 / 1139px** (tol=3)。局所影は両経路でほぼ同一に落ちており、残差は従来どおり SSAO 分。★golden は **`demo_render_forward` / `_deferred` の 2 枚だけ**が動いた (fwd maxDiff=200 / 49000px、def maxDiff=200 / 48972px)。**他 6 枚は `--update` を全体に掛けた後も 1 バイト不変** = 「局所ライトの居ないシーンに副作用が漏れていない」の証明。★`View > 影` サブメニューを新設し、`Rendering > 影` の旧トグルはそこへ移した (`Shadow_Directional` = CSM 親 / `Shadow_LocalLights` = アトラス子 + タイル/描画/GPU ms の統計行)。**エディタ GUI の実機目視は未** (M54b からの持ち越し)。★`AssetPreviewCache` は `enableShadows=false` → RenderSystem がアトラス節に入らない → SRV null → `gShadowAtlasEnabled=0` + t6 に null、の 4 段で自然に止まる (コード目視のみ。サムネイルの実機目視は未)。★撮影の壁時計は **shot_verify 8 枚 = 49.0s (M54d) → 53.9s** だが、3 マイルストーン同時稼働の分散が大きく (同条件の 200 フレーム計測で 42.1s と 54.9s が並ぶ) 影のコストとして読める数字ではない |
| M55a `LinearizeDepth` 共有化 | 済 | (本コミット — `git log --oneline --grep=M55a`) | **ローカルコピーは 3 つではなく 5 つあった** (計画の 3 つ + `particle_render.hlsl` / `particle_render_gpu.hlsl`)。5 つとも完全同値 `n*f/max(f-d*(f-n),1e-4)` で、違いは near/far の出所だけ (cbuffer の名前付きスカラ / `gParams2.yz` / `gCollScreen.zw`) = 全部共有版へ寄せられた。**★計画が懸念した「CS から `common.hlsli` を include すると `PerturbNormal` の ddx/ddy で cs_5_0 が落ちる」は起きない** — D3DCompile は未使用関数を検証前に落とす (`deferred_gbuffer.hlsl` の VSMain が同じ理由で通っているのが既存の傍証)。`common.hlsli` は register 宣言を 1 つも持たないので postfx/particle の独自スロット割当とも衝突しない → **意図的な二重定義はゼロ**で着地。**★検証は golden より強い手が取れる**: fxc を engine と同じフラグ (`/Ges /O3`) で回して全 46 シェーダ・80 エントリポイントのバイトコード SHA256 を HEAD と比較 → **80/80 一致**。golden 8 枚 (全部 maxDiff=0) は「実際に使われるシェーダ」しか被覆しないので、シェーダの純リファクタではこちらを主証拠にする (使い捨てスクリプトなのでリポジトリには残していない — 30 行程度、再作成が早い)。`$constGroups` は 2 件とも登録でき、変異注入 (`MAX_LIGHTS` 16→15 / `vps[3]`→`[4]`) で両方が赤くなることを実証。★限界 1 件: `SampleShadowCSM` は `vp0/vp1/vp2` を個別引数で受けるが「1 ファイル 1 整数」の表現力では引数の本数を検査できない (配列長だけ) — HLSL 側にコメントで注意喚起した。CPU ミラーも 1 本化 (`PostFxMath.h::LinearizeDepth` が正本、`ParticleCurves.h::LinearizeParticleDepth` は別名へ縮退) |
| M55b カメラジッタの一元化 | 済 | (本コミット — `git log --oneline --grep=M55b`) | **★worktree に `MyeScripting.dll` が無いと `flow_title` golden が必ず割れる** (実測 maxDiff=62 / 1073px、y=404..413 の 1 行だけ灰 vs 白)。`TitleHint` の点滅は C# レーン (`FlowMenu`) なので、C# ホストが死んでいると点滅せず白のまま撮れる。`tools\build_managed.bat Debug` / `Release` は **sln の外**なので worktree を切っただけでは存在しない — **Wave 1/2 の全ブランチが同じ穴に落ちる**。「golden が 1 枚だけ割れた」を M5x の変更のせいだと誤診する前にここを見ること。★計画は「3 箇所目 = `SceneViewWindow` が二重に組んでいる」としていたが、ジッタを避けるだけでは足りず **`CameraOverride` に proj を渡して構築そのものを 1 箇所へ寄せた**。副作用として **SceneView の Ortho トグルが初めて描画に効く** — これまで overlay / ギズモ / ピッキング (`lastProj_`) だけが ortho で絵は常に透視 = 3 者が食い違っていた (`ComputeCascadeVPs` のコメントが「非 perspective (エディタ Ortho ビュー)」を既に想定していたのが「本来こう繋がるはずだった」傍証)。★`ApplyToProj` は `_34` で透視/正射影を分ける必要がある — ortho は w=1 なので `_31/_32` に足すと view z に比例した歪みになる (selftest が両方を撃つ)。`projNoJitter` を読む側は 6 箇所 = `prevVP_` 保存 / `ComputeCascadeVPs` / 視錐台カリング / `RunMotionBlur` / `ComputeSunScreenPos` / RtPasses デバッグ一次光線。新規ファイルは作らず `PostFxMath.h` へ入れた (M55a が同ヘッダに「共有する描画数式はここへ」と方針を書いているので従った = `gen_project_files` 不要 = 統合の競合が 1 ファイル減る)。実測 (振幅を一時的に 1.0 にした確認): 同一フレーム 2 回撮影は maxDiff=0 (決定的撮影モードで列も決定論)、frame 3 vs frame 4 = 67625px 差、frame 3 vs golden = 34625px 差 = ジッタが実際に載っている |
| M55c velocity バッファ + prev-render ストア | 済 | (本コミット — `git log --oneline --grep=M55c`) | **★このサブは golden では 1 ミリも検査できない** — velocity を読む消費者が M55d まで存在しないので、正しく書けていても間違って書けていても既存 8 枚は maxDiff=0 になる (実際そうなった)。だから機械検査は selftest 2 本 (`TestVelocityUv` / `TestPrevRenderWorldStore`) に置き、実物は可視化モードの**画素分布**で数えた: `--render-demo --deferred --velocity-debug` の frame 3 で **518400 中 514825 (99.31%) がぴったり (205,205,205) = 速度厳密 0**、残り **3575 画素が bbox x=[368..589] y=[254..307]** = Spinner の刃だけ。床/柱/遠景/背景が背景と**同一の 1 色**に落ちることが「静止物の velocity がビット単位で 0」の証拠になる (ジッタ引き戻しと prev-render ストアが両方効いていないとこうならない)。同一フレーム 2 回撮影で maxDiff=0。★**Spinner の 3 枚の刃はインスタンス run** (同 mesh + 同 material が隣接) なので、この 1 枚で `gPrevInstances` の添字整合も同時に検証されている (並びがずれれば刃ごとに出鱈目な色が出る)。★計画は「RenderView に velocity を足す」形を想定していたが、**prevWorld は RenderItem 側 (末尾 append)** に載せた — 前フレーム行列は per-object 量なので view に置き場が無い。★`PrevRenderWorldStore` は RenderSystem.h ではなく **RenderTypes.h** に置いた: selftest (`RenderSelfTest`) が Renderer 層にあり、Engine 層を include できないため (層規則)。★CB は **b4 に GBuffer パス専用の `VelocityParams` を新設**した。PerFrame/PerObject に足すと Forward 3 種 + `ForwardPath.cpp` のミラーが同期対象になるので、代償 (非インスタンス描画 1 回につき CB 更新 1 本増) を払って隔離した。★可視化の感度は **UV 倍率でなく px/frame** で定義 (`kVelocityDebugPxRange=1.0`) — 最初 UV×60 で書いたら 205±3 レベルにしかならず「動いているのに灰色に見える」で判断を誤りかけた。HDR 中間へ描いてトーンマップを通るので、UV スケールでは解像度も露出も絵の意味を変えてしまう。★`AssetPreviewCache` は Forward 固定 + viewKey=0 なので GBuffer もストアも通らない (確認済み)。crash_verify Debug = PASS (GBuffer 5 枚化の VRAM 増を実機で 1 回) |
| M55d TAA 本体 | 済 | (本コミット — `git log --oneline --grep=M55d`) | **★ジッタと TAA は 1 つのスイッチにした** — 別々に切れると「TAA 無しでジッタだけ」= 画面が毎フレーム半ピクセル揺れるだけの状態を作れてしまう。判定は `RenderView::taaEnabled` 1 箇所で、`IRenderPath::WritesVelocity()` (新設、Deferred だけ true) を **Render を呼ぶ前に**見る必要がある (SRV は描いた後にしか無いので、SRV の有無では判定できない)。実証: `--render-demo --taa` (Forward) は golden と **maxDiff=0** = ジッタごと落ちている。★**TAA は Deferred 限定**という v1 制限がここで確定した (velocity が GBuffer RT4 にしかないため)。spec §6.1 に明記。★出力は `t.scene` へ書き戻さず**履歴テクスチャの SRV をそのままチェーンに流す** — 書き戻すと全画面コピーが 1 回増え、次フレームの読み元 (素の描画) も汚れる。そのために `RunDof` の入力を `t.scene` 固定から引数 (`sceneSRV`) へ変えた (DoF だけが入力をハードコードしていた)。★履歴は **(w,h) ではなく viewKey キー** (計画どおり `RtPasses::kHistorySlots` に倣う) + 描画通番の連続性 + リサイズ破棄。★実測 (`--render-demo --deferred --taa`、WARP 960x540 frame 3): golden (TAA off) との差は **anyDiff=79967 / maxDiff=188**、内訳は >1 が 22442、>16 が 8629、>64 が 1395、>128 が 62 = 差の大半が 1 レベルの縁 = アンチエイリアスの分布そのもの。**同一条件 3 回の撮影がバイト一致**。★`--frames 6 --shot-frame 3` = 撮影時点の履歴は 3 枚しかないので「収束後」前提にできない — 履歴無効時に cur をビット単位でそのまま返すことを selftest (`TestTaaResolve`) が固定している (golden は on の 1 枚しか押さえないので、恒等側は絵から確かめられない)。★CI は `MYE_SHOT_SKIP_TAA=1` で 9 枚目を飛ばす (FXAA の前例を踏襲)。**増幅率は未実測** — 実測せずに CI へ載せない判断 |
| M55e モーションブラー v2 | 済 | (本コミット — `git log --oneline --grep=M55e`) | **★計画は「深度再投影を velocity 参照へ差し替え」だったが、差し替えでは絵が悪くなる** — 背景 / スカイは GBuffer を書かないので RT4 が 0 のまま残り、「カメラを振っても空だけ止まる」ことになる。Forward パスにはそもそも velocity が無い。**速度源は画素ごとに選ぶ** (`depth<1` かつ velocity あり → RT4、それ以外 → M44d の深度再投影) 形に変えた。CPU ミラーは `ReprojectUv` を残したまま `motionblur::BlurVector` でその選択規則ごと包む。★**v1 が背景を「動いていないのに」ブラーしていたことが実測で判明**: `--render-demo --deferred --motion-blur 1.0` を旧シェーダで撮ると golden (mb off) に対し **15554 px / maxDiff=4** も動く — カメラ静止でも `inv(VP)·VP` の往復が厳密な恒等にならず、サブピクセル速度が全面に出ていた。新実装は静止ジオメトリで RT4 の厳密 0 を読むので **685 px / maxDiff=75** まで落ち、しきい値 >4 の 225 px は **x=[369..589] y=[254..307] = M55c が velocity 非ゼロと数えた Spinner の bbox と一致**。残る ≤4 レベルは空と点光源まわりの高コントラスト縁 = 再投影フォールバックが担当する画素で、これは v1 由来の既知の粗さ。★**v1 と v2 の A/B はシェーダを差し替えて撮るのが最短** — HLSL は実行時コンパイルなので `git show HEAD:assets/shaders/postfx_motionblur.hlsl` を置いて撮り直すだけで旧挙動が再現でき、再ビルドが要らない (CB を 160 バイトへ広げても旧 144 バイト宣言は余りを無視する / t2 の余分な bind も無害)。この手で **Forward は v1 と v2 が maxDiff=0** = フォールバック経路が 1 ビットも動いていないことを実証した。★golden は 1 枚も足していない (既定 `motionBlurIntensity=0` なので撮っても既存 9 枚と同じ絵になる)。機械検査は `RenderSelfTest::TestMotionBlurVelocity` のみ = **on 側の唯一の自動被覆**。★A/B のために `--motion-blur N` (Editor/Runtime) を足した。UI は足していない — `View > Rendering` に置くと接頭辞が `Taa_*` の予約から外れるうえ、per-camera の Inspector が既にある |
| M55f RT の物体モーションベクトル | 済 | (本コミット — `git log --oneline --grep=M55f`) | **★計画の検証手段 (`--rt-demo` でゴーストが消えるのを見る) は原理的に成立しない** — コーネル箱は被写体もカメラも**完全に静止**しているのでゴーストの出る余地が無い。実測でも old/new が最終画像・蓄積 GI とも **maxDiff=0**。これは「静止シーンでは velocity 経路が旧経路へ**ビット単位で**縮退する」ことの証明にはなるので恒等性の検査として残す価値はあるが、機能の実証は別に要る。★実証は `--render-demo` の Spinner でやるしかないが、**既定の 30 deg/s では画面速度が約 1 px/frame** しか出ず、GI は**半解像度**なので `int2(prevUv * outSize)` の切り捨てで old/new が同じテクセルに落ちて**差が 1 ピクセルも出ない** (実測: Spinner bbox 内の差 0 px。差が出た 1171 px は全部 velocity==0 の**静止**画素 = R16F のワールド座標を往復させる旧経路が 1 テクセル外していた場所で、これはこれで新経路の副次的な改善)。→ `Editor.exe --render-demo --save-scene-on-start` で吐いたシーン JSON をコピーして Rotator の `speedDegPerSec` を 30 → 300 に書き換えた**使い捨てシーン** (`cache\`、gitignore) を `--scene` で読ませて測った。**シーン JSON を手書きしなくてよい**のがこの手の要点。★実測 (300 deg/s、960x540 WARP、frame 20、`--rt-gi --rt-anim-seed`): velocity 非ゼロ = 3914 px (bbox x=[389..594] y=[254..316])。履歴長ヒートマップ (`--rt-debug 6`) が **4026 px 変化 / うち 3700 px が動いている画素**、動いている画素の平均 G は **139.5 → 198.6** (3414 px が緑寄り = 履歴が伸びた / 286 px が赤寄り)。蓄積 GI (`--rt-debug 5`) は >4 の差 2931 px のうち 2629 px が動いている画素、最終画像は差 2579 px (>4 は 26 px)。★**このシーンでのゴーストの出方は「尾を引く」ではなく「動く物体だけノイズが落ちない」だった** — 旧経路は再投影先の深度が合わず履歴を**捨てて**いたので、症状は残像ではなく 1spp のファイアフライ。ヒートマップの刃が橙 (履歴 1) → 黄 (蓄積継続) に変わり、蓄積 GI の刃からファイアフライが消えるのが目視の決め手。★**深度判定は現フレームの P と前カメラの距離のまま**にした (2D の画面速度から前フレームのカメラ距離は復元できない)。1 フレームでカメラ距離が 5% (`kRtTemporalDepthThreshold`) 以上動く物体は今も履歴を落とす = 残像でなくノイズへ落ちる安全側の縮退。spec §6.4 に明記。★CB は `pad[2]` を潰して `useVelocity` を入れた (**128 バイト据え置き** = `static_assert` も不変)。SRV は rt_temporal の **t7** — 予約表 2 の t12-t15/t6-t7 は Deferred 光パスと Forward の話で、CS は別のバインド空間なので競合しない。★A/B は M55e の手 (`git show HEAD:assets/shaders/rt_temporal.cs.hlsl` を置き戻して**再ビルドせずに**旧挙動を撮る) がそのまま効く。pad→int の書き換えも旧宣言は余りとして無視するので安全 |
| M56a デカール (albedo) | 済 | (本コミット — `git log --oneline --grep=M56a`) | ★**計画の「IndependentBlendEnable=TRUE で RT2/RT4 を書込マスク 0」を採らなかった。** デカールは受け面の法線 (RT1) とワールド座標 (RT2) を**同じ draw の中で SRV として読む**ので、そもそも RTV に残したままには出来ない (同一リソースの読み書き二重バインド)。→ **RT0 (albedo) 1 枚だけを bind する**方が「PS が書かなかった RT の内容は未定義」という D3D の規則ごと消えて強い。**M56b は RT1 と RT3 を足すだけにして、RT2 (SSAO/RT/SSR の入力) と RT4 (TAA の入力) は nullptr のまま据え置くこと。** ★**ボックスは CULL_FRONT + 深度テスト無し + DepthClipEnable=FALSE。** 表面を描くとカメラが箱に入った瞬間に消える。**この専用ラスタライザを描画後に `rasterizer_` へ戻し忘れると、後段のフルスクリーン三角形が丸ごと裏面として消える** (SSAO を off にした経路で最初に踏む)。★**GBuffer はゼロクリアなので、空 (ジオメトリ無し) の画素はワールド原点として箱の中に入ってしまう** — `RT2.a < 0.5` の門を OBB 判定より**先**に置くのが必須。★頂点バッファを持たず `SV_VertexID` から立方体 36 頂点を組む (ShaderManager の入力レイアウト構築は SV_ 系を無視して `inputLayout = null` を返す = 既存の仕組みだけで通る)。★CB は **b0** (地形が b4 へ逃げたのは「地形の後に透明後段が来る」ため。デカールの後には SSAO / 光パス / 透明後段が必ず自分の CB を張るので b0 で安全)。★サンプラは s0 = `iblSampler_` (LINEAR/CLAMP) を流用 = 新設ゼロ。CLAMP を選んだので `uvScale` は「繰り返す」ではなく**アトラスの部分矩形を選ぶ**意味になった (spec §6.6 に制限として明記)。★**`--render-demo` にデカール 2 枚を置いた** (計画は「置いた場合のみ golden 更新可」)。置かないとデカールが壊れても `demo_render_deferred` は緑のままで回帰被覆がゼロになるため。代償として **golden 2 枚が動く** — 実測: デカールの寄与 = deferred `maxDiff=132 / 22475 px / 最悪 (216,435)`、taa `maxDiff=132 / 23207 px / 最悪 (220,433)`。TAA の寄与は**前後で maxDiff=188 / 最悪 (413,300) が完全一致** (画素数だけ 70691 → 71613 = デカールの縁も TAA が解くぶん)。`demo_render_forward` は **maxDiff=0** = Forward v1 非対応の証明。残る 8 枚も `--update` 後にバイト不変。★画素サンプルで「床と柱の**天面**には乗り、柱の**側面**には乗らない」を実測 ((203,327) 143,64,43 → 202,84,21 / (200,355) 不変) = 角度フェードが効いていることの直接の証拠。★`DecalSelfTest` を新設 (計画外。連鎖 31 → **32 スイート**)。`invWorld` が world の逆であること・`projDir` が第 3 行であること・`DecalDrawOrderLess` が収集順非依存であること・**`kComponentNoHash` なのでワールドハッシュが動かないこと**を機械で固定する。★`shot_verify` 10 枚 = 68.7s (デカール込みでも baseline 75.0s から増えていない) / `replay_verify` 215.8s |
| M56b デカールの法線 + roughness | 済 | (本コミット — `git log --oneline --grep=M56b`) | ★**強度をシェーダの分岐にせず、ハードウェアのブレンド係数そのものにした**のが中心的な設計判断。`IndependentBlendEnable=TRUE` のとき D3D11 は RT n の `SRC_ALPHA` を「RT n 向けの PS 出力のアルファ」から取るので、`normalStrength` を `SV_Target1.a`・`roughnessStrength` を `SV_Target3.a` に載せると **強度 0 → `src*0 + dst*1` = dst を厳密に維持**が式として保証される (係数 0 の乗算は IEEE で厳密)。probe で実証: RT1/RT3 を**張ったまま**強度 0 にして撮ると **golden 10 枚とも maxDiff=0** (WARP)。★**RT1 は読みながら書く**ので、RTV に張るフレームは先に SRV 専用のコピーを取って角度フェードはコピーから読む (同一リソースの読み書き二重バインドは不可)。コピーは「1 枚でも法線/粗さを書くデカールが居る」フレームだけ = albedo だけで使う限り M56a と 1 命令も違わない。順序も命 — `OMSetRenderTargets(0,...)` で外してからでないと `CopyResource` が RTV に bind 済みのリソースに当たる。★**書込マスクが安全装置**: RT3 は **GREEN のみ** (metallic/emissive をデカールが触ると「色を貼っただけで金属になる」が静かに起きる)、RT1 は RGB のみ (R10G10B10A2 の 2bit アルファは誰も読まない)。RT2/RT4 は RTV を張らないうえ **PS に `SV_Target2`/`SV_Target4` を宣言しない** = 書きようがない (M56a の申し送りどおり)。★TBN は `T = axisX / B = -axisY / N = -projDir`。**B の符号は UV の v 反転 (`uv.y = 0.5 - lp.y`) に由来する**ので、ここだけ見ると符号ミスに見える — `DecalSelfTest` の CPU 鏡で固定した。3 軸の正規化は `FillDecalTransform` の 1 箇所 (シェーダは正規化しない)。★**平坦な法線マップ (0,0,1) は投影方向の逆にしかならない** = 正対した床に真下から貼るデカールは法線マップが無いと 1 画素も変わらない。被覆を作るためにリポジトリに元から居た未使用の `assets\textures\demo_normal.png` (256², リニア読み) を `DecalGround` に付けた。★**2 枚のうち 1 枚だけ**を M56b 対応にしてある — `DecalPillar` は強度 0 のままなので、同じ 1 枚の絵で「albedo だけのデカールは 1 ビットも動かない」が検証できる (実測: 差分の bbox は `(50,317)-(374,449)` = `DecalGround` の footprint に閉じ、柱デカールの画素 (600,350) / (620,340) は完全一致)。★実測 golden: デカールの寄与 = deferred `maxDiff=145 / 19328 px / 最悪 (355,360)`、taa `maxDiff=142 / 19688 px / 最悪 (356,361)`。TAA の寄与は前後で **maxDiff=188 / 最悪 (413,300) が完全一致** (画素数は 71613 → 86761 = 凹凸と鏡面が増えたぶん TAA が解く画素が増えた)。`demo_render_forward` は **maxDiff=0**。残る 8 枚は `--update` 後もバイト不変。★`shot_verify` 69.1s / `replay_verify` 398.2s。selftest は**スイートを増やさず** (32 のまま) `DecalSelfTest` に 16 項目を足した |
| M56c HZB パス | 済 | (本コミット — `git log --oneline --grep=M56c`) | ★**ピラミッドを組む条件を「デバッグ表示が on」1 本にした。** 計画は消費者不在のまま毎フレーム組む前提だったが、WARP での実測が **3.3〜5.5 ms/frame** (RTX 3060 は 0.078 ms) で、shot_verify の 10 枚が全部これを払うことになる。M56d が来るまで払う理由が無いので `RenderView::hzbDebug != 0` を確保もディスパッチも含めた発火条件にした (SSR が入ったら `ssrOn` を or で足す)。**この差 40〜70 倍が「WARP で HZB を常時 on にはしない」の根拠**。★**縮小の被覆区間 `[floor(i*src/dst), ceil((i+1)*src/dst)-1]` が実装の核心。** 素朴な 2x2 で 15→7 を畳むと最後の 1 行 / 1 列がどの出力にも入らず、症状は「壁があるのに HZB が空と答える」= SSR の光線が壁を貫通する、だけ。絵からは永久に追えないので `HzbReduceSpan` を CPU 鏡として置き、`HzbSelfTest` が全段の**穴なし被覆**を機械で確かめる。★src == dst で区間が 1 テクセルに退化する = **段 0 (深度→mip 0) が同じ CS で書ける** (コピー専用シェーダも奇数辺の分岐も不要)。★段ごとに単一 mip の SRV と UAV を作る。全段を覆う `srv_` を張ったまま次段を書くと同一サブリソースの読み書き二重バインドになる。★`RenderTexture` は触っていない (`MipLevels=1` は GBuffer / postfx / SceneView / RT が共有)。RTV も DSV も要らないので素の `ID3D11Texture2D` (`R32_FLOAT`) を自前で持つ方が狭い。★**`GpuTimer` は kFrames=6 のリングなので `--frames 6` の撮影では 1 度も回収されず 0.000 ms を返す** — 計測には 60 フレーム以上回すこと (最初これで「HZB が走っていない」と誤読しかけた)。★可視化は最近傍拡大 (バイリニアだと段の粗さが溶ける) + **ジオメトリ無し = 青** (灰の濃淡だけだと「遠い床」と「min-Z が far のまま残った取りこぼし」が区別できない)。実測: mip 0 と mip 1 は `maxDiff=210 / 36567 px` で別物、`--hzb-debug 11` (段数超過) は mip 10 と**バイト一致** = 頭打ちが効いている。★golden 10 枚は `--update` 無しで **全枚 maxDiff=0** (既定 off の恒等が絵で確認できた)。`shot_verify` 71.0s / `replay_verify` 481.2s。selftest 連鎖 32 → **33** (`HzbSelfTest` 18 項目) |
| M56d SSR | 済 | (本コミット — `git log --oneline --grep=M56d`) | ★**t13 (予約 2 が SSR に割り当てた光パスの SRV 席) を取らなかった。** SSR はライトパスの**出力**を読むので、ライトパスの入力には渡せない (鶏と卵)。ブリーフ本文の指示どおり「光パス直後に加算合成する別パス」にしたため、`nullSrvs` は M54c の **[13] のまま**。**M56f / M57 は自分が足すぶんだけ広げること** (t13 は空き席のまま残っている)。★**加算するのは反射そのものではなく「IBL スペキュラとの差分」**にした。ライトパスは環境スペキュラを既に足しているので、生の反射を上乗せすると同じ光を二重に数える。`(反射 - IBLスペキュラ) * 環境BRDF * 重み` を足すと結果は `ApplyLightingHybrid` の lerp と一致し、重み 0 でちょうど 0 が足される (= off で恒等) のも同じ理由で保証される。フォグも同じ理屈で `(1-f)` を掛ける (係数は `ApplyFog` を黒→白で呼んで取り出す = 式を複製しない)。★**--render-demo にスカイが無いので BRDF LUT を明示的に焼く必要があった。** 焼かないと t6 が null → `SampleLevel` が 0 → 環境 BRDF が 0 → **--ssr を付けても絵が 1 ピクセルも変わらない**。M46h が `enableRtRefl` に対して同じことをしていたのが唯一のヒント。★**実際に踏んだ罠: M56c の HZB 可視化ブロックの発火条件が `hzbOn`** で、そこに `ssrEnabled` を or したので **`--ssr` を付けただけで画面が HZB の可視化に化けた** (518309/518400 画素が変化)。可視化の条件は `view.hzbDebug != 0` に直した — 「組む条件」と「見せる条件」は別物。★交差判定は**セルの入口ではなく出口**と比べる (`tPlane > tCell` なら空セル)。入口 1 点だけ見て「手前だから空」と判断すると、粗いセルの中で光線が面を突き抜ける。★`SsrCellAdvance` は**必ず半画素以上進む**ことを保証する — 境界のちょうど上 / 軸平行で 0 を返すと光線が同じ場所で 64 回の反復を使い切り、症状は「反射が出ない」だけになる。`SsrSelfTest` (計画外の追加、22 項目) がここと roughness フェードの厳密 0、RT 反射との式一致を機械で固定する。★**実測** (960x540 / 10 段 / --render-demo --deferred --ssr): **WARP = トレース 11.0〜12.0 ms + HZB 3.2〜3.5 ms**、**RTX 3060 = 0.113 ms + 0.079 ms** (差 約 100 倍 / 40 倍)。撮影 1 枚の壁時計は SSR 有無で差が測れない (6.1〜6.8s、プロセス起動が支配項)。★golden: **新設 `demo_render_ssr` 1 枚のみ**。`--update` 後 `git status tests/golden` は新規 1 件だけ = 既存 10 枚はバイト不変。SSR の寄与は `demo_render_deferred` 比で **maxDiff=185 / 15442 px / 最悪 (575,286)**。`demo_render_forward --ssr` は **maxDiff=0** = Forward v1 非対応の証明。同条件 2 回の撮影で maxDiff=0 (決定的)。★SSR の交差判定は FXAA/TAA と同型の「しきい値で分岐する」演算なので**機種差が増幅されうる (未実測)**。予約どおり CI 判定 (tol=3) に載せたが、ランナーで理由不明に赤くなったとき golden を撮り直さずに降格できるよう `MYE_SHOT_SKIP_SSR` の口を bat に用意してある。★`shot_verify` 11 枚 = **78.8s** (M56c の 10 枚 68.2s + 11 枚目 1 枚ぶん ≒ 7s。SSR そのものの上乗せは 6 フレーム撮影では測定限界以下)。`replay_verify` 589s — ただしこれは**別マイルストーンと CPU を共有した状態**の値で、同じ混雑下で shot_verify は 148s まで伸びた (M56c 比 2.2 倍)。描画専用の変更なので replay への実質的な増分は無い。selftest 連鎖 33 → **34** |
| M56e プローブのシーンキャプチャ基盤 | 済 | (本コミット — `git log --oneline --grep=M56e`) | ★**「結果は Inspector にサムネイル」だけ実現できなかった** — サムネイルを出す先の `ReflectionProbeComponent` が M56f なので、置く場所が無い。代わりに `View > Rendering` の**明示ボタン** + 専用の小窓 (`Probe_Preview`) にした。M56f はここを Inspector へ移してよい。★**ベイクをメニューのコールバックの中でやってはいけない** — `OnImGui` は ImGui の描画提出の直前なので、そこで 6 面を描くと RTV を握り替えたまま UI の描画が始まる。要求フラグだけ立てて`OnRenderViews` (SceneView / GameView と同じ「描いてよい場所」) で焼く。CLI 側も同じ理由で**スクショ保存の後**に置いた (前に置くとその日の PNG が説明のつかない形で変わりうる)。★**キャプチャは Forward パス固定**。Deferred で撮ると共有 GBuffer 5 枚が 128^2 へ縮んで次フレームに戻り、メイン描画の TAA / SSR 履歴を巻き添えにする。128^2 に SSAO も SSR も要らない。★**postFx を切ると path はリニア放射輝度を RTV へ直書きする** = プリフィルタがそのまま食える。ただし直描き経路は背景色の sRGB→リニア変換をしないので、そこだけベイカ側でやる。★`EnvMapBaker` は「キャッシュ + 空専用」だったので **`BakedEnv` を public 化 + `BakeFrom` / `MapsFor` を追加**し、面基底の表 (`CubeFace`) をヘッダへ出した。プリフィルタと irradiance の式は 1 行も複製していない。**表を 2 つ持つと「撮影した向き」と「サンプルする向き」がずれ、焼いた反射が面ごとに 90 度回るが絵からは追えない** — `ProbeBakerSelfTest` が「面カメラで逆投影した方向 == プリフィルタがサンプルする方向」を機械で固定する (最悪誤差 1.1e-5)。★**継ぎ目チェックの分母をシーンの明るさにして 1 度失敗した** — 平均輝度 0.09 の `--render-demo` では正しい絵でも 0.53 が出てしまい、しきい値がシーン依存になる。**面の内側で 1 テクセル進んだときの差**を分母にすると「継ぎ目は面の中と同じ滑らかさか」という自己校正された問いになる。実測: 正常 **2.66** / +Z を 90 度回した変異 **6.06** (変異注入して実走) → 上限は幾何平均の **4.0**。合成の滑らかキューブでは 0.68 / 7.70。★ただし正常と変異が 2.3 倍しか離れていない = **シーン依存が残る判定**で、別位置 (-14,2,3) では 2.92 まで上がった。最終的な根拠は PNG の目視。★PNG は十字配置 (`+Y` / `-X +Z +X -Z` / `-Y`) — **隣り合う面が画像でも隣り合う**ので、水平の 4 枚が一周するパノラマになり地平線が一直線に繋がるかで向きが読める (2 位置で目視確認)。★実測: ベイク **0.5〜0.6 s/回** (WARP、Debug/Release ともほぼ同じ = WARP 律速。6 面 128^2 + 5mip GGX プリフィルタ + 32^2 irradiance)。**明示指示のときしか払わない**。★golden 11 枚は `--update` 無しで **全枚 maxDiff=0** (新設ゼロ・変更ゼロ)。`shot_verify` 81.4s / `replay_verify` 439.6s。selftest 連鎖 34 → **35** (`ProbeBakerSelfTest` 20 項目) |
| M56f ローカル反射プローブの合成 | 済 | (本コミット — `git log --oneline --grep=M56f`) | ★**光パスに新しい合成式を持ち込まず、M56d と同じ「差分の加算」にした。** `(プローブ - IBL) * ao * 環境BRDF * 重み * (1-wRt)` を色に足すと結果は「スペキュラ放射輝度を lerp で差し替えた」値と一致し、**重み 0 で厳密に 0** = 既定の絵が 1 ビットも動かない。副作用として `ApplyLighting` / `ApplyLightingHybrid` のシグネチャを触らずに済む = **Forward 3 本が 1 文字も動かない**ことが構造的に保証される (計画は「ApplyLightingHybrid の規約で差し替える」と書いていたが、そちらは引数追加 = 4 経路の同期が要る)。★**SSR にも同じプローブ配列を渡すのが必須**だった — SSR が引く基準値を素の IBL のままにすると、SSR とプローブが同時に効く画素でプローブの寄与が二重に乗る。`ssr_trace.hlsl` の `iblSpec` を `lerp(iblSpec, probeSpec*ao, w)` に差し替えるだけで、フォールバック連鎖 (SSR → プローブ → env) が式として閉じる。**実証**: 「SSR」と「SSR + プローブ」の差分画像で、**SSR が当たった画素 (映り込んだ柱そのもの) だけが真っ黒**で、SSR が外した画素だけが動いている。★**実際に踏んだ罠 (M56e の潜在バグ)**: `ProbeBaker::Bake` はプリフィルタへ渡す前に **RTV を外していなかった**。最後の面の RTV が残ったまま同じテクスチャを t0 へ SRV で渡すと D3D は**黙って SRV 側を NULL にする** → そのプローブだけ真っ黒なキューブが焼ける。1 個目は素通り、**2 個目で必ず踏む** (症状は「2 個目のプローブだけ何も反射しない」だけで、ログにも絵にも原因が出ない — Debug の検証レイヤの 1 行が唯一の手がかり)。★t13 は SSR が取らなかった空き席のまま、M56f が取ったのは予約どおり **t14 の 1 本**。`nullSrvs` は **[13] → [15]** (t13 には常に nullptr)。SSR 側は t8。**サンプラ新設ゼロ**。★プローブは 1 本の `TextureCubeArray` にまとめる (光パスの空きスロットが 1 本しかない)。プリフィルタ済みキューブを `CopySubresourceRegion` で `5 mip × 6 面` ずつ写す = プリフィルタの式を 1 行も複製しない。★選択は「一番深く入っている 1 個」で**混ぜない** (v1)。同点は添字が小さい方 + 収集は EntityID 順 = アーキタイプの並びに依存しない。★箱は**軸平行**で、パラメータは**ベイク時点で凍結**する (絵と箱が食い違うと視差補正が嘘をつく)。★golden は **11 枚とも `--update` 無しで maxDiff=0** — `--render-demo` にプローブを 1 個置いたが、ベイクは明示指示のときだけなので絵が動かない (デカールが 2 枚を動かしたのと対照的)。★実測 (WARP、`--render-demo --deferred`): プローブ 1 個の寄与 = `maxDiff=219 / 25856 px / 最悪 (593,289)`、2 個 (scratch シーン) = `60901 px` = 1 個ずつの和とちょうど一致。ベイクは **約 33 ms/個** (IBL シェーダがコンパイル済みのとき。プロセス最初の 1 回だけ 350〜450 ms でコンパイルを払う)。★`ProbeBakerSelfTest` に 20 項目追加 (スイート数は **35 のまま**)。CPU 鏡 (重み / ボックス投影 / 選択) と `kComponentNoHash` を機械で固定する。★エディタ GUI (メニュー 3 項目 / プレビュー窓の複数プローブ / SceneView の影響ボックス) は**実機目視ゼロ** |
| M57a 3D テクスチャ基盤 + WARP 実測 | 未 | | |
| M57b 密度注入 + 局所ライト散乱 | 未 | | |
| M57c テンポラル + 前方積分 | 未 | | |
| M57d フォグ合成の一元化 | 未 | | |
| M57e 透明/スカイ/パーティクル + UI | 未 | | |
| M56a デカール (albedo) | 未 | | |
| M56b デカールの法線 + roughness | 未 | | |
| M56c HZB パス | 未 | | |
| M56d SSR | 未 | | |
| M56e プローブのシーンキャプチャ基盤 | 未 | | |
| M56f ローカル反射プローブの合成 | 未 | | |
| M57a 3D テクスチャ基盤 + WARP 実測 | 済 | (本コミット — `git log --oneline --grep=M57a`) | **①FL11_0 の typed 3D UAV (R16G16B16A16_FLOAT) は WARP でも実 GPU でも動く** = 設計変更なし。「作れた」ではなく **CS が書いた値を STAGING へ読み戻して先頭/中央/末尾の 3 点で照合**して確かめた (末尾を見るのが要点 — `Texture3D.WSize` や境界判定を誤ると最終スライスだけ 0 のまま残る)。②**空の clear CS 1 回の実測** (`Editor.exe --froxel-probe`、Release): 160x90x64 (921,600 セル / 7.03MB) = **WARP 1.04ms / RTX3060 0.026ms**、128x72x48 = 0.46ms / 0.014ms、80x45x32 = 0.17ms / 0.007ms。③**解像度は落とさず 160x90x64 のまま**にした — WARP のスループットが 8 倍のセル数域でほぼ一定 (約 0.9 Gcell/s) = **コストがセル数に線形**で固定費に食われていないため、後から素直に効くつまみとして残せる。注入 (M57b) が WARP で重すぎたらこの表の下段へ落とせばよく、線形なので予測が立つ。★clear は**下限**であって注入のコストではない — CI 昇格の判断材料にはまだならない。④計画の「froxel は RT GI の 7 倍」という見積りは**セル数の比でしかなく時間の比ではない**: RT GI は 1 要素あたり BVH 走査、froxel の clear は 1 要素あたり 8 バイトのストアで、桁が違う。⑤VRAM は 1 枚 7.03MB。M57c のテンポラルが履歴 ping-pong を持つと 3〜4 枚 = 約 21〜28MB になる (計画が「M57a は VRAM を増やす」と名指しした箇所の数字)。⑥`shot_verify` 10 枚 **全部 maxDiff=0** (既存 golden は 1 バイトも動かない — このサブは描画経路に 1 行も繋いでいないので当然だが、`RenderTypes.h` に触ったので確認した)。⑦**グリッドの幾何 (`mye::froxel`) は純関数として `RenderTypes.h` に置き `RenderSelfTest` に足した** (新スイートは作っていない = 連鎖は 31 本のまま。予約 7 と競合しない)。3D UAV が本当に書けるかは GPU が要るので `--froxel-probe` 側の担当、という役割分担。⑧`froxel::kGroupSize`(=8) ↔ `MYE_FROXEL_GROUP` を `check_rules.ps1` の `$constGroups` へ追加済み |
| M57b 密度注入 + 局所ライト散乱 | 済 | (本コミット — `git log --oneline --grep=M57b`) | **①注入の実測が M57a の宿題の回収**: `--froxel-probe`(160x90x64、ライトを光軸上に並べた合成シーン) で **WARP = 固定費 10.0ms + 約 5.5ms/ライト** (0本 9.96 / 1本 15.5 / 4本 33.2 / 16本 95.9)、**RTX3060 = 0.028 / 0.084 / 0.265 / 2.87ms**。実シーン (`--render-demo --deferred`、ライト 5 本・影キャスタ 4・アトラス 14 タイル) は **WARP 60.5ms / RTX3060 1.25ms** (`--froxel-dump`)。**clear (M57a の 1.2ms) は本当に下限でしかなかった** — 固定費だけで 8 倍、影つき実シーンで 50 倍。②**pow は犯人ではない**: スライス深度の `pow` を線形式に差し替えて測っても 10.95ms (変化なし)。HLSL は実行時コンパイルなのでファイルを一時的に書き換えて測るだけでよい (M55e の手)。ただし `pow(x,1.5)` → `x*sqrt(x)` は位相関数側で効くので両方 (HLSL / C++ ミラー) 揃えて直した。③**`pow(f,e)` は底が正だと分かっていても fxc が X3571 を出す** — `max(ratio, 1e-4f)` を噛ませないと実行のたび警告がログに流れる。④**影の実証 = 影あり/なしを同じ実行の中で 2 回注入して読み戻す** (`--froxel-dump N`)。**87,396 セル (9.48%) がアトラスで暗くなり、内向き散乱の総和が 1012.9 → 904.7 (89.3% が残る)**。WARP と RTX3060 で 87396 / 87404 = 8 セル差 = 機種差ではなく half の丸め。**消費者がまだ居ないサブなので、これが「ビームが SampleShadowAtlas 由来である」ことを言える唯一の口**。⑤**プローブの値検査を「> 0」で済ませない** — CPU 側で同じ式 (逆射影 + 減衰 + HG) を組んで期待値を出し 2% 以内で照合する。係数を 10 倍間違えても「> 0」は通るし、**CB のレイアウト崩れも消散係数の厳密比較 1 本で全部落ちる**。⑥**GpuTimer は 6 フレームのリング**なので、6 フレームで終わる撮影実行では 1 件も確定せず **0.000 ms が出る** (最初これで「速い」と誤読しかけた)。ダンプの前に 8 回空回しして確定させてある。⑦`litCells` (= ボリューム最小値より明るいセル) は**ハイトフォグのシーンでは飽和する** (99.98%) — 密度が場所ごとに違うとアンビエントの床が一定でなくなるため。非一様なシーンで意味があるのは ④ の A/B のほう。⑧**平行光をあえて足していない** — 太陽の大気散乱は `ApplyFog` と godray が既に持っており、ここで足すと三重計上になる。役割分担は M57d の議題なので、M57b は「既存の霧と絶対に重ならない範囲 = 局所ライトだけ」に閉じた。⑨CS は PS と別のバインド空間なので t0/s0/u0 を取っても予約 2 (Deferred t12 / Forward t6) と衝突しない。比較サンプラは DeferredPath と同設定 (LESS_EQUAL/CLAMP) を CS 側にもう 1 つ作った — 面の影と霧のビームが別サンプラだと縁が 1 テクセルずれる。⑩**正射影のビュー (SceneView の Ortho) では降りる** — 視錐台がスラブになり「NDC.xy と view 深度から view 座標を復元する」式が成り立たない (1 回だけ WARN)。⑪**既定 off なので golden 10 枚は maxDiff=0**。さらに **`--froxel` を on にして撮った `--render-demo --deferred` が golden と maxDiff=0 (tol=0 でビット一致)** = 「注入は絵に 1 ビットも触れていない」の直接証明。⑫やり残し: **注入コストのセル数スケーリングは未検証** (clear は 8 倍域で線形だったが、注入で同じかは測っていない。グリッド寸法が `froxel::kGrid*` の constexpr なのでプローブから振れない)。UI・CameraPostFx フィールド・積分は予約どおり M57c/M57e |
| M57c テンポラル + 前方積分 | 済 | (本コミット — `git log --oneline --grep=M57c`) | **①`demo_render_froxel` (予約 3 のスロット 12) は撮っていない — M57d へ送った。** 積分結果を読む者がまだ 1 人も居ないので、いま撮れる絵は `demo_render_deferred` と **tol=0 でビット一致する PNG** にしかならない (実測。`--froxel` / `--froxel-no-temporal` の 2 条件とも maxDiff=0 / anyDiff=0/518400)。「同じ絵の 2 枚目」を golden に足すと、以後それが動いたとき原因が M57d か撮影条件かを切り分けられなくなる。**絵が初めて変わるサブ (M57d) が撮るのが正しい**。②**代わりの機械検証は 3 本とも読み戻し**: (a) 積分 GPU 出力 vs CPU 参照 (読み戻した注入結果から同じ前方積分を組む) = **0/921,600 セルが許容外 / max &#124;dT&#124; 0.000488 (= half の量子化そのもの) / 散乱の最大相対誤差 0.098%**、(b) **テンポラルの恒等性** = カメラ静止 + ジッタ停止で履歴を 0.9 混ぜても **0/3,686,400 成分が動かない** (再投影が各セルを自分自身へ写している証明。ずれていれば必ずぼやける)、(c) **ジッタ蓄積** = 8 フレーム回すと単発と max &#124;Δ&#124; 0.0085 違う (= ジッタが本当に CB に載って履歴が生きている)。★(b) と (c) は**対で意味がある** — (b) だけだと「テンポラルが何もしていない」と区別できない。③**積分の解析検算をプローブに入れた**: 一様媒質なら最奥スライスの透過率は厳密に e^{-σ_t(far-near)}。実測 0.278564 vs 解析 0.278594。**スライスを 1 枚ずらす / 厚みを中心差で刻む / 手前と奥を取り違える、どれもこの 1 本で落ちる** (絵では絶対に分からない)。④**格納規約を決めた**: 積分ボリュームのテクセル z = 「スライス z の**奥端**までの積分」。テクセル中心は sliceCoord z+0.5 なので、サンプル側は半テクセル手前 `w=(s-0.5)/count` を指す (`froxel::IntegratedSampleW` が正本)。ここを落とすと霧が 1 スライス手前へずれる。⑤**1 スライスの散乱は解析積分 (1-e^{-σd})/σ** で入れた。厚みの素掛けにすると 1 スライス内の自己遮蔽が消え、濃い霧ほど明るくなる。⑥**計画外の追加ファイル `assets\shaders\froxel_common.hlsli`** (予約 6 に追記)。スライス深度・逆射影・積分係数を 4 本の CS が共有するようになったため。`MYE_FROXEL_GROUP` の正本もここへ移し、`check_rules.ps1` の照合先も 1 本に畳んだ。⑦**実測 (160x90x64、ライト 4 本、`--froxel-probe`)**: WARP = inject 27.9 + temporal 9.1 + integrate 3.3 ms、RTX3060 = 0.270 + 0.074 + 0.051 ms。実シーン (`--render-demo --deferred`、WARP) は inject 41 + integrate 3.2 + temporal 10.1 ms。**テンポラルは注入の 1/4、積分は 1/13** — つまり WARP の支配項は今も注入で、解像度を落とす判断は M57d 以降でも同じ材料で下せる。⑧**VRAM**: 注入 7.03MB + 積分 7.03MB + **履歴は viewKey あたり 14.06MB** (ping-pong 2 枚、使ったビューにだけ遅延確保)。エディタ (SceneView + GameView) で約 42MB。⑨ジッタ列は `camerajitter` と**同じ基数 2 van der Corput・同じ周期 8** で、実装が 2 つある (RenderTypes.h は PostFxMath.h を include できない層) ため **RenderSelfTest が両者の一致を機械照合**し、周期の一致は `check_rules.ps1` の `$constGroups` が見る。通番 0 のジッタは厳密に 0.5 = M57b の代表点。⑩`CameraPostFxComponent` に `froxelOn/froxelDensity/froxelAnisotropy` を末尾 append (予約 4 どおり)。取り込みは TAA と同じ規則 = **シーンカメラに CameraPostFx があればそちらが勝つ** ので、`--froxel` は CameraPostFx を持たないシーンでしか効かない (デモ 4 種はどれも持っていない)。⑪やり残し: **目視ゼロ** (原理的に見えない) / CI ランナー未実行 / `--froxel-no-temporal` は A/B 用の口だけで UI が無い (M57e) |
| M57d フォグ合成の一元化 | 済 | (本コミット — `git log --oneline --grep=M57d`) | **①三重計上は「式を消す」ではなく「距離で割る」で解いた**。`ApplyFog` を froxel on では呼ばない、にすると**グリッドが 64m で切れている**ので遠景の霧が丸ごと消える。代わりに解析フォグの**起点を「視線がグリッドを出る点」まで押し出す** (`froxel::FogHandoffFraction` = gridFarZ/viewZ)。グリッド内のピクセルでは起点が posW と厳密に一致 → 距離 0 → ApplyFog が恒等になり、受け持ちが 1m も重ならない。★起点を動かすだけなのでハイトフォグも太陽インスキャッタも「残り区間ぶん」が自動的に正しくなる (どちらの式も書き直さなくてよい)。②★**`lerp(cameraPos, posW, frac)` で起点を作ってはいけない** — frac==1 でも `a+(b-a)` は b と厳密には一致しない (丸めで最下位ビットが動く)。グリッド内は posW を**そのまま代入する分岐**にしてある。ここがビット恒等でないと近景に霧が二重に乗り、しかも 1 レベルの差なので絵では絶対に気づけない。③**ゴッドレイの自動 off は 2 条件** (`view.froxelSRV != nullptr && path.AppliesFroxel()`)。SRV の有無だけで判定すると Forward + `--froxel` で「ゴッドレイだけ消えて霧が増えない」= 霧が減るだけになる。そのために `IRenderPath::AppliesFroxel()` を足した (v1 で true は Deferred のみ)。潰すのは**そのフレームの実効値だけ**でユーザー設定は書き換えない。④**SRV は t15 に置き t13/t14 を空席のまま空けた** (`gbSrvs[16]` の [13][14] が nullptr)。詰めると M56 と統合したときに無言で潰し合う。`nullSrvs` も [16] へ — 剥がし忘れると**次フレームの積分パスが同じテクスチャを UAV に取った瞬間に D3D が片方を黙って外す**。`froxel::kSrvSlot` ↔ `register(t15)` を `check_rules.ps1` に登録 + `static_assert` で C++ 側の配列位置に結び直した。⑤view 深度は **view 行列の第 3 列をそのまま CB へ載せて `dot(float4(posW,1), row)`**。カメラ前方ベクトルの内積で書くと非一様スケールのビューで静かにずれる。⑥サンプル w は奥端で**止める** (`IntegratedSampleWForDepth`)。外挿するとグリッドより奥にグリッド全体より濃い霧が乗り、解析フォグの残りと足して三重計上が再発する。⑦**golden 予約 12 `demo_render_froxel` を撮った** (M57c からの移送分)。tol=0 のローカル限定 (`MYE_SHOT_SKIP_FROXEL`、CI に env を追加済み)。tol=0 の根拠は実測: **同一引数の 2 回撮影が maxDiff=0 / Debug と Release も maxDiff=0** = テンポラル + 深度ジッタが決定的撮影モードの上で完全再現する。CI へ載せなかった根拠も実測: (a) TAA と同型の「フレーム間に蓄積するしきい値分岐」で OS ビルド差の増幅率が未測、(b) WARP の注入が実シーンで 42〜44ms/フレームなので 1 枚で `shot_verify` が **68.2s → 80.2s**。⑧**実測**: `--render-demo --deferred --froxel` は golden `demo_render_deferred` に対し **maxDiff=80 / 381,036 画素 (73.5%) が変化** (最悪画素 578,241)。同じシーンの froxel off は **maxDiff=0** = 「`ApplyFog` の式に 1 ビットも触れていない」の直接証明。`shot_verify` 11 枚すべて maxDiff=0 で既存 10 枚は 1 バイトも動いていない。⑨`--froxel-dump` の 3 検査は維持 (0/921,600 セル許容外 / 0/3,686,400 成分 / ジッタ蓄積 max|Δ| 0.0085)。ただし**ダンプ後に `Render` をもう 1 回回すようにした** — `DebugDumpAB` は検査のためにボリュームを塗り替えるので、消費者が付いた M57d 以降はそのままだと「ダンプしたフレームだけ絵が違う」になる (M57c までは露見しようがなかった)。⑩やり残し: 透明 / スカイ / パーティクル / Forward `t7` / UI は M57e。**スカイに霧が乗らないので地平線に段が出る**のが今いちばん目立つ穴 (絵で確認済み)。froxel の密度は `CameraPostFx.froxelDensity` 由来で `FogComponent.density` とは別物 — 高度プロファイルだけ共有している |
| M57e 透明/スカイ/パーティクル + UI | 済 | (本コミット — `git log --oneline --grep=M57e`) | **①計画が名指しした 3 者 (透明 / スカイ / パーティクル) のほかに口が 2 つ足りなかった**: (a) **`deferred_light` の `albedo.a < 0.5` 早期 return** = スカイボックスを持たないシーンの背景。`--render-demo` がまさにそれで、M57d が「今いちばん目立つ穴」と書いた地平線の段の正体はここ (スカイボックスパスは `skyMode < 0` で 1 命令も走らない)。(b) **Forward パス本体**。予約 2 の Forward `t7` は「Deferred の透明後段が forward_lit を使うから」という理由で取ってあったが、同じシェーダなので Forward の不透明も自動的に手に入る。`ForwardPath::AppliesFroxel()` を true にして完結させた (M57d が false にした理由 = 合成が無いのにゴッドレイだけ降りる、が解消したため)。②**スカイに専用の SRV スロットを与えてはいけない** — スカイボックスは不透明と透明の**間**に挟まるパスなので、ここで t1 を取ると Forward で後段の半透明が読む CSM を潰す (実装中に気づいて撤回)。既にホストが張っている **t7 / s2 に相乗り**する形にし、Deferred 側だけは「光パスが t0-t15 を剥がした後」なので呼ぶ側で t7 を張り直す。同じ理由で **SkyboxPass は SRV もサンプラも自分では張らない**。③**加算合成の粒子に内向き散乱を足してはいけない** — 背後のサーフェスが既に 1 回足しているので、加算で重ねるたびに足すと粒子の枚数ぶん霧が濃くなる。加算が受け取るのは透過率だけ (`froxel::CompositeFroxelAdditive`)。alpha 粒子は `scene·T + inscatter` (既存の「フォグ色へ lerp」と同じ形)。RenderSelfTest が「8 枚重ねても inscatter は 1 回きり」を機械で見ている。④**粒子の解析フォグも受け持ちを分ける**: `dist * (1 - FogHandoffFraction(viewZ, farZ))` = グリッドを出てから粒子までの残り区間だけ。グリッド内は乗算で厳密に 0m = 恒等。⑤**スカイのサンプル w は最遠ジオメトリと厳密に同じテクセルでなければならない** (`IntegratedSampleWFar` == `IntegratedSampleWForDepth(viewZ>=farZ)` を selftest が `==` で検査)。1 テクセルずれると M57e が消しに来た段がそのまま残り、しかも「なんとなく境界が見える」で見逃す。⑥**グリッドより奥の解析フォグはスカイには掛けない** (v1 の判断)。空に `ApplyFog` が掛かる挙動は M29d 以来一度も無く、掛けると濃霧で skybox が丸ごとフォグ色に潰れる。フロクセル区間ぶんの段だけを消す。⑦**t7 を剥がす場所が 3 つ増えた** (ForwardPath / DeferredPath の Render 末尾 + CpuParticleBackend の t3)。光パスの `nullSrvs[16]` **より後**にスカイと透明後段が張り直すので、あれだけでは足りない。⑧**forward_terrain.hlsl の PerFrame は M54e ブロックを持たない切り詰めミラーだった** — 末尾 append するには先にそれを足してオフセットを合わせる必要がある (足さずに書くと地形の霧パラメータが全部ずれる)。⑨計画外の修理 1 件: **ForwardPath が半透明の前にサンプラを張り直していなかった** (cubemap スカイが s0 を LINEAR/CLAMP にしたまま残す M38b からの潜在バグ)。golden に cubemap スカイのシーンが無いので絵は 1 枚も動かない。⑩**実測**: `shot_verify` は既存 10 枚すべて maxDiff=0 / `demo_render_froxel` だけが **maxDiff=61 / 139,678 画素 (26.9%) / 最悪画素 (387,185)** = 背景ぶん。新 golden も tol=0 の根拠を取り直した (同一引数 2 回 maxDiff=0 / Debug↔Release maxDiff=0)。⑪**透明・スカイ・粒子の被覆は golden に無い** (`--render-demo` に半透明もスカイも無く、足すと `demo_render_deferred` と `demo_render_taa` が動いて予約 3 に違反する)。代わりに **一時プローブ** (showcase へ半透明 2 枚 + gradient スカイ + additive/alpha の粒子塊を足した uncommitted ビルド) で Deferred **maxDiff=70 / 509,902 画素 (98.4%) / 最悪画素 (428,233) = 遠景の alpha 粒子**、Forward **maxDiff=70 / 509,905 画素** を実測し、目視で 3 者すべてに霧が乗ることを確認してから revert した。⑫やり残し: GPU パーティクルバックエンド (そもそも解析フォグも持っていないので据え置き) / 歪み粒子 (色を出さないので対象外) / VfxRenderer の Sprite・Trail・TextMesh (計画にも無い。次の穴はここ) / エディタ GUI の実機目視 |
| M58a 地形アセット + クック (.mterr) | 済 | (本コミット — ハッシュは `git log --oneline --grep=M58a` で引く) | ソース = `.terrain.json`、クック = `.mterr` (小ヘッダ + レイヤ表 + R16 + RGBA8 の生バイト)。**計画に無い追加: 画像を 1 枚も要求しない「手続き生成」経路** — デモ地形をバイナリ非同梱でリポジトリに置けて M58b/c が即座に被写体を得られる (値ノイズ + fBm、格子値は FNV 整数ハッシュ由来なので実時間も `rand()` も混ざらない)。★**`CookedCache` の deps は存在しか見ない**ので、画像の**中身を焼き込む**地形には足りない (`.terrain.json` を触らずに PNG だけ差し替えると古い絵が出続ける)。取り込み画像の size + FNV ハッシュを blob に持たせて `Load` で照合する (封印キャッシュ中は元画像が無いので跳ばす)。selftest が「同サイズの内容改変」で実証。★スプラットは cook 時に**最大剰余法で合計 255 へ量子化正規化**する = M58d のシェーダ側が正規化を持たなくてよい。★`BuildSettingsWindow` は 2 箇所要る: 拡張子リストだけでなく **`StageCookWarm`** も — 地形はシーンに置かれるまで誰もロードしないので、温めが無いと dist が空になる。実測: 129x129 R16 + 128x128 RGBA8 + 4 レイヤ = payload **99,329 bytes**、`--package` の sealed dist に入ることを確認。★**worktree の罠 (M58 とは無関係だが後続が必ず踏む)**: 新しい worktree には `MyeScripting.dll` が無く、`--flow-demo` が C# の `FlowMenu` を attach せずにシーンを保存するため golden `flow_title` が 1073 画素ずれる。`tools\build_managed.bat Debug` と `Release` を両方走らせてから `shot_verify` を回すこと |
| M58b チャンク生成 + カリング | 済 | (本コミット — ハッシュは `git log --oneline --grep=M58b` で引く) | `TerrainComponent` は `char source[64]` の**相対パス**でアセットを指す (AssetID にしない): `DrawAssetRef` はフィールド名から Mesh/Material/Texture 等のランタイムライブラリを推定する作りで、地形にはそのライブラリが無い。相対パスならシーン JSON がチェックアウト先に依存しない (M51j の絶対パスハッシュ問題を踏まない)。★**`TerrainData::HeightAtTexel` の引数は `uint32_t`** なので、法線の中心差分で `x-1` (x==0) を渡すと 0xFFFFFFFF が `std::min` で**反対側の端**に落ちる — 符号付きでクランプする入口 (`HeightClamped`) を必ず通すこと。★法線は**チャンクではなく地形全体の texel 座標**で差分を取る (チャンク内で閉じると縁の法線が両側で食い違い、継ぎ目がライティングの線として出る)。selftest が共有列/共有行のビット一致で実証。★チャンク AABB の高さ範囲は担当タイル + 1 の**頂点**範囲で取る (タイル範囲だと縁の頂点が AABB から溢れてカリングが可視物を落とす)。selftest の「AABB が全頂点を含む」がその歯。★`viewZ` はチャンク AABB 中心で作る — エンティティ原点だと全チャンク同値になり M58e の LOD もソートも成立しない。★selftest のローカル変数に `near` / `far` を使わない (windows.h のマクロで潰れる)。実測: 129x129 の同梱デモ地形を `chunkTiles=32` で 4x4 = 16 チャンク。**CLAUDE.md の「29 スイート」は 30 に増えたが、M54b も同じ行を触るので統合時にまとめて 31 へ直すこと** (この行だけの衝突を避けるため本コミットでは触っていない) |
| M58c 地形描画パス | 済 | (本コミット — ハッシュは `git log --oneline --grep=M58c` で引く) | ★**地形 CB は b4** に置いた。b1-b3 (PerObject / MaterialParams / ボーン) を張り替えると**地形の後に描かれる透明メッシュが地形の CB を読む**ので、ホストが張り直さなくて済む空きスロットを使うのが唯一きれいな解。b0 (PerFrame) はホストパスが張ったものをそのまま読む (`deferred_terrain` は前半だけ宣言、`forward_terrain` は forward_lit の完全ミラー)。★**Forward 系 CB の同期対象が 4 本になった** — `forward_lit` / `_instanced` / `forward_skinned` / **`forward_terrain`** + C++ ミラー 2 箇所。★**地形は「面」であって「塊」ではない**ので、視線が手前の縁の高さより下を通ると裏面カリングで消え、画面下端に空色の帯が出る (最初の試写で実際に出た。バグにしか見えない)。デモカメラは `atan((camY - 最大高さ) / 縁までの距離) > 俯角 + 縦画角の半分` を満たす位置に置いてある (50m / 14m / 32m → 52° > 44.5°)。★golden は `--terrain-demo` の**専用シーン**で撮る (`--render-demo` に足すと M54a の golden 2 枚が動き、同じ Wave の他ブランチと PNG で衝突する)。★`RenderSystem::assetsRoot` が空 = 地形を 1 枚も収集しない = `AssetPreviewCache` の専用 RenderSystem に地形が混ざらない元栓 (「配線の 2 箇所目」を 1 箇所に畳んだ)。★描画順 `TerrainDrawOrderLess` は純関数として `TerrainPass.h` に出して selftest が検査 (同 viewZ のタイブレークが AssetID = 規則 7)。★`check_rules.ps1` の `$constGroups` に `kTerrainObjectCbSlot ⇔ register(b4)` を登録 (変異 1 件で検出を実証)。実測: 撮影の壁時計 **4.2s** / 9 枚の shot_verify 全体 **41.8s** (WARP・Release)。**既存 golden 8 枚は maxDiff=0 で不変**。**★統合時 (M55c): `deferred_terrain.hlsl` の `PSOut` に `float2 velocity : SV_Target4;` を足して 0 を書くこと** (静的地形の画面速度は 0) |
| M58d スプラットブレンド 4 層 | 済 | (本コミット — ハッシュは `git log --oneline --grep=M58d` で引く) | ★**計画に無い追加 2 件**: ①`TerrainLayer` に **tint (authored sRGB 3 float)** を足した (`.mterr` blob **v1→v2**)。素材画像を 4 種コミットするのは「生成物はコード側から組み直す」既存の流儀 (parts / flow) に反するので、**画像を 1 枚も足さずに 4 層を見分けられる**手段が要る。②レイヤ用の共有 HLSL **`assets\shaders\terrain_common.hlsli`** を新設 (予約 6 の表に無い新規ファイル)。deferred / forward の 2 本が**同じ地表**を出すことがこのサブの主張なので、ブレンド本体は 1 箇所にしか置けない。**`common.hlsli` には置けない** — あちらは「register 宣言を 1 つも持たない」ことを契約に cs/postfx から include されており、こちらは register を 9 個持つ (`ibl_common` / `rt_common` と同じ「用途別の共有ヘッダ」の流儀)。★**SRV は t20..t28** (splat=t20 / albedo=t21-24 / normal=t25-28)。t0-t7 はホストの持ち物で t12-t15 / t6-t7 は他 M の予約席なので、**誰とも隣り合わない位置**へ逃がした。`check_rules.ps1` に 4 群 (t20 / t21 / t25 / レイヤ数 4) を登録し、変異 2 件で検出を実証。★**サンプラは 1 つも増やしていない** (予約 2 に従い、ホストが張った s0 = 異方性 WRAP を層に、s2 = LINEAR CLAMP を スプラットに流用)。★**シェーダに分岐を 1 つも置かない**のが設計の芯: 未設定レイヤには 1x1 白 / 1x1 平坦法線 (128,128,255) を bind して**常に 9 枚サンプルする**。`if` で読み分けると Sample が非一様フローに入り `PerturbNormal` の ddx/ddy が壊れる。★重み正規化は **cook 側 (合計 255 へ量子化) + シェーダ側 (有効フラグを掛けてから合計 1 へ再正規化)** の二段。後者が無いと「レイヤ 4 未満のとき殺したチャンネルの重みで有効レイヤの色が痩せる」= 「なんとなく暗い」としか見えない壊れ方をする。★`TextureLibrary::CreateFromRgba8` を新設 (スプラットは `.mterr` の中にしか無い生成物)。**名前に中身の FNV ハッシュを混ぜる** — 同名先勝ちなのでパスだけをキーにすると M58f のブラシで焼き直しても古い重みが出続ける。★**golden `demo_terrain_deferred` は撮り直した** (地表の見た目そのものが変わるサブなので不可避。**既存 8 枚は maxDiff=0 で不変**)。旧 golden との差は maxDiff=128 / 326745 画素 = 地形の面積ぶんだけ (空と柱は無変化)。実測: Debug と Release が**ビット一致**、同条件 2 回撮影も**ビット一致**、Forward と Deferred の差は maxDiff=24 (SSAO と GBuffer 法線量子化のぶん = 期待どおり) |
| M58e チャンク LOD + クラック対策 | 済 | (本コミット — ハッシュは `git log --oneline --grep=M58e` で引く) | ★**スカート深さを勘で決めない**のがこのサブの設計の芯。`ComputeMaxLodEdgeGap` が「隣接チャンクが LOD la/lb のとき共有する縁に生じうる最大の縦の食い違い」を実測し、その 1.25 倍 (`kTerrainSkirtMargin`) をスカートにする。この不等式が成り立てば 2 枚の面の縦区間が必ず重なる = **穴が幾何的に開きえない**ので「絵を見て隙間が無さそう」を卒業できる (1〜2 画素の筋は目視では判定不能)。margin 1.25 は幾何ではなく**ラスタライズの逃げ** (ちょうど接する 2 面は頂点を共有しないのでフィル規則しだいで 1 画素残る)。★**インデックス縫合を採らなかった理由**は計画の「2 サブぶん」だけではない: 隣の LOD を見て縁を組み替えると**チャンクのメッシュがカメラ位置の関数になり**、キャッシュが毎フレーム崩れる。スカートならメッシュはカメラから独立したまま。★**外周の縁にはスカートを出さない** — 隣が居ない縁は割れようがないのに、出すと地形の外側に垂直な壁が立って絵に映る (地形は面であって塊ではない)。selftest が「角チャンク = 2 辺 / 縁チャンク = 3 辺 / 単一チャンク = 0 辺」で固定。★**スカートの表はチャンクの外向き**。周回の向きを 4 辺で揃える (-X は z 昇順 / +Z は x 昇順 / +X は z 降順 / -Z は x 降順) と三角形パターン 1 つで足りる。裏返っていると裏面カリングで消え、**絵の上では「隙間が塞がっていない」のと区別が付かない**。selftest が幾何法線とチャンク中心の内積で検査。★**AABB は「分割 → 段差の計測 → 拡張」の順にしか組めない** (計測が layout を要る) ので `ExpandLayoutForSkirt` を後掛けにした。下げないと「上面は視錐台の外だがスカートは見えている」チャンクをカリングが落とす。★**index を `chunk.tilesX` で回すと LOD で間引いた分だけ頂点数を飛び越す** — LOD 0 では偶然一致するので気付けない。セル数は必ず LOD 後の格子 (vx-1, vz-1)。★**`TerrainLodSamples` の末尾は必ずチャンクの端**にする (端数タイルは最後のセルだけ短くする)。stride の倍数で切り上げ/切り捨てにすると、同じ LOD の隣接チャンクどうしですら縁を共有できない。★**selftest の fixture が非線形でないと、クラック検査が何も検査しないまま緑になる** (実際に一度そうなった): 旧 `MakeTestTerrain` は高さが x と z の 1 次式なので間引いても補間が厳密に一致し、最悪段差 0.0000 m で PASS した。さらに素の FNV でも隣 texel の差が抽出ビットで 1 しか動かず 1.16 m 止まり — **雪崩化 (finalizer) を足して初めて 17.94 m** の worst case になった。判定も `> 0` ではなく `> 1.0 m` にしてある。★**実測** (`--terrain-demo --deferred`、WARP・Release、960x540): LOD off は golden と **maxDiff=0** / LOD 80 で 126992 画素が変化 (maxDiff=141) / LOD 40 で 295389 画素 (maxDiff=170) / **スカート有無の差 = LOD 80 で 1542 画素・LOD 40 で 1732 画素** (これがスカートが塞いでいるクラックそのもの。ヒートマップはチャンク行の境界に一直線に並ぶ) / 同条件 2 回撮影はビット一致。撮影 1 枚 約 4.2s。★**計画からの逸脱**: 検証は「カメラ距離を変えた 2 枚」ではなく `--terrain-lod` の値を変えた 2 枚にした。LOD 選択は viewZ のみの関数なので、切替距離を半分にするのはカメラを半分に寄せるのと**どのチャンクがどの LOD になるかの点で等価**で、かつ画角内の他の要素が 1 画素も動かないぶん差分が LOD だけを写す。CLI は `--terrain-lod DIST` / `--terrain-skirt D` (Editor / Runtime 共通)。★**LOD は既定 off** (`TerrainComponent.lodDistance = 0`) なので golden `demo_terrain_deferred` は撮り直していない (既存 9 枚すべて maxDiff=0) |
| M58f ブラシペイント + Undo | 済 | (本コミット — ハッシュは `git log --oneline --grep=M58f` で引く) | ★**編集結果の置き場所を新設した** (計画は「ブラシ + Undo」としか言っていない): `.terrain.json` の隣の**サイドカー `.terrain.edit`** (小ヘッダ + R16 + RGBA8 の生バイト)。JSON は「地形の作り方 (寸法・レイヤ・ノイズ種・元画像)」のレシピで、ブラシが変えるのは**画素**なので、`CookFromSource` がレシピを解いた**後**に被せる = 「解像度が一致する限りブラシ結果が勝つ」の 1 本の規則になる (手続き生成の地形にも画像由来の地形にも同じ規則が効く)。★**`.mterr` blob v2→v3** — サイドカーの刻印 (`editSrc`) を 3 本目の `TerrainSourceImage` として追加。これに伴い **`CookedCacheSelfTest` の `kHeightCountOff` が 104 → 124** になった (統合時に blob を触る人はここも直す)。★**クックキャッシュの穴を 1 つ塞いだ**: `SourceImageStillMatches` は relPath が空だと「外部入力なし」で素通りするので、**無かったサイドカーが後から現れた**ケース (よそのプロセス / VCS が置いた) をキャッシュが隠す。`.terrain.json` は 1 ビットも変わらないので mtime も deps も miss を出せない — `EditSidecarStillMatches` が「ディスク側の存在」と「blob の主張」を突き合わせる。**この判定を外す変異で selftest が 1 本だけ落ちることを実走確認**。★**Undo は矩形パッチ** (`UndoFileOp::Kind::TerrainPaint` = M51i のファイル操作エントリの 4 種目)。全量スナップショット 2 枚だと 1 ストロークが 4097^2 で 66MB になる。往復の厳密さは「差分の外は 1 バイトも違わない」という `MakeDiffPatch` の不変量に乗っているので、selftest は重なる 5 ダブのストロークで undo→バイト一致→redo→バイト一致、さらに 3 往復のドリフト無しまで見る。★**ブラシも Undo/Redo も同じ永続化経路 (`TerrainEdit::SaveEdits`) を通す。** 「塗るときは A、戻すときは B」に分けるとクックキャッシュの更新漏れが片方でだけ起き、「Undo したのに絵が戻らない」という直し方の分からないバグになる。★**平滑化は書き込み前の高さから読む** (矩形を 1 texel 広げたコピーを作る)。自分の書き込みを読み返すと走査順が結果に効く = 「決定論だが順序依存」という一番厄介な状態になる。検査は「**X 鏡映してから平滑化して鏡映し戻すと元と一致する**」— ブラシは centerX=0 について対称なので、順序依存があるとここだけが割れる (**読み元を `d.heights` に変える変異で実際に 1 本だけ落ちた**)。★**ダブは距離間隔 (半径の 1/4) で置く。** カーソルを止めたまま押しっぱなしで塗り続けると穴が底まで抜けるうえ、1 ダブ = 1 回のサイドカー書き出しなのでディスクも回り続ける。★**imgui 1.92.8 は `AddPolyline` も thickness と flags が入替**わっている (旧順序は `= delete`。M51f の `AddRect` と同じ罠)。★**selftest のローカル変数に `small` は使えない** — `rpcndr.h` の `#define small char` で潰れる (節 (12) の `near`/`far` と同じ)。★**実測** (Release、129x129 + 128x128 スプラット + 4 レイヤ、16 チャンク): サイドカー **98,842 bytes** / ダブ 1 回の永続化 (ブラシ + サイドカー書き出し + クックキャッシュ更新) **7.8 ms** / チャンク全再構築 (キャッシュ読み + 分割 + 16 メッシュ登録) **2.2 ms** = **1 ダブ 約 10 ms**。ライブプレビュー用の「ディスクを経由しない差し替え API」は**採らなかった** — 支配項がメッシュ再構築側なので 節約は 3 割程度で、経路が 2 本に増える対価に見合わない。★golden は**撮り直していない** (9 枚すべて maxDiff=0)。サイドカーの無い地形の絵は 1 画素も変わらない。★**新規ファイル `TerrainEdit.{h,cpp}` は予約 6 の表に無い** (M58d の `terrain_common.hlsli` と同じ扱い。表に追記済み)。地形の**オーサリング**は描画側の `TerrainSystem` にもクック側の `TerrainAsset` にも属さないため。★**未検証 = エディタ GUI の実機目視** (ツールバーのトグル / ブラシリング / 3 モード / Ctrl 反転 / Shift 平滑化 / Ctrl+Z)。自動化不能なのでユーザー同席時に。**塗りモードはダブごとにスプラットテクスチャが 1 枚増える** (名前が中身のハッシュ = M58d の意図的な設計) ので、長時間の塗りで TextureLibrary が太る — v1 の既知の制限。★M59 への申し送り: `TerrainEdit::SampleHeightLocal` / `RaycastLocal` が**高さ場の問い合わせの雛形**(いまはエディタ専用。sim から呼ぶと地形がハッシュレーンに入る) |

---

## マイルストーン構成

| M | 主題 | 候補 | サブ数 | 相対工数 |
|---|---|---|---|---|
| **M54** | 局所ライトの影 | ① | 5 | 1.0 (基準) |
| **M55** | テンポラル基盤と TAA | ② | 6 | 1.3 |
| **M56** | スクリーンスペースの表面と反射 | ⑥ + ③ + ④ | 6 | 1.4 |
| **M57** | フロクセル・ボリュメトリック | ⑤ | 5 | 1.1〜1.6 |
| **M58** | 地形システム | ⑦ | 6 | 1.6 |

参考: M51 = 10 サブ、M52 = 9 サブ。

### 順序の根拠

- **M54 が先頭** — 絵の穴が最大で、新しい器を 1 つも要求しない (既存 `ShadowPass` の拡張)。
  かつ M57 のフロクセルが「局所ライトの光芒」のために M54 のアトラスを直接サンプルする。
- **M55 (TAA) が M56 (SSR) より先** — ハード依存ではない (決定論的な固定ステップ SSR なら独立で、
  SSR が要るのは深度/法線/HZB/ライトパス出力だけ = 全部フレーム内で揃う)。それでも先行させるのは
  ⓐTAA が本ロードマップ最大の CI リスクなので早期に踏んで挽回の余地を残す、
  ⓑ副産物としてモーションブラーの v1 制限と RT のゴースト (spec §6.4) を回収する、
  ⓒ後から確率版 SSR に書き直す手戻りを避ける、の 3 点。
  **M55 が荒れたら M56 を先行させてよい — この順序に強制力は無い。**
- **デカール (⑥) は M56 に置く** — M55 で GBuffer に 5 枚目 (velocity) が増えるので、
  デカールを M54 に置くと M55 が書き込みマスクを直しに戻ることになる。
  SSR/プローブとは「GBuffer と深度をスクリーン空間で読み直す」機構を共有する。
- **地形 (⑦) が最後** — 結合度は最低だが最大サイズで、アセット/クック/配布/LOD/エディタの
  5 レーンを跨ぐ。失敗しても他をブロックしない。

### 切り落とし (意図的に入れない)

| 項目 | 理由 |
|---|---|
| **RT 影の局所ライト拡張** | RT レーンは既定 off かつ CI 撮影から除外済み (`shot_verify.bat:86`「RT デモは WARP では重すぎる」) = 自動被覆が永久にゼロ。M54 のアトラスが同じ絵を出す。RT でしか得られないのはソフトシャドウの正確さだけでコストに見合わない。spec の「local lights cast no ray-traced shadows」は v1 制限のまま残す |
| **拡散 SH プローブグリッド** | 拡散環境光の実装が既に 2 つある (IBL irradiance / RT 拡散 GI + SVGF)。SH グリッドは 3 つ目で品質は RT GI に劣り、ベイク基盤 (N 点キャプチャ → SH 射影 → グリッド化 → シリアライズ) を丸ごと要求する (2〜3 サブ) |
| **地形コリジョン** | 候補に無い。付けた瞬間に sim/ハッシュレーンへ入り `replay_verify.bat` の 5 ペア目が要る。M59 送り |
| **ABI (`EngineAPI.h`) の bump** | **M54〜M58 で 1 スロットも増やさない。** デカール生成もライトの `castShadow` も TAA/SSR の on-off も、既存の `Instantiate` (v7) + `SetComponentField` (v11) + `CameraPostFxComponent` 末尾 append で叩ける。プローブの再ベイクは「その場で GPU 6 面キャプチャ」= フレーム時間を壊すので公開しないのが正しい。`TerrainSampleHeight/Normal` が要るのは地形コリジョンが入るとき (M59 で v14 として 1 回 bump) |

---

## 全マイルストーン共通の規約

**各サブの受入基準** (spec §6.4 / M46 が確立した家風):

> その機能を **off にした撮影が、直前コミットの PNG と `--img-diff` で tol=0 (ビット一致)**。
> on 側は `--img-diff` の数値と目視で「意図した箇所だけ変わった」を示す。

**踏む手順**:

- ソース追加 → `pwsh -File tools\gen_project_files.ps1`。新シェーダには `.meta` が要る
  (`{"guid","type":"shader","version":1}`、例 `assets\shaders\rt_temporal.cs.hlsl.meta`)。
  シェーダは実行時 `D3DCompile` でキャッシュ無し。
- C++/HLSL 共有定数 → `tools\check_rules.ps1` の `$constGroups` に登録 (規則 9) + 双方に相互参照コメント。
  **既存の未登録ペアもここで回収する**: `kMaxLights=16` ↔ `MAX_LIGHTS` ↔ `MYE_RT_MAX_LIGHTS`、
  `ShadowPass::kCascades=3` ↔ `SampleShadowCSM` の `vps[3]`。
- UI 文字列 → `LocalizationTable.inl` に en/ja 両方 (規則 10)。`###` の右辺は両言語一致かつ一意。
- 定数バッファのフィールドは**必ず末尾 append + 既定値=恒等**。Forward 系 CB を触ると
  `forward_lit` / `forward_lit_instanced` / `forward_skinned` の 3 HLSL + C++ ミラー
  `ForwardPath.cpp:18-52` が同期対象 (`deferred_gbuffer*` は先頭だけ宣言なので影響なし)。
- コンポーネントは `RegisterBuiltinComponents()` の**末尾 append** (`Components.cpp`。
  現在の最大 TypeId = 32 = `PlayerInputComponent`、次は 33)。描画専用なので全部 `kComponentNoHash`
  → `WorldHasher.cpp:163` がコンポーネント丸ごとスキップ = `.rep` 互換の作業ゼロ。
  `EditorComponentCatalog.cpp` にアイコン/カテゴリ/日本語名も足す (未登録でも動くが Unknown 分類になる)。

**★「配線の 2 箇所目」チェックリスト** (毎回ここで漏れる):

1. Deferred 光パス (`DeferredPath.cpp:684-751`)
2. **Deferred の透明後段 Forward** (`DeferredPath.cpp:759-839` — Forward の CB/SRV を張り直している)
3. ForwardPath 本体 (`ForwardPath.cpp`)
4. **`AssetPreviewCache` の別 RenderSystem** (`AssetPreviewCache.cpp:296-312`。`enableShadows=false`、
   Forward 固定、`depthSRV` は意図的に null)。新 SRV が null のときのゲートを入れないと
   **サムネイルだけがゴミをサンプルする**

**空きスロットの現況**: Forward は SRV `t6+` / サンプラ `s3+`。Deferred 光パスは SRV `t12+` / サンプラ `s2+`。
`GpuLight` (64B 固定) に `pad0/pad1`、`LightComponent` に `pad` が空き。ステンシル 8bit は完全に未使用。

---

## M54 — 局所ライトの影

### M54a: 描画ショーケースシーン + golden 2 枚

- `DemoContent.cpp/.h` に `--render-demo`: 大スケールの床 (200×200) + 分散した柱 +
  **スポット 2 + 点光源 2** + 金属度 0.9 / 粗さ 0.1 の反射床 + 高さフォグ + 遠景オブジェクト。
  M54〜M58 の全機能が「見える」1 本にする。
- 再利用: `BuildFlowTitleScene` 系の組み立て、`--flow-demo` の CLI 配線 (`EditorMain.cpp` / `RuntimeMain.cpp`)。
- `tools\shot_verify.bat` に `demo_render_forward` / `demo_render_deferred` の 2 枚 (tol=3、CI 対象)。
- 検証: 再撮影でビット一致。**既存 5 枚が無変化**。
  **WARP 撮影の壁時計を計測してログに残す — 以降の CI 予算の起点になる。**

### M54b: ライト選別の決定論化

- 新 `Engine\Engine\LightSelection.h/.cpp` (**純関数**) — 範囲球 × 視錐台カリング +
  決定論キー (type → `entity.index` 昇順) でソート + 「影を投げる上限 N 本」の抽出。
  現状 `RenderSystem.cpp:335-378` は**カリングもソートも無い登録順の先着 16 本**。
- `LightComponent::pad` (`Components.h:60-70`) を `int32_t castShadow` へ**置換** —
  sizeof も既存フィールドの offsetof も 1 バイト動かない。`Components.cpp:58-66` に FieldDesc 1 行。
- 再利用: `FrustumCull.h:31 BuildFrustum` / `:57 AabbInFrustum`、`RenderQueue::Sort` の決定論キー流儀。
- 検証: **このロードマップで唯一まともに selftest 化できるサブ**。`LightSelectionSelfTest.cpp` を
  `EditorMain.cpp` の連鎖に足して 29 → 30 スイート。`LightComponent` はハッシュ対象なので
  `replay_verify.bat` も回す (golden.rep は毎回録り直しなので無風のはず)。golden 全枚ビット一致。

### M54c: シャドウアトラス基盤 + スポット (透視 1 面)

- 新 `Renderer\ShadowAtlas.h/.cpp` — 4096² `R32_TYPELESS` 1 枚 + 矩形割当。
  `shadow_depth.hlsl` / `shadow_depth_instanced.hlsl` は **pos しか読まない最小 VS なので無改造**で
  透視 lightVP をそのまま流用できる。
- 再利用: `ShadowPass.cpp:44-74` の「TYPELESS + D32 DSV + R32 SRV」生成手順、
  `ShadowPass::Render` のインスタンシング run 構築 (`BuildInstanceRuns` / `MeshInstanceBuffer` — run は
  カスケード間で 1 回だけ構築する最適化がそのままタイル間でも効く)、
  `ShadowPass.cpp:145-149` の `RSSetViewports` を per-tile へ。
- SRV は Forward `t6` / Deferred `t12`、per-light パラメータは StructuredBuffer で `t7` / `t13`。
  `GpuLight::pad0/pad1` (`RenderTypes.h:177-178`) にアトラススロット index を載せれば
  **`Light` 配列部分の HLSL ミラー 3 箇所 (`common.hlsli:31-42` / `rt_common.hlsli:56-66`) は無改造**。
- **★罠: 深度バイアスの単位が違う。** `ShadowPass.cpp:94-103` の `DepthBias=800` /
  `SlopeScaledDepthBias=2.5` は正射影 CSM 用に調整された値。透視は NDC 深度が非線形なので、
  同じ値ではアクネかピーターパンのどちらかが必ず出る。**ラスタライザステートを分ける前提で。**

### M54d: 点光源 (キューブ 6 面 → アトラス 6 矩形)

- 再利用: `EnvMapBaker.cpp:29-36` の `kFaces[6]` (D3D 面順 +X,-X,+Y,-Y,+Z,-Z の forward/right/up basis)
  をそのまま持ち込む。
- `common.hlsli` に `SampleShadowAtlas` を新設。**このとき `SampleShadowPCF` (:46-63) を削除する** —
  M17 の遺物で現在どこからも呼ばれていない。新実装の下敷きとして読んでから消すのが自然。
- **★罠: draw 数の爆発。** 点光源 1 個 = 6 面。`--render-demo` の点 2 + スポット 2 で
  既存 CSM 3 パス + 14 パス = 5 倍。**面カリング (シーン AABB と交差しない面は描かない) を最初から**
  入れないと WARP 撮影が計測不能に遅くなる。GpuTimer を ProfilerWindow に出す
  (現在 `ShadowPass` には GpuTimer が付いていない)。
- **★罠: アトラス枠の割当が frame 間で不安定だと影がポップする。** 距離ソートを避け、
  M54b の決定論キー (entity.index 昇順の先着 N) をそのまま使う。

### M54e: 3 経路への配線 + UI

- 「配線の 2 箇所目」チェックリストの 1〜4 全部。Deferred の `nullSrvs[12]` → `[14]`。
- **★罠: サンプラの張り直し。** `DeferredPath.cpp:728-732` が既に警告している —
  SSAO パスが `s1` を point-wrap で上書きするので光パスで必ず張り直す (忘れると影が全消え)。
  アトラス用に新サンプラを増やさず **`s1` の比較サンプラを流用**するのが安全。
- `View > Shadows` サブメニュー (`EditorApp.cpp:571` の隣) + `LocalizationTable.inl`。
- 検証: **Forward と Deferred の絵が一致すること**を `--img-diff` で数値化 (ADR-007 の主張)。
  `AssetPreviewCache` のサムネイルが壊れていないこと。

---

## M55 — テンポラル基盤と TAA

### M55a: `LinearizeDepth` の共有化

- 現在 3 シェーダにローカルコピーで散在 (`postfx_dof_prefilter.hlsl:38` / `postfx_dof_composite.hlsl:40` /
  `particle_sim.cs.hlsl:13`)。`common.hlsli` に共有版が 1 つも無い。
- 再利用: `PostFxMath.h` の「HLSL 式の CPU ミラー + `RenderSelfTest.cpp` で検証」パターン
  (`HeightFogEffectiveDistance` が前例)。
- **検証が本体**: 式を 1 文字も変えないので **golden 全枚ビット一致**。1 枚でも動いたら移動が非等価。
  `common.hlsli` の変更は全依存シェーダを再コンパイルさせる (ファイル冒頭に明記あり)。

### M55b: カメラジッタの一元化

- `RenderSystem` が「非ジッタ proj」と「ジッタ proj」の 2 本を持つ。組み立ては
  `RenderSystem.cpp:282-285` (CameraOverride) と `:322-325` (シーンカメラ) の 2 箇所、
  加えて `SceneViewWindow.cpp:117-119` が**二重に組んでいる**ので 3 箇所。
- **★罠: `prevVP_` に保存するのは非ジッタ側。** `RenderSystem.cpp:764-772` の 1 箇所を
  `RtPasses` (M46d) と `PostProcess::RunMotionBlur` (M44d) の**両方**が読む。
  ジッタ付きを保存すると RT テンポラルとモーションブラーが同時に壊れる。
- ジッタ列は frame index 由来 → 決定的撮影モード (`EngineLoop.cpp:1047-1052` で frame==tick) で
  自動的に決定論。`rtFreezeSeed` の「撮影時に自動で決定化し明示フラグで解除」(`EngineLoop.cpp:242-243`)
  が前例。
- 検証: 振幅 0 で golden 全枚ビット一致。

### M55c: velocity バッファ (GBuffer 5 枚目) + prev-render 行列ストア

- `DeferredPath.cpp:337-363` を MRT 5 本化 (`R16G16_FLOAT`)。
  `deferred_gbuffer{,_instanced,_skinned}.hlsl` に prevWorld を追加。
- **★★最重要の罠: `PrevWorldStore` は「前 tick」であって「前フレーム」ではない。**
  `TickRunner.cpp:172-186` の `CapturePrevWorld` は tick 頭に採取し、描画は
  `LerpWorld(prev, cur, interpAlpha)` (`RenderSystem.cpp:401, 419-420`)。前フレームに**実際に描かれた**
  行列は `LerpWorld(prev, cur, alpha_前フレーム)` であって `prev` ではない。`prev` をそのまま
  velocity に使うと最大 1 tick 分過大になり TAA が履歴を外し MB が過剰にブレる。
  **さらに悪いことに、決定的撮影モードでは dt 固定で accumulator がちょうど 0 に戻る =
  `interpAlpha == 1.0` なので、この誤りは golden に一切現れない — 対話プレイでのみ出る。**
  → viewKey 別の「前フレームに描いた world 行列」ストアを新設する。
- 再利用: `RtPasses::RtHistory` (`RtPasses.h:105-122`) の viewKey×4 + serial 連続性判定 + リサイズ破棄。
- **v1 制限として明記**: 前フレームボーンパレットが無いのでスキンメッシュは velocity 0
  (カメラのみ再投影へ縮退)。`parts` golden にスキンメッシュがある。
- 検証: velocity 可視化のデバッグモード (`rtDebugMode` の隣) を作って目視。

### M55d: TAA 本体

- 新 `Renderer\TaaPass.h/.cpp` + `assets\shaders\postfx_taa.hlsl`。
  `PostProcess::Resolve` (`PostProcess.cpp:644-764`) のチェーン先頭 (DoF の前) に挿入。
- 再利用: `rt_temporal.cs.hlsl:64-89` の `RtReprojectValid` (カメラ距離の相対差 + 法線 cos) と
  履歴長重み、`RtPasses::Accumulate` (`RtPasses.cpp:398-474`) の構造。
- **★罠: history を `PostProcess::Target` の (w,h) キー LRU に置かない。** SceneView と GameView が
  同サイズだと履歴を食い合う。`RtPasses::kHistorySlots=4` (viewKey 別) が既に解決済みの設計。
- **★★本ロードマップ最大の CI リスク。** M52c 追補の実測: 開発機 (10.0.26100) と CI ランナー
  (10.0.20348) で素の描画は maxDiff=1 なのに **FXAA 1 パスで 35 に増幅**した
  (近傍輝度のしきい値分岐が ULP 差で反転すると、ブレンド係数ごと変わる)。
  TAA の近傍 min/max クランプはまさに同じ分岐で、しかも history でフレーム間に蓄積する。
  → **`demo_render_taa` を tol=0 のローカル限定 7 枚目とし、CI は `MYE_SHOT_SKIP_TAA` で飛ばす。**
  FXAA の前例 (`shot_verify.bat:93-102` の `SHOTBASE`/`SHOT` 分離) をそのまま踏襲する。
- 注意: `--frames 6 --shot-frame 3` = 撮影時点で履歴は 3 フレームぶんしかない。
  「収束後の絵」前提の実装にしない。

### M55e: モーションブラー v2

- `postfx_motionblur.hlsl` の深度再投影を velocity 参照へ差し替え。`PostProcess.cpp:352-354` の
  「カメラのみ = v1 制限」コメントを削除。CPU ミラー `PostFxMath.h:108-130 ReprojectUv` と
  `RenderSelfTest.cpp:307` も更新。
- 検証: 回転する Spinner (`DemoContent.cpp:99`) がブレる。`motionBlurIntensity=0` で golden ビット一致。

### M55f: RT の物体モーションベクトル対応

- `rt_temporal.cs.hlsl:124` の再投影 (prevViewProj のみ) を velocity 併用に。
  同ファイル :12-13 の v1 制限コメントと spec §6.4 の「moving objects ghost」を削除。
- 検証: `--rt-demo` (ローカル、CI 対象外) でゴーストが消えることを `--img-diff` + 目視。

---

## M56 — スクリーンスペースの表面と反射

### M56a: デカール (投影ボックス、albedo)

- `DecalComponent` (TypeId 33、`kComponentNoHash`) を `Components.cpp` 末尾へ。
  挿入点は `DeferredPath.cpp:565` (ジオメトリパス直後・SSAO 前)。新 `assets\shaders\decal_project.hlsl`。
- 再利用: 既存 `blendAlpha_` (`DeferredPath.cpp:281-290`)、`depthSRV` + `dsvReadOnly` の同時バインド
  (`RenderSystem.cpp:698-700`)。**GBuffer RT2 にワールド座標そのものがあるので深度逆投影が不要** —
  `gPosition.Load()` でワールド位置を取り、デカールの逆行列で OBB 内判定 → UV 生成が最短。
- **★設計判断: `IndependentBlendEnable=TRUE` で RT2 (position) と RT4 (velocity) を
  `RenderTargetWriteMask=0` で塞ぐ。** RT2 は SSAO/RT/SSR の入力、RT4 は TAA の入力。
- **ステンシル 8bit がリポジトリ全体で完全に未使用** (全 DepthStencilState が `StencilEnable=FALSE`)
  なので、必要ならデカール受け側のマスクに使える。
- 検証: `--render-demo --deferred` で床に貼れる。デカール 0 個で golden 全枚ビット一致。
  **Forward は GBuffer が無いので v1 非対応**と明記。

### M56b: デカールの法線 + roughness 書き込み

- **★`PerturbNormal` (`common.hlsli:105-119`) の微分ベース TBN は投影パスでは使えない**
  (posW の ddx/ddy が投影ボックスの面のものになる)。デカール自身の OBB 基底から TBN を作る。
- 分離可能なサブにしてある — 膨らんだら v1 から落として制限として明記してよい。

### M56c: HZB パス

- **★`RenderTexture` にミップを足さない。** `RenderTexture.cpp:21` の `MipLevels=1` 固定は
  GBuffer / postfx 中間 / SceneView RT / RT パス全部が使う共有クラスで、触ると影響範囲が全描画。
  新 `Renderer\HzbPass.h/.cpp` + `assets\shaders\hzb_reduce.cs.hlsl` で min-Z ピラミッドを自前で持つ。
- 再利用: `GpuBufferUtil.h` の `CreateConstant`/`UploadCB`、
  `RtPasses.cpp:307-319 UnbindCompute` の「SRV/UAV/CS を null で明示解除」作法、
  `DeferredPath.cpp:666-668` の「CS 前に `OMSetRenderTargets(0,nullptr,nullptr)`」(実際に踏んだ罠)。
- 検証: ミップ各段をデバッグモードで可視化。**WARP での壁時計を必ず計測**。

### M56d: SSR (階層 Z トレース)

- ライトパス出力を読んで加算合成。粗さでフェード。挿入は光パス直後。
- 再利用: **`RtReflWeight` (`common.hlsli:235`) をそのまま流用** — SSR → プローブ → IBL の
  フォールバック重みを RT 反射と同一式にすれば段差が出ない。
- **★罠 1: `gbPosition` (R16G16B16A16F) を交差判定に使わない。** 原点から離れると 16F の精度が破綻する。
  `view.depthSRV` から復元する (M55a で共有化した `LinearizeDepth` を使う)。
- **★罠 2: SSR が読むのはライトパスが書いている最中の RT。** RTV を外してから SRV で読み、
  別 RT へ書く。`sceneB` は DoF/MB が使うので `PostProcess::Target` に 1 枚追加するか衝突を確認する
  (`distort` を後から足した M42d が前例)。
- 検証: `--render-demo` の反射床が映る。off で golden 全枚ビット一致。
  **golden 8 枚目 `demo_render_ssr` (tol=3、Deferred、CI 対象)**。

### M56e: 反射プローブのシーンキャプチャ基盤

- `EnvMapBaker` は**位置の概念が無く、シーンをキューブ 6 面へ実描画する機能が存在しない**
  (ソースは cubemap SRV か gradient 解析式の 2 択 = 空しか焼けない)。新 `Engine\Engine\ProbeBaker.h/.cpp`。
- **★罠: `RenderSystem::Render` は再入不可** (`queue_` / `skinPalettes_` / `viewSerial_[]` / `prevVP_[]` を
  インスタンスで持つ)。6 面を再帰呼び出しすると RT テンポラルの serial が 6 進んで履歴が全滅する。
  → **`AssetPreviewCache.cpp:296-312` の「専用 RenderSystem インスタンスを持つ」解に倣う。**
- 再利用: `EnvMapBaker` の prefilter (128²cube 5mip) / irradiance (32²cube) / BRDF LUT (256²) の
  シェーダとキューブ生成 (`CreateCube`) をそのまま。プリフィルタ処理を関数分離して共有する。
  `RenderView` は POD なので view/proj/rtv/dsv を差し替えて `IRenderPath::Render` を 6 回回すだけ。
- **★ベイクは明示ボタン (自動ベイクにしない)。** 「見えたらベイク」にすると撮影ごとに結果が変わり
  決定的撮影が壊れる。結果は Inspector にサムネイル表示。

### M56f: ローカル反射プローブの合成 (ボックス投影)

- `ReflectionProbeComponent` (TypeId 34、`kComponentNoHash`)。
  フォールバック連鎖 **SSR → ローカルプローブ → グローバル env**。
- 再利用: `ApplyLightingHybrid` (`common.hlsli:251-`) の「同次元の放射輝度を smoothstep で差し替える」
  規約 — RT GI/反射が既に同じことをしている。プローブ配列の GPU 常駐は
  `RtScene.h:23-83 GpuArray` (容量成長する StructuredBuffer + SRV) が雛形。Deferred SRV は `t13+`。
- 検証: プローブ 0 個で golden 全枚ビット一致。Forward は v1 非対応と明記。

---

## M57 — フロクセル・ボリュメトリック

### M57a: 3D テクスチャ基盤 + WARP 実測プローブ

- **`Texture3D` / `RWTexture3D` はリポジトリ全体で 0 件。** `RenderTexture` は 2D 専用。
  新 `Renderer\VolumeTexture.h/.cpp` + `assets\shaders\froxel_clear.cs.hlsl`。
- **★設計を決める前に WARP で 160×90×64 の空 CS を回して壁時計を測る。**
  `shot_verify.bat:86` が「RT デモは WARP では重すぎる」と明記している。froxel 921,600 セルは
  RT GI (960×540×0.5² = 130k ピクセル) の**セル数だけで 7 倍**。
  **実測値が計画に載ること自体がこのサブの成果物**で、遅すぎたら 80×45×32 へ落とす判断をここで下す。
  FL11_0 の typed 3D UAV が WARP で本当に動くかも同時に確認する (ここでこけると設計をやり直し)。
- 再利用: `GpuBufferUtil.h`、`RenderTexture` の「毎フレーム Resize を呼ぶ (no-op で通過)」パターン。

### M57b: 密度注入 + 局所ライト散乱注入

- `assets\shaders\froxel_inject.cs.hlsl` + 新 `Renderer\FroxelPass.h/.cpp`。
- 再利用: **M54d の `SampleShadowAtlas` をそのまま呼ぶ** (これが「局所ライトのビーム」の実体) +
  M54b の `LightSelection`。ProfilerWindow に GpuTimer 行。

### M57c: テンポラル + 深度スライスジッタ + 前方積分

- `froxel_temporal.cs.hlsl` / `froxel_integrate.cs.hlsl`。
- 再利用: `RtHistory` の viewKey 別 ping-pong + serial 判定 (3 度目の複製)。
- golden 追加は**実測してから判断** — テンポラルが入るので tol=0 のローカル限定になる公算が高い。

### M57d: フォグ合成の一元化 (三重計上の解消)

- **★`ApplyFog` (`common.hlsli:306-347`) は `deferred_light` と `forward_lit*` の両方の中で呼ばれ、
  そこに froxel の積分結果と `postfx_godray_*` のスクリーンスペース放射ブラーが重なる。**
  3 つとも大気散乱を表現していて、素直に足すと霧が 3 倍になる。
- 既定を「froxel on なら `ApplyFog` の距離項を froxel へ委譲、godray は自動 off」に確定する。
  godray は遮蔽マスクが「空だけ」の下位互換なので、README / engine_spec の表を
  **「froxel の簡易版 (低スペック向け)」へ書き換える**のが誠実。
- 検証: froxel off で golden 全枚ビット一致 (= `ApplyFog` の式が 1 ビットも変わらない)。

### M57e: 透明・スカイ・パーティクルへの適用 + UI

- froxel は「そのピクセルまでの積分」なので深度を持つ全描画物に適用が要る。
  `DeferredPath.cpp:759-839` の透明タイル / `SkyboxPass` / `RenderSystem.cpp:701` のパーティクル。
  **パーティクルは加算合成なので適用しないと浮く。**
- Rendering メニュー + `LocalizationTable.inl`。

---

## M58 — 地形システム

### M58a: 地形アセット型 + クック (.mterr)

- ハイトマップ (R16) + スプラットマップ (RGBA8) + レイヤ定義。
  新 `Engine\Engine\Asset\TerrainAsset.*`、`CookedCache` に新種別 (`kCookVersion` の扱いに注意)。
- 再利用: **`Audio\AudioClip.cpp:259-315` (.mpcm) が最小の雛形** (小ヘッダ + 生バイト列)。
  ハイトマップの取り込みには `Renderer\TextureCook.h:16 CookImageToDds` が使えるが
  R16 は BCn 非対応なので `compress=false` 経路。
  ソースアセット種別を増やすなら `AssetDatabase.h:13-29` の `AssetType` (**append-only**) +
  `ClassifyPath` / `TypeName` / `ParseTypeName`。
- **★罠: `BuildSettingsWindow.cpp:253` の拡張子リスト (`.mmdl` / `.mpcm`) に `.mterr` を追記する。**
  忘れると配布物に入らず、しかも `--package` を叩かない限りローカルでは絶対に再現しない。
- 検証: `CookedCacheSelfTest.cpp` に往復 + **破損 blob の境界検査**テスト
  (検算なしで `bad_alloc` 即死の実績あり)。`--package` の中身に `.mterr` が入ること。

### M58b: チャンクメッシュ生成 + カリング

- `TerrainComponent` (TypeId 35、`kComponentNoHash`) + 新 `Engine\Engine\TerrainSystem.*`。
- 再利用: `MeshLibrary::Register` (span API、AABB 自動計算)、`WorldAabb` / `AabbInFrustum`。
  `MeshVertex` にタンジェントは不要 (`PerturbNormal` の微分 TBN で足りる)。
- 検証: チャンク分割と AABB は**純関数なので selftest 化できる** — `TerrainSelfTest.cpp` を連鎖に足す。

### M58c: 地形描画パス (Deferred + Forward の 2 本)

- **★罠: Deferred の不透明パスは `material->shader` を見ない** (`DeferredPath.cpp:509-519` で
  GBuffer シェーダ固定 3 種を bind。Forward は見る)。マテリアルとして通そうとすると数時間溶ける。
  **専用パス一択** — 新 `Renderer\TerrainPass.h/.cpp` + `deferred_terrain.hlsl` / `forward_terrain.hlsl`。
- M55c で 5 枚になった GBuffer への出力規約に合わせる。
- 検証: **golden 9 枚目 `demo_terrain_deferred`**。

### M58d: スプラットブレンド (4 レイヤ)

- **★罠: `Material` はテクスチャ 2 枚まで** (albedo + normal)。4 レイヤ × (albedo + normal) = 8 枚は入らない。
  `Material` を拡張すると `GpuResources.cpp:975` の「マテリアル JSON の唯一の本体」とシーン互換に波及する。
  **地形は `Material` を使わない設計にする。**
- UV は**ワールド XZ 由来** (`MeshVertex` に 2 セット目 UV を足さない)。
- 検証: 4 レイヤの重みが 1 に正規化されること (selftest 可)。

### M58e: チャンク LOD + クラック対策

- **LOD 機構も空間分割もエンジンに存在しない**ので、地形が両方の最初の実装になる。
  クラックは**スカート方式** (1 セッション) を採る。インデックスバッファ縫合は 2 セッション。
  LOD 選択の材料は `RenderItem::viewZ` が既にある。
- 検証: LOD 境界に隙間が出ない。カメラ距離を変えた撮影 2 枚を `--img-diff`。

### M58f: ブラシペイント + Undo

- `SceneViewWindow` にブラシ (高さ上げ下げ / 平滑化 / スプラット塗り) + `LocalizationTable.inl`。
- 再利用: **M51i のアセット操作 Undo エントリ** (`UndoStack` + `AssetOps::ExecuteAssetFileOp`) の流儀 —
  ファイル実体を書き換える操作を Undo に載せる既存パターン。
  フォーカス判定は `ImGuiFocusedFlags_RootAndChildWindows`。
- 検証: Undo/Redo の往復でハイトマップがバイト一致 (**selftest 化できる**)。
- **エディタ GUI の実機目視は自動化不能** — ユーザー同席時に。

### 申し送り (M59 候補)

- **地形コリジョン** (`ColliderComponent.shape=4` ハイトフィールド)。`shape=3` (mesh) が M41 で予約済み。
  **sim/ハッシュレーンに入る**ので `replay_verify.bat` に 5 ペア目が要る。
  同時に **ABI v14** で `TerrainSampleHeight` / `TerrainSampleNormal` の 2 スロットを append
  (`Interop.cs` の位置ミラー / `EngineApiTable.cpp` の全スロット充填 / `check_rules.ps1` の
  `$apiVersionSlots` を規則 11 に従って同時更新)。地形の高さは機種依存の値ではないので sim へ
  書き戻してよいが、**`.mterr` のクックがバイト決定論であることが担保条件**。
- 地形へのレイキャストに新スロットは要らない — ハイトフィールドコリジョンを入れれば
  既存 `Raycast` (v3) / `SphereCast` (v4) がそのまま通る。

---

## 検証

### 各サブの終わりに毎回

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
```

- **受入は「機能 off で直前コミットと tol=0」**。tol=3 で通しただけでは
  「機種差の余裕 1 レベル」を食い潰した可能性が消えない。
- 差分が出たら `--img-diff A B --tol N` の数値をコミット本文に残す (隠さないことがこのテストの価値)。
- ソース追加時は `pwsh -File tools\gen_project_files.ps1`。

### マイルストーンの終わりに

- `tools\replay_verify.bat` — sim は触らないので無風のはず。**割れたら設計を間違えている**
  (描画専用のつもりが sim に漏れた)。M54b (`LightComponent`) だけは必ず回す。
- `tools\crash_verify.bat` / `tools\net_verify.bat` — 描画変更では原則不要。
  ただし M55c (GBuffer 5 枚化) と M57a (3D テクスチャ) は VRAM を増やすので Debug で 1 回。
- **CI (`.github\workflows\ci.yml`) を緑にする。** WARP の壁時計が伸びていないか
  ステップのログで確認する — M54a で計測した基準値と比較する。

### 手動確認 (自動化不能)

- `--render-demo` を Editor で開いて、影 / デカール / 反射 / 光芒 / 地形を目視。
- `--render-demo --deferred` と Forward の絵が一致すること。
- AssetBrowser のサムネイルと Inspector のマテリアルプレビューが壊れていないこと
  (別 RenderSystem 経路の回帰。M53 の作業と隣接する)。
- 各マイルストーンの Rendering メニューのトグルで A/B。

---

## 付録: 並列実装の統合契約 (Wave 構成と共有資源の予約表)

M54〜M58 を **worktree で同時に走らせる**ために、後から取り合いになる資源をここで先に予約する。
**各ブランチはこの表の外にある番号・名前・レジスタを勝手に取らないこと。** 取った瞬間に
統合で無言の上書きが起きる (TypeId とレジスタ番号は、食い違っても *コンパイルは通る*)。

### Wave 構成

| Wave | 中身 | 並列度 | 出る先 |
|---|---|---|---|
| **Wave 0** | `engine_spec §6.5` (切り落とし 3 件) / **M54a** / **M55a** / 本契約 | 1 (master 直) | master |
| **Wave 1** | **M54b-e** / **M55b-f** / **M58a-f** | 3 (worktree) | 統合 1 |
| **Wave 2** | **M56a-f** / **M57a-e** | 2 (worktree、統合 1 の master から切る) | 統合 2 |

依存の根拠 (これ以外の順序制約は無い):
- **M57b は M54d の `SampleShadowAtlas` と M54b の `LightSelection` を直接呼ぶ** → M57 は M54 の後。
- **M56d/M56f は M55b のジッタ非適用 VP を前提にする** → M56 は M55 の後。
- M58 は他のどれにも依存しない (Wave 1 の 3 本目に置いたのは並列度の都合)。
- M54a (ショーケースシーン) は **全ての golden の土台**なので Wave 0。
  M55a (`LinearizeDepth` 共有化) は M56c/M56d/M57 が読むので同じく Wave 0。

### 予約 1: ComponentTypeId (`RegisterBuiltinComponents()` 末尾 append)

現在の最大は 32 (`PlayerInput`)。**登録順 = TypeId** なので、統合の順序がそのまま番号になる。

| TypeId | コンポーネント | サブ | 統合 |
|---|---|---|---|
| 33 | `TerrainComponent` | M58b | 統合 1 |
| 34 | `DecalComponent` | M56a | 統合 2 |
| 35 | `ReflectionProbeComponent` | M56f | 統合 2 |

**3 つとも `kComponentNoHash`** (描画専用) なので `.rep` 互換の作業はゼロ。シーン JSON は
コンポーネント**名**で引くので、統合時に並び替えても保存済みシーンは壊れない。
ブランチ内で単独ビルドすると番号は前倒しになる — **数値をコード/コメントに焼かないこと**。

### 予約 2: シェーダレジスタ

実測 (Wave 0 時点): Deferred 光パス = `t0..t11` / `s0..s1`、Forward = `t0..t5` / `s0..s2`。

| レジスタ | 用途 | サブ |
|---|---|---|
| Deferred 光パス `t12` | シャドウアトラス (`Texture2D`) | M54c |
| Deferred 光パス `t13` | SSR 結果 | M56d — **結局取らなかった (空き席)**。理由は進捗表の M56d 行 |
| Deferred 光パス `t14` | ローカル反射プローブ (`TextureCubeArray`) | M56f — 予約どおり取得。**SSR パス側では `t8`** (同じ実体) |
| Deferred 光パス `t15` | froxel 積分結果 (`Texture3D`) | M57e |
| Deferred 光パス `t13` | SSR 結果 | M56d |
| Deferred 光パス `t14` | ローカル反射プローブ (`TextureCubeArray`) | M56f |
| Deferred 光パス `t15` | froxel 積分結果 (`Texture3D`) | **M57d** (M57e から前倒し — 絵が初めて変わるサブが不透明の合成を持つ。`gbSrvs`/`nullSrvs` は **`[16]`** で、`[13]`/`[14]` は M56 の空席として **nullptr のまま空けてある**) |
| Forward `t6` | シャドウアトラス | M54c |
| Forward `t7` | froxel 積分結果 | **M57e で確定**。読むのは forward_lit / `_instanced` / `_skinned` / **`_terrain`** の 4 本 + **スカイボックス 2 本** (`skybox` / `skybox_cubemap`)。★スカイに専用スロットを与えないこと — スカイは不透明と透明の**間**のパスなので、別スロットを取ると Forward で後段の半透明が読む t1 (CSM) を潰す。ホストが張った t7 / s2 に相乗りする。パーティクルだけは別シェーダなので **t3** (`froxel::kParticleSrvSlot`)。`AppliesFroxel()` は Forward / Deferred とも true |
| GBuffer MRT `RT4` | velocity (`R16G16_FLOAT`) | M55c |
| `GpuLight::pad0` | アトラススロット index | M54c |
| `GpuLight::pad1` | アトラス面数 (点光源=6 / スポット=1 / 未割当=0) | M54c/d |
| 地形パス `b4` | 地形の CB (`kTerrainObjectCbSlot`) | M58c |
| 地形パス `t20` / `t21-t24` / `t25-t28` | スプラット / レイヤ albedo x4 / レイヤ normal x4 | M58d |

- 地形パス (`TerrainPass`) は**ホストのスロットを 1 つも奪わない**独立パス。上の 2 行が
  「誰とも隣り合わない位置」= b4 と t20 以降にあるのはそのため。描画後に t20-t28 は
  null で剥がす。**サンプラは s0 (異方性 WRAP) と s2 (LINEAR CLAMP) の流用**で新設ゼロ。

- **サンプラは 1 つも増やさない。** アトラスは `s1` の比較サンプラ、SSR/froxel/プローブは
  `s0` の LINEAR/CLAMP を流用する。`DeferredPath.cpp:728-732` が既に警告しているとおり
  SSAO パスが `s1` を point-wrap で上書きするので、光パスで**必ず張り直す**。
- `nullSrvs[12]` (`DeferredPath.cpp:750`) は最終的に `[16]` になる。**自分が足した分だけ広げる**
  (M54 は `[13]`、M56 は `[15]`、M57 は `[16]`)。
  **M56 の実績: `[15]`** — M56d が t13 を取らなかったので t13 は空き席のまま (常に nullptr を
  張る)、M56f が t14 を取った。M57 は **t15 を足して `[16]`** にすればよい。
- Deferred の透明後段は `PSSetShaderResources(1, 5, fwdSrvs)` (`DeferredPath.cpp:769`) で
  Forward の `t1..t5` を張り直している。Forward に `t6`/`t7` を足したらこの本数も増やす。

### 予約 3: golden スロット (`tools\shot_verify.bat` の呼び順)

| # | 名前 | tol | CI | サブ |
|---|---|---|---|---|
| 1-5 | `demo_forward` / `demo_deferred` / `parts` / `flow_title` / `ui_probe` | 3 | ○ | 既存 |
| 6 | `demo_render_forward` | 3 | ○ | M54a |
| 7 | `demo_render_deferred` | 3 | ○ | M54a |
| 8 | `demo_terrain_deferred` | 3 | ○ | M58c |
| 9 | `demo_render_ssr` | 3 | ○ | M56d |
| 10 | `demo_forward_fxaa` | 0 | × (`MYE_SHOT_SKIP_FXAA`) | 既存 |
| 11 | `demo_render_taa` | 0 | × (`MYE_SHOT_SKIP_TAA`) | M55d |
| 12 | `demo_render_froxel` | 0 | × (`MYE_SHOT_SKIP_FROXEL`) | **M57d** (M57c から移送。積分結果を読む者が居ない M57c で撮ると `demo_render_deferred` と tol=0 でビット一致する 2 枚目にしかならず、動いたときの切り分けができなくなる。実測済み) **→ M57d で撮影済み / M57e で撮り直し** (背景ピクセルにも合成が乗ったため。旧 golden に対し maxDiff=61 / 139,678 画素)。tol=0 の根拠 = 同一引数 2 回 / Debug↔Release とも maxDiff=0 (M57e 後に取り直し済み)。CI へ載せない根拠 = TAA と同型の蓄積系で増幅率未測 + WARP 撮影が 12s 増える |

- **★`tests\golden\*.png` はバイナリ = マージ不能。** 各ブランチは**自分が新設した golden だけ**を
  コミットし、**既存の golden には 1 バイトも触らない**。触った瞬間に
  「機能 off で直前コミットとビット一致」というこのロードマップ唯一の受入基準が消える。
- **M56a が 7 番 (`demo_render_deferred`) と 11 番 (`demo_render_taa`) を撮り直した** —
  デカール 2 枚を `--render-demo` に置いたため (置かないと回帰被覆がゼロ。数値は進捗表の
  M56a 行)。**この 2 枚はセットで動く** = `demo_render_deferred` を動かす変更は必ず
  `demo_render_taa` も動かす。統合でこの 2 枚が競合したら **M56 側 (後から撮ったもの) を採る**。
- **M56b が同じ 2 枚をもう一度撮り直した** — `DecalGround` に法線マップ + roughness 上書きを
  付けたため (数値は進捗表の M56b 行)。**M56 ブランチが持つ 7 番 / 11 番が最新**。
- 統合後に `shot_verify.bat --update` を**回さない**。回すと全部を「現状が正解」で塗り潰す。
- CI 判定枚数が 5 → 9 に増える = WARP 撮影の壁時計が約 1.8 倍。**M54a で 1 枚あたりを実測し、
  9 枚で CI 予算に収まるかを判断する** (収まらなければ 8/9 をローカル限定へ降格)。

### 予約 4: `CameraPostFxComponent` の末尾 append 順

全て `kComponentNoHash` なので順序は互換に影響しないが、統合の競合を機械的に解くために固定する。
**既定値は必ず「恒等 = 従来の見た目」**。

1. M55d: `int32_t taaOn = 0` / `float taaFeedback = 0.9f`
2. M56d: `int32_t ssrOn = 0` / `float ssrMaxRoughness = 0.6f` / `float ssrIntensity = 1.0f`
3. M57c: `int32_t froxelOn = 0` / `float froxelDensity = 0.02f` / `float froxelAnisotropy = 0.3f`

### 予約 5: `LocalizationTable.inl` のキー接頭辞

| M54 | M55 | M56 | M57 | M58 |
|---|---|---|---|---|
| `Shadow_*` | `Taa_*` | `Decal_*` / `Ssr_*` / `Probe_*` | `Froxel_*` | `Terrain_*` |

`###` の右辺 (ImGui ID) も同じ接頭辞を使う (両言語一致・テーブル内で一意)。

### 予約 6: 新規ファイルの置き場所

| サブ | ファイル |
|---|---|
| M54b | `src\Engine\Engine\LightSelection.{h,cpp}` / `src\Editor\LightSelectionSelfTest.{h,cpp}` |
| M54c | `src\Engine\Renderer\ShadowAtlas.{h,cpp}` |
| M55d | `src\Engine\Renderer\TaaPass.{h,cpp}` / `assets\shaders\postfx_taa.hlsl` |
| M56a | `assets\shaders\decal_project.hlsl` / `src\Editor\DecalSelfTest.{h,cpp}` (**計画外の追加**。逆行列と投影方向の取り違え / 描画順の収集順依存 / `kComponentNoHash` の書き忘れは、どれも「絵は出るのに間違っている」形でしか現れないので機械検査が要る) |
| M56c | `src\Engine\Renderer\HzbPass.{h,cpp}` / `assets\shaders\hzb_reduce.cs.hlsl` / `assets\shaders\debug_hzb.hlsl` + `src\Editor\HzbSelfTest.{h,cpp}` (**計画外の追加 2 件**。消費者 (SSR) が M56d まで居ないので、可視化シェーダが唯一の目視口になる — `debug_velocity` と同じ立ち位置。段の数え方と縮小の被覆区間は「絵を見ても壊れているか分からない」ので機械検査が要る) |
| M56d | `src\Engine\Renderer\SsrPass.{h,cpp}` / `assets\shaders\ssr_trace.hlsl` + `src\Editor\SsrSelfTest.{h,cpp}` (**計画外の追加 2 件**。SsrPass は HzbPass と同じ立ち位置 — DeferredPath をこれ以上太らせない。SsrSelfTest は「セル前進が 0 になる」「フェードが厳密 0 でない」のどちらも絵から追えないため) |
| M56f | **新規ファイルなし。** 型 (`ReflectionProbeGpu` / `ReflectionProbeSet` + CPU 鏡 3 本) は `RenderTypes.h` へ、ベイク束と `BakeAll` は `ProbeBaker.{h,cpp}` へ相乗りした (デカールが `DecalRenderItem` + 純関数を `RenderTypes.h` に置いたのと同じ流儀)。自己テストも新設せず `ProbeBakerSelfTest` に 20 項目を足してある = **スイート数は 35 のまま** |
| M56e | `src\Engine\Engine\ProbeBaker.{h,cpp}` + `src\Editor\ProbeBakerSelfTest.{h,cpp}` (**計画外の追加**。「撮影した面の向き」と「プリフィルタがサンプルする向き」がずれても絵は普通に出てしまう — 焼いた反射が面ごとに 90 度回るだけなので、機械検査でしか捕まらない) |
| M57a | `src\Engine\Renderer\VolumeTexture.{h,cpp}` / `assets\shaders\froxel_*.cs.hlsl` |
| M57b | `src\Engine\Renderer\FroxelPass.{h,cpp}` |
| M57c | `assets\shaders\froxel_common.hlsli` (**計画外の追加**。スライス深度 / 逆射影 / 解析積分を clear・inject・temporal・integrate の 4 本が共有するため。`MYE_FROXEL_GROUP` の正本もここ。`common.hlsli` へは置けない — あちらは「register 宣言を持たない」契約で、共有したいのは式だけ) |
| M58a | `src\Engine\Engine\Asset\TerrainAsset.{h,cpp}` |
| M58b | `src\Engine\Engine\TerrainSystem.{h,cpp}` / `src\Editor\TerrainSelfTest.{h,cpp}` |
| M58c | `src\Engine\Renderer\TerrainPass.{h,cpp}` / `assets\shaders\{deferred,forward}_terrain.hlsl` |
| M58d | `assets\shaders\terrain_common.hlsli` (**計画外の追加**。deferred / forward の 2 本が同じ地表を出すための共有本体。`common.hlsli` は「register 宣言を持たない」契約なので置けない) |
| M58f | `src\Engine\Engine\Asset\TerrainEdit.{h,cpp}` (**計画外の追加**。ブラシ / 矩形パッチ / 編集サイドカー `.terrain.edit`。地形の**オーサリング**は描画側の `TerrainSystem` にもクック側の `TerrainAsset` にも属さない) |

- 新シェーダには **`.meta` が要る** (`{"guid":<64bit>,"type":"shader","version":1}`)。
- ソース追加のたび `pwsh -File tools\gen_project_files.ps1`。
- **`.vcxproj` の `<!-- BEGIN FILES -->` 区間はマージしない** — 統合後に生成器を 1 回回して
  作り直す (競合は ours/theirs どちらで潰してもよい)。

### 予約 7: selftest 連鎖 (`EditorMain.cpp` の `&&` 連鎖) の追加順

末尾 append。統合順に並ぶ: `LightSelectionSelfTest` (M54b) → `TerrainSelfTest` (M58b)
→ `DecalSelfTest` (M56a) → `HzbSelfTest` (M56c) → `SsrSelfTest` (M56d)
→ `ProbeBakerSelfTest` (M56e)。29 → **35 スイート**
(banner を grep すると 32 本しか出ないが、`RenderSelfTest` だけ見出しの文言が違うだけ)。
**連鎖は短絡なので、最初に落ちた 1 本で以降は走らない** — 追加位置は必ず末尾。

### 予約 8: `check_rules.ps1` の `$constGroups`

**Wave 0 で既存の未登録ペア 2 件を回収する** (規則 9 の穴):
`kMaxLights=16` ↔ `MAX_LIGHTS` ↔ `MYE_RT_MAX_LIGHTS` / `ShadowPass::kCascades=3` ↔ `vps[3]`。
各ブランチは自分の共有定数を**末尾に追記**する。
M56c が追記済み: `kHzbThreadGroupSize` (`HzbPass.h`) ↔ `MYE_HZB_TG` (`hzb_reduce.cs.hlsl`)。
M56d が追記済み: `kSsrMaxSteps` (`SsrPass.h`) ↔ `MYE_SSR_MAX_STEPS` (`ssr_trace.hlsl`)。
M56f が追記済み: `kMaxReflectionProbes` (`RenderTypes.h`) ↔ `MYE_MAX_REFLECTION_PROBES`
(`common.hlsli`) — **光パスと SSR の 2 本が同じ CB 配列を持つ**ので食い違うとレイアウトごとずれる。

### 統合時のチェックリスト (統合 1 / 統合 2 で毎回)

1. マージ順は上の表の TypeId 順 (統合 1 = M54 → M55 → M58、統合 2 = M56 → M57)。
2. `Components.cpp` / `Components.h` / `LocalizationTable.inl` / `shot_verify.bat` /
   `check_rules.ps1` / `common.hlsli` の競合は **全部「両方採用・予約表の順に並べる」** で解く。
3. `pwsh -File tools\gen_project_files.ps1` を回して `.vcxproj` を作り直す。
4. `nullSrvs[]` の本数と `PSSetShaderResources` の件数が予約表と合っているか目視。
5. Debug + Release ビルド (警告 0) → `--selftest` → `check_rules.ps1` → `shot_verify.bat`
   → `replay_verify.bat`。**`shot_verify` の既存 golden が全部ビット一致すること**が
   「並列で入れた機能がどれも既定の絵を動かしていない」証明になる。
6. 動いた golden があったら **`--update` で潰さず、なぜ動いたかを先に説明する**。
