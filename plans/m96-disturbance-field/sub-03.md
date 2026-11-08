# sub-03: 地面の霧の専用描画 (2D 層を板のレイマーチで描く、自己照明 + 太陽光の bool)

- 依存: sub-02
- 状態: OK (commit は司会が記入)
- 往復: 2

## やること

spec §2 #13 (ユーザー回答 2026-11-08: 地面の霧専用の描画を足す)、§4.3 の「地面の霧」。sub-02 のフロクセル注入では、浅い角度で見たときに
深度スライスの横縞と縦の粗さが出た (sub-02 のスクショ 02 / 03)。**GroundLayer の霧はフロクセルから外し**、各ピクセルで層の板を直接
レイマーチして描く。フロクセルは Volume (3D、sub-07) 専用になる。

1. 共有の HLSL 関数 `GroundFogComposite` (sub-02 の `disturbance_common.hlsli` に置く):
   - 入力 = カメラ位置・面のワールド位置 (posW)・色。カメラ → posW の線分と、各層 (最大 4 枚) の「xz の格子の矩形 × [基準高さ, 基準高さ + thickness] の板」の交差区間を解析的に求める。基準高さは baseY + G チャネルのずれの最小 / 最大で板を包み、区間の中で正確な `p(y)` を評価する (sub-10 で起伏が入っても区間が外れないため)
   - 区間の中を**固定の段数** (既定 24。定数で 1 か所。C++ 側と規則 9 で一致) で積分する。段の始まりのずらしはピクセル位置からの固定の雑音 (interleaved gradient noise 等。フレーム番号を使わない = 同じ入力から同じ絵) とし、TAA があるときはそのまま TAA に均させてよい
   - 段ごとに `σ = fogDensity × ρ(posW) × p(y) × edgeFade` (sub-02 と同じ式・同じテクスチャ)、透過率 `T *= exp(−σ·Δs)` (描画レーンなので exp を使ってよい)、散乱 = σ × albedo × (環境光 + `fogColor × fogBrightness` + `fogSunLight` の層だけ平行光 × HG(g)、影なし)
   - 出力 `color × T + inscatter`。層が 0 枚、または線分がどの板とも交わらないピクセルは**何もしない** (分岐で抜ける = 絵がビット不変)
   - 局所ライト (点・スポット) はこのサブでは照らさない。ON/OFF の切り替え (`fogLocalLights`) は sub-15 で足す。ライトの寄与を後から 1 項足せる形 (段ごとの散乱を関数に分ける) にしておく
2. 合成する場所: フロクセルの合成と同じ 10 本 (`grep -rn gFroxelEnabled assets\shaders`。不透明 / 透明 / 地形 / スカイ / パーティクル / VFX / 水面 / サーフェスシェーダ)。**順序は「地面の霧 → 既存の解析フォグとフロクセル」** (地面の霧は面のすぐ手前にあるので先に掛け、その外側の大気を後に掛ける)。スカイは posW = 遠方として扱う
   - 束縛: sub-02 の 2D 配列・サンプラ・CB を、各シェーダの空いたスロットに。番号は共有ヘッダの PS 用の定義で 1 か所 (sub-02 の申し送り: 段ごとに `MYE_DFLD_*` を define してから include)
   - 束縛の口は Forward / Deferred の両パス。CB の詰め替えは sub-02 の `DisturbanceFieldGpu` を使い回す
3. フロクセルから GroundLayer を外す: `froxel_inject.cs.hlsl` の `DisturbanceFogAt` を Volume だけの関数にする (sub-07 まで Volume は 0 個なので、このサブの後はフロクセルへの場の寄与は 0 = フロクセルの絵は sub-02 以前と同じ)。RenderSystem の転送はフロクセルの on / off に依らず、GroundLayer の霧がある (濃さチャネルあり、`fogDensity > 0`) ときに行う
4. `DisturbanceFieldComponent` の Inspector の説明: GroundLayer の霧はフロクセル不要、局所ライトは sub-15 まで効かない、太陽光は影なし (`Tr()` 両言語)。`fogSunLight` / `autoFroxel` の説明のうち Volume 向けの部分は sub-08
5. デモ: `--field-demo` のカメラの `froxelOn` を外す (地面の霧はフロクセル無しで写る)。太陽 (平行光) を 1 つ足し、層の `fogSunLight` を true にする
6. 計測: 960×540 と 1920×1080 で、層 1 枚 / 4 枚のときの地面の霧の GPU 時間 (Deferred の光パスと Forward の合計の増分。GpuTimer か `--render-stats-dump`)。合否の線は置かず報告する (planner が段数の既定を判断)

## やらないこと (このサブでは)

- 局所ライトで地面の霧を照らす (sub-15)、太陽光の影
- Volume の描画 (sub-07)、フロクセルの自動有効 (sub-08)
- 霧の時間方向の蓄積 (テンポラル) — 段数とピクセル雑音で足りなければ planner が判断する

## 触る場所 (planner の見立て)

- `assets\shaders\disturbance_common.hlsli`、合成箇所 10 本 (`deferred_light.hlsl:249-275`、`forward_lit.hlsl:173-`、`forward_lit_instanced.hlsl`、`forward_skinned.hlsl`、`forward_terrain.hlsl`、`MyEngineSurface.hlsli:151-170`、`particle_render.hlsl`、`vfx_sprite.hlsl`、`water_surface.hlsl`、スカイ)、`froxel_inject.cs.hlsl`
- `src\Engine\Renderer\Passes\DisturbanceFieldPass.*`、`DeferredPath.cpp` / `ForwardPath.cpp` / 水・粒子・VFX のパスの束縛、`src\Engine\Engine\Rendering\RenderSystem.cpp` の `UpdateDisturbanceField` (今は froxelOn のときだけ転送 — 外す)
- 環境光・太陽の値は各シェーダが既に持つもの (`gSunDirection` / `gSunColor` など) を使い、新しい CB を増やすのは最小限に
- `tools\check_rules.ps1` 規則 9 (段数と PS 用の束縛番号)
- `src\Engine\Engine\Demo\DemoContentField.cpp`

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 32 (2D 層の分), 34。

- 34:
  - (a) 横縞が消えた: sub-02 と同じカメラ・同じフレーム (100 / 200) で撮り、霧の領域の行ごとの平均輝度の 2 階差分の最大値を sub-02 の 02 / 03 と並べて報告し、sub-02 より小さいこと。画像も並べる
  - (b) フロクセル off (カメラ・グローバルとも) で霧が写り、フロクセルのパスが走らない
  - (c) 場の無い既存デモ (`--fog-demo` と、霧を使わないデモ 1 本) の絵が、着手前 (sub-02 のコミット `2687227`) のビルドで撮った絵と `--img-diff` で差 0。sub-02 で残った「旧 C++ との比較」の nit もこれで閉じる
  - (d) 霧の後ろの透明物 (粒子か VFX) と、霧の中の不透明物の両方が同じように霞む (スクショ)
  - (e) 計測 (上の 6) の数値
- 32 (2D 層の分): `fogSunLight = false` と true の 2 枚、太陽の方向と逆の方向の平均輝度
- 12: replay_verify `field` (描画だけの変更なので sim は不変。運用によりまとめて実行)
- 撮った画像のパスを SELF_EVAL に書く (最後の目視はユーザー)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-16 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- スクショ一式 (Runtime.exe、Forward と Deferred)、`Editor.exe --img-diff`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1

SELF_EVAL: sub-03 (round 1)

実装:
- assets\shaders\ground_fog.hlsli (新規) — 板のレイマーチ。層ごとに「xz の格子矩形 × [baseY + ずれ最小, baseY + ずれ最大 + thickness]」とカメラ→面の線分を解析的に交差し、24 段 (MYE_GFOG_STEPS) で積分。段の始まりは IGN(SV_Position) で、フレーム番号は使わない。層は t0 の大きい順 (奥から) に重ねる。段ごとの散乱は GfogStepRadiance に分けた (局所ライトは sub-15 でここへ 1 項足す)。透過率 exp(-σ·Δs)、1 段の in-scatter は厳密積分 (1 - exp(-σΔs))。層 0 枚 / どの板とも交わらないピクセルは color をそのまま返す
- PS の束縛は全シェーダ共通の固定番号 (b6 = 層 CB、b7 = ビュー CB、t32 / t33、s8)。RenderSystem::UpdateDisturbanceField が 1 描画に 1 回張り、DrawParticlesAndDebug の後に剥がす (各シェーダは include と呼び出し 1 行だけ)
- 合成 13 本 (順序は「地面の霧 → 解析フォグ / フロクセル」): deferred_light, forward_lit, forward_lit_instanced, forward_skinned, forward_terrain, MyEngineSurface.hlsli (MyeApplyFog)、particle_render (加算は透過率のみ)、particle_render_gpu (同)、vfx_sprite、water_surface、skybox / skybox_cubemap / skybox_panoramic (視線方向 2000 m 先を面として扱う)
- assets\shaders\disturbance_common.hlsli — DisturbanceFogAt を層 1 枚の消散係数 DfldLayerSigma(i, posW) に置き換え。層 CB の fogColor.w = 太陽光 bool、texScale.zw = 基準高さのずれの最小 / 最大 (CB の大きさは 336 のまま)
- assets\shaders\froxel_inject.cs.hlsl — フロクセルから 2D 層を外した。DisturbanceVolumeFogAt (Volume 専用、sub-07 まで 0 を返す)
- src\Engine\Renderer\Passes\DisturbanceFieldPass.h/.cpp — kGroundFog* 定数、GroundFogViewCB (64 B)、GroundFogViewParams、BindGroundFog / UnbindGroundFog。Upload が fogSunLight と基準高さのずれの範囲 (転送し直すときだけ数える) を CB に載せる
- src\Engine\Engine\Rendering\RenderSystem.cpp/.h — UpdateDisturbanceField をフロクセルの on/off に依らず「霧のある層 (hasDensity && fogDensity > 0) があれば転送 + 束縛」へ。環境光 / 太陽は f.lights から詰める
- src\Engine\Engine\Disturbance\DisturbanceRender.cpp — fogSunLight を転送指示へ
- src\Engine\Core\Ecs\Components.cpp — fogSunLight / autoFroxel のツールチップ (地面の霧はフロクセル不要、局所ライトは未対応、autoFroxel は Volume 向け)
- src\Engine\Engine\Demo\DemoContentField.cpp — カメラの CameraPostFx (froxelOn) を外し、層の fogSunLight = true (太陽は既存の平行光 "Sun")
- tools\check_rules.ps1 規則 9 — kGroundFog* と MYE_GFOG_* の 6 組を追加
- src\Engine\Engine\Disturbance\DisturbanceFieldSelfTest.cpp — fogSunLight が転送指示へ届く検査を追加 (コードのみ。未実行)

仕様との差分:
- [追加] 各シェーダに 1 行ずつ呼ぶ方式ではなく「PS の固定スロットへ RenderSystem が 1 回張る」方式にした (sub の「束縛の口は Forward / Deferred の両パス」の代わり) — 13 本 x 多数の束縛箇所の取りこぼしを避けるため。残るリスク: 同じ DeviceContext で別 RenderSystem (サムネイル等) が描くとき。UpdateDisturbanceField は場の無い World では何も張らず、張った描画は最後に必ず剥がす
- [追加] 板の層の順序を t0 でソートして奥から重ねる (複数層が奥行きで並ぶとき)
- [追加] 水面 (water_surface) は従来フロクセルを合成していないが、sub の「合成する場所」に水面が入っているので地面の霧だけ掛けた (フロクセルは掛けない)
- [未実装] Deferred の「ジオメトリ無し = 背景」(deferred_light の albedo.a < 0.5) には地面の霧を掛けていない (視線方向の復元が要る。スカイボックスがある絵は skybox 側で掛かる。スカイ無しの Deferred 背景は霧が掛からない)
- [未実装] fogLocalLights は sub-15 (指示どおり)
- [逸脱] sub の「ビット不変」確認は --fog-demo を含む 4 デモ (--fog-demo / --render-demo / --particle-demo / --terrain-demo) を Forward / Deferred の 2 経路で撮り、着手前ビルド (sub-02 のバイナリ + 元のシェーダ) と比べた
- [追加] 受け入れ 34(d) 用の透明物・加算物を足す一時コードをデモに入れて撮影した後で消した (コミットに残していない)。計測 (6) も同じく一時の環境変数で層数を切り替えた

検証:
- Release / Debug ビルド (MSBuild、-m) → どちらも error / C 警告 0
- tools\gen_project_files.ps1 → build/ の差分なし (シェーダは vcxproj に載らない)
- fxc (ps_5_0) で deferred_light / forward_lit / forward_lit_instanced / forward_skinned / forward_terrain / particle_render / particle_render_gpu / vfx_sprite / water_surface / skybox x3 と、ToonFlat.surface (MyePSColor / MyePSVelocity)、froxel_inject (cs_5_0) をコンパイル → 全部 rc=0
- tools\check_rules.ps1 → 0 error (59 warning は既存)
- 34(a) 横縞: 霧の領域 (行 180..455、列 30..110) の行平均輝度の 2 階差分の最大値 (同じカメラ・同じフレーム)。Forward: sub-02 = t100 0.787 / t200 1.430 → sub-03 = t100 0.223 / t200 0.223。Deferred: sub-02 = 0.787 / 0.856 → sub-03 = 0.450 / 0.450。いずれも sub-02 より小さい。画像は下
- 34(b) フロクセル off: デモのカメラから CameraPostFx を外し、グローバルも --froxel なし。霧は写る (下の画像)。ログに froxel の行は 0 件 (FroxelPass は enableFroxel / froxelOn が立つまで動かない)。※フロクセルのパスが走らないことは専用の計測値では確かめていない (コードとログからの確認)
- 34(c) ビット不変: --fog-demo / --render-demo / --particle-demo / --terrain-demo の frame 60 を Forward と Deferred で撮り、着手前 (sub-02 のバイナリ + 元のシェーダ) の絵と Editor.exe --img-diff --tol 0 → 8 組すべて maxDiff=0 diffPixels=0。sub-02 の「旧 C++ との比較」の nit の閉じ方としては、着手前のシェーダ (2687227) との一致で代えた
- 34(d) 霧の後ろの透明物 / 中の不透明物: 一時のコードで加算の火花 (CPU 粒子) と alpha の煙を、霧の中 (z = 5) と霧の奥 (z = 28) に置いて撮影 → 奥の粒子は霧越しに薄く、中の粒子は霧に沈んだ分だけ下が見えない。霧の中の球 (不透明) も同様に白く霞む。Forward / Deferred の両方。GPU 粒子バックエンド (particle_render_gpu) は fxc コンパイルのみで、絵は未確認
- 34(e) 計測: RTX 3060 (ハード)、--render-stats-dump の gpuMs.frame (frame 100、5 回)。GPU のクロックが揺れて中央値が割れるので、最小値と中央値を両方書く (ms)
  - Forward 960x540: 層 0 = 最小 0.588 / 中央 0.591、層 1 = 0.707 / 0.763、層 4 = 1.148 / 1.174
  - Forward 1920x1080: 層 0 = 0.564 / 1.889 (割れている)、層 1 = 1.182 / 1.185、層 4 = 2.739 / 2.752
  - Deferred 960x540: 層 0 = 0.371 / 0.372、層 1 = 0.536 / 0.577、層 4 = 2.075 / 2.114
  - Deferred 1920x1080: 層 0 = 1.002 / 1.030、層 1 = 1.572 / 1.924、層 4 = 3.210 / 3.216
  - 増分 (最小値どうし): 層 1 枚で +0.1〜0.6 ms、4 枚で +0.5〜2.2 ms。4 層は全部がカメラ線分と交わる最悪形 (同位置に 4 枚重ね)。光パスだけの GpuTimer は取っていない (frame 全体の差分)
- 32 (2D 層の分) fogSunLight false / true、霧の領域の平均輝度 (0..255): カメラが太陽の進行方向を向く (太陽が背後) = off 224.625 / on 225.601。カメラが太陽のほうを向く (太陽の逆向き) = off 224.804 / on 226.748。on が常に明るい。差が小さいのは霧が濃く (σ = 1.6) ほぼ飽和しているため
- 12 replay_verify `field` / selftest → 未実行 (指示どおり、sub-07 後 / sub-16 でまとめて実行)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 34(a)(b)(c)(d)(e)・32 は画像と数値で確認。背景 (ジオメトリ無し) の Deferred と GPU 粒子の絵が未確認・未実装
  正しさ: 4 — 8 組のビット不変と横縞の解消を実測。複数層の奥行き順序と起伏 (baseOffset の範囲) の経路は、まだデータが無い (起伏は sub-10) ため実画面では未確認
  コード品質: 4 — 束縛を 1 か所に集約し、各シェーダは 1 行。DisturbanceFieldPass に「CB を毎フレーム組み直す」前提が残る
  テスト: 2 — selftest は 1 検査を足したが未実行。replay_verify も未実行 (運用)。GPU の絵はスクショと img-diff のみ

不安・質問:
- 地面の霧の固定スロット (b6 / b7 / t32 / t33 / s8) を RenderSystem が張る方式にした。サムネイル (AssetPreviewCache の別 RenderSystem) や SceneView が同じ DeviceContext で同時期に描いても、各描画の末尾で剥がすので漏れないはずだが、Editor の実画面では未確認 (スクショ検証は Runtime.exe のみ)
- 計測の frame 全体の差分は 4 層 / 1080p Deferred で +2.2 ms。段数 24 の既定が重いかの判断は planner (段を 16 に減らす / 層の交差区間が短いと自動で段が減る形にする、などの案)。24 段のまま TAA に均させる前提だが、sub-03 のスクショは --no-fxaa で TAA は効いていない (ノイズは出ていない)

触ったファイル:
- assets\shaders\ground_fog.hlsli, assets\shaders\ground_fog.hlsli.meta (新規)
- assets\shaders\disturbance_common.hlsli, froxel_inject.cs.hlsl, deferred_light.hlsl, forward_lit.hlsl, forward_lit_instanced.hlsl, forward_skinned.hlsl, forward_terrain.hlsl, MyEngineSurface.hlsli, particle_render.hlsl, particle_render_gpu.hlsl, vfx_sprite.hlsl, water_surface.hlsl, skybox.hlsl, skybox_cubemap.hlsl, skybox_panoramic.hlsl
- src\Engine\Renderer\Passes\DisturbanceFieldPass.h, DisturbanceFieldPass.cpp
- src\Engine\Engine\Rendering\RenderSystem.cpp, RenderSystem.h
- src\Engine\Engine\Disturbance\DisturbanceRender.cpp, DisturbanceFieldSelfTest.cpp
- src\Engine\Core\Ecs\Components.cpp
- src\Engine\Engine\Demo\DemoContentField.cpp
- tools\check_rules.ps1
- plans\m96-disturbance-field\ 配下 (spec / sub-*.md / harness.md / 新規 sub-15, sub-16 = planner と司会の未コミット変更、本サブのコミットへ同梱)

申し送り:
- スクショ (全部 C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\14d68bbd-02de-486d-b73d-7903e06a5ea3\scratchpad\m96-shots\ 配下):
  - sub03_01_forward_t100.png / sub03_02_forward_t200.png (Forward、sub-03)
  - sub03_03_deferred_t200.png (Deferred、sub-03)
  - sub03_04_before_sub02_forward_t200.png / sub03_05_before_sub02_forward_t100.png (sub-02 のコミットでの同じ frame・同じカメラ = 横縞あり。比較元)
  - sub03_06_particles_forward_t200.png / sub03_07_particles_deferred_t200.png (霧の中 / 奥の粒子)
  - sub03_08_sun_off_facing_along_t100.png / sub03_09_sun_on_facing_along_t100.png / sub03_10_sun_off_facing_sun_t100.png / sub03_11_sun_on_facing_sun_t100.png (太陽光の bool)
- sub-15 へ: GfogStepRadiance (ground_fog.hlsli) に局所ライトの項を足す。ビュー CB (GroundFogViewCB, 64 B) にライトを足すなら CB の大きさと HLSL を同時に直す
- sub-07 / sub-08 へ: froxel_inject.cs.hlsl の DisturbanceVolumeFogAt (今は 0) に Volume を実装する。FroxelPass は今も層の CB / SRV を CS へ張っているが CS は読まない (Volume 用に残した)
- sub-10 へ: 層 CB の texScale.zw に「基準高さのずれの最小 / 最大」を Upload が計算して載せる実装は入っている (baseOffset が null の今は 0)。起伏が入ったら板の包みが効く
- Deferred の「ジオメトリ無し」背景への地面の霧は未対応 (上記)

### round 2

SELF_EVAL: sub-03 (round 2)

実装:
- #1: コードは変えていない (検証のみ。結論は下)
- #2: ground_fog.hlsli に GroundFogCompositeBackground (逆ビュー射影から視線を復元し、2000 m 先を面として扱う)。ビュー CB に gGfogInvViewProj を追加 (64 -> 128 B)。deferred_light.hlsl の背景分岐 (albedo.a < 0.5) で、フロクセルの前に掛ける。層 0 枚なら gClearColor とビット恒等。C++ は GroundFogViewParams / GroundFogViewCB に invViewProj、RenderSystem が inverse(view * proj) を詰める
- #3: Editor.exe で撮影 (下)。#4: 据え置き

#1 検証 (仮説は正しかった。絵は sub-03 の方が正しい):
- (a) 源の無い箱 (x = 6、球と同じ寸法 0.8 m、中心 y = 0.45) を霧の中に置いて撮影 -> 箱は上から下へ霧に沈む (下ほど霞む)。球は源が晴らした穴の中でくっきり
- (b) 球の densityDelta を 0 にして同じ frame -> 球 3 個とも霧に霞む (上は赤、下は白へ沈む)
- (c) 数値 (解析値。HLSL の値を出力して測ったものではない): 層の板 = [baseY, baseY + thickness] = [0, 1.0] m。球の中心 y = 0.45、半径 = 0.5 (メッシュ) x 0.8 (scale) = 0.4 -> 上面 y = 0.85。σ = fogDensity 1.6 x ρ x p(y)、p(y) = 1 - y。カメラ (0, 7, -12) から見下ろす角は約 23〜37 度 (sin 0.4〜0.6)。上面 (y = 0.85) の手前で線分が層を通る長さは約 0.25〜0.37 m、平均 p 約 0.075 -> σ 約 0.12 -> 透過 約 97% (ほぼ素通し)。正面の点 (y 約 0.45) まででは層内 約 1 m、平均 p 約 0.27 -> σ 約 0.44 -> 透過 約 65% (ρ = 1 のとき。(b) の絵と合う)。実際は源が ρ を下げる (densityDelta -0.3、半径 1.5 m) ので球の周りは更に晴れる
- 原因: sub-02 は粗いフロクセル (奥行きスライス / 縦のセルが太く、テンポラル 0.9) で穴がぼけ、球が沈んで見えていた。sub-03 は画素ごとに ρ を引くので穴がそのまま見える。GroundFogComposite の積分 (面で t1 を切る) に誤りは見つからない

#2 検証: カメラを y = 0.6・仰角 +4 度 (層の中、地平線を向く) にして Forward / Deferred を撮影。Deferred は背景に地面の霧が掛かる (層の上端で柔らかく消える帯)。Forward は背景がシェーダで描かれない (スカイボックスが無い = clear 色のまま) ので霧が掛からず、床の縁で硬い線になる -> スカイ無しの背景は Forward と Deferred で食い違う。既存のフロクセルの背景 (Deferred のみ) と同じ構造で、Forward には背景のピクセルを描くシェーダが無く掛ける手段が無い。スカイボックスがあるシーンは両方 skybox 側で掛かる (一致)

#3 検証: Editor.exe --field-demo --screenshot (1600x900。Profiler / Animation 窓は imgui.ini で画面外へ、Game を浮かせて Scene と同時表示。imgui.ini は撮影後に元へ戻した) -> Scene View は場のある所だけ霧、手前 (場の外) は霧なし。Game View も同じ霧。互いに漏れない。Asset Browser のサムネイルは CLI からフォルダを開けず撮れなかった (未確認)。理由付けのみ: サムネイルの RenderSystem は disturbanceWorld が null なので何も張らず、張った描画は最後に剥がす

検証:
- Release / Debug ビルド -> error / C 警告 0
- 着手前との img-diff (4 デモ x Forward / Deferred、--tol 0) -> 8 組すべて maxDiff=0 (背景分岐の変更後に撮り直し)
- check_rules -> 0 error
- 画像 (C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\14d68bbd-02de-486d-b73d-7903e06a5ea3\scratchpad\m96-shots\ 配下): sub03r2_01_box_without_source_and_balls_t100.png / sub03r2_02_box_and_balls_densityDelta0_t100.png / sub03r2_03_horizon_forward_t100.png / sub03r2_04_horizon_deferred_t100.png / sub03r2_05_editor_scene_and_game_view_t200.png
- selftest / replay_verify -> 未実行 (運用)

自己採点 (1-5):
  仕様適合: 4 — #1 は (a)(b) で実証、(c) は解析値。#2 は Deferred 実装済み・Forward の背景は構造上不可。#3 サムネイル未確認
  正しさ: 4 — ビット不変 8 組を再確認。霞みの数値は HLSL からの実測ではない
  コード品質: 4 — 背景関数 1 本とビュー CB の拡張のみ
  テスト: 2 — 絵と img-diff のみ。selftest 未実行

不安・質問:
- スカイ無しの背景が Forward と Deferred で食い違う (Forward に掛ける手段が無い)。許容か、Forward でもスカイ無しのとき背景を描く案 (大きな変更) かは planner 判断
- Asset Browser のサムネイルの実画面確認が出来ていない

触ったファイル (round 1 に加えて): assets\shaders\ground_fog.hlsli, assets\shaders\deferred_light.hlsl, src\Engine\Renderer\Passes\DisturbanceFieldPass.h / .cpp, src\Engine\Engine\Rendering\RenderSystem.cpp

申し送り: ビュー CB は 128 B になった (sub-15 でライトを足すなら大きさと HLSL を同時に直す)。デモは元の内容に戻した (一時コードはコミットに無い)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-08)。横縞は 34(a) の数値と司会の目視で消えた。34(c) の 8 組差 0、32、34(e) の計測を確認。must 1 件: 赤い球が霞まなく見える件 (司会の目視) と 34(d) の整合の証拠。仮説 = 球は自分の源 (半径 1.5 m・densityDelta −0.3) で周りの霧を晴らしており、sub-02 ではフロクセルの粗さ (奥行き 1〜2 m / スライス) とテンポラルがその穴をぼかしていた。should 2 件: Deferred の背景 (ジオメトリ無し) にも掛ける / Editor の実画面で束縛が漏れないことのスクショ
- round 2: VERDICT OK (planner、2026-11-08)。#1 は (a) 源の無い箱が霞む・(b) densityDelta 0 で球も霞む・(c) 解析値で、球が自分の源の穴の中にいるためと確定 (積分の誤りなし)。#2 Deferred の背景に掛けた (層 0 枚でビット恒等、8 組差 0 を撮り直し)。スカイ無しの背景の Forward / Deferred の食い違いは既存のフロクセルと同じ構造の制限として受け入れ (spec §7 R13)。#3 Scene / Game View の漏れなしを確認、サムネイルは sub-16 の手動確認へ
