# sub-03: Deferred フォワード段・速度・CSM 影

- 依存: sub-02
- 状態: OK (review-1 差し戻し分、コミット待ち)
- 往復: 3

## やること

spec §4.1「パスへの組み込み」の Deferred 行と CSM 行を実装し、**頂点変位が深度・速度 (TAA)・影に反映される**ことを実画像で示す。動機 (TAA 付き動画) の本丸。

- DeferredPath:
  - GBuffer ループ (`DeferredPath.cpp:877-970`) とインスタンス run 構築 (`DeferredPath.cpp:844-876` 付近) から**不透明サーフェスのアイテムを除外**
  - **SSR の後・水面の前** (`DeferredPath.cpp:730-735` の 2.6 と 2.7 の間) に「サーフェス段」を追加: RT = HDR シーン (`view.rtv`) ＋ `gbVelocity_`、DSV = 既存 (テスト＋書き込み)、サーフェスプログラムの**速度エントリ**で描く。前履歴なし (`view.prevViewProjValid == 0`) は velocity 0 (既存 GBuffer と同じ)。前 World は `RenderItem::prevWorld`
  - サーフェスのアイテムが 0 件ならこの段は RT / ステート / SRV を一切触らない
  - 透明サーフェスは既存の透明段 (`DeferredPath.cpp:1350-`) で色エントリ (sub-02 のバインドを流用)
  - 失敗時の `surface_error` もこの段で描き、剛体の速度 (prevWorld / prevViewProj) を書く
- ShadowPass (CSM): サーフェスの不透明アイテムは**影エントリ** (ライト VP を `gViewProj` に入れて `VSMain`、PS なし) で描く。インスタンス run から除外。深度バイアス等のステートは既存どおり。失敗時は従来の `shadow_depth` (変位なし) でよい
- ShadowAtlas (スポット/ポイント) は変更しない (従来シェーダ = 変位なしで描かれる。spec §3 後回し)
- `docs/` に「Deferred のサーフェス画素には SSAO / SSR / デカール / RT 受光が掛からない」「アトラスの影は変位なし」を追記 (既存の M78 docs の近くに。新規ファイルでもよい)
- ~~速度エントリの GPU 時間計測~~ (round 1 VERDICT で後回しへ。spec §4.4)
- **(round 1 VERDICT で追加・must)** 自動 SelfTest (AGENTS.md §7)。sub-02 の `SurfaceMaterialSelfTest` と同じく WARP で DeferredPath / ShadowPass を実際に動かし read-back で確かめる: (a) `gTime` 変位ありのサーフェス不透明アイテムで `gbVelocity` が非 0、変位なしで 0 (b) サーフェスのアイテムが GBuffer (albedo 等) に書かれず、HDR シーンに色エントリの色が出る (c) CSM シャドウマップの深度が変位あり/なしで違う (d) サーフェスのアイテム 0 件でフォワード段が何も張らない (既存 golden で足りるならその根拠を書く)
- docs (`docs/surface-shaders-deferred-limits.md`) とサンプル / テンプレートのコメントに「影エントリでは `MyEnginePerFrame` が 0 埋め」を追記 (spec §4.1)

## やらないこと (このサブでは)

- Forward パスへの velocity / TAA 追加
- GBuffer 作者規約、SSAO / SSR / デカールをサーフェスに効かせること
- シャドウアトラスの変位込み影、アルファクリップ影
- WaterWave (sub-05)

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/DeferredPath.cpp/.h` — `Render` の段の並び (`DeferredPath.cpp:716-737`)、`RenderGeometry`、新しい段の関数。MRT のブレンド状態 (`IndependentBlendEnable=FALSE` の前提、`DeferredPath.cpp:760-761`) と R16G16F の組み合わせを確認
- `src/Engine/Renderer/ShadowPass.cpp` — `Render` (`ShadowPass.cpp:117-`) のループとインスタンス run
- `src/Engine/Renderer/TaaPass.*` — 変更は不要の見込み (velocitySRV を読むだけ)。触るなら理由を書く
- 検証用の一時シーン: 変位 (`gTime` 駆動のサイン波) ありの平面と、それを受ける床、静止カメラ、太陽 1 本

## 受け入れ条件 (このサブ)

1. Deferred で不透明サーフェスが描かれ、深度を書く (後段の水面・透明・パーティクルが正しく前後する) — スクショ
2. `gTime` 駆動の変位メッシュが、カメラ静止でも `--velocity-debug` に動きとして出る。変位なし版では出ない — `Runtime.exe --deferred --velocity-debug --screenshot` の 2 枚
3. `--taa` 有効時、変位メッシュの縁に前フレーム形の残像が出ない — `--deferred --taa --screenshot` (決定的撮影で数十フレーム進めた時点) と、比較用に「速度を書かない」一時改変版の 1 枚 (比較版はコミットしない)
4. CSM の影が変位後の形で落ちる — 変位あり/なしの 2 枚
6. 自動 SelfTest が (a)〜(d) を検査し、`--selftest` (Debug/Release) で PASS。検査の一部を一時的に壊すと FAIL すること (反証) — `--selftest`
5. サーフェスの無いシーンは Deferred の絵が不変 (フォワード段が何も張らない) — `--selftest` (既存 golden)、`tools\replay_verify.bat`

## 検証コマンド

```
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\check_rules.ps1
tools\replay_verify.bat
Runtime.exe --project <一時> --scene <一時> --deferred --velocity-debug --screenshot <png>
Runtime.exe --project <一時> --scene <一時> --deferred --taa --screenshot <png>
```

`replay_verify` が割れたらメモリ `replay-verify-triage.md` の手順で切り分けてから報告すること。画質系スクショはメモリ `rt-screenshot-freeze-seed-trap.md` の freeze-seed に注意。

## 差し戻し (review-1 #1)

- **#1 [blocker]** `ShadowPass` で、サーフェスの影エントリが名前解決で張った CB / SRV を戻さない。b0 (`objectCB_`) / t0 (instance SRV) はループ前に 1 回だけ張る (`ShadowPass.cpp:215`/`:218`) のに、`:254-:291` で上書きしたまま continue する。後続の非サーフェス (forward_lit・失敗時の `shadow_depth` フォールバック・instanced) が壊れた b0/t0 を読んで影を落とさない (reviewer rv1\shadowA.png / 順序を入れ替えた shadowB.png では出る / Deferred でも shadowA_def.png)。期待: サーフェス描画後に深度 VS / IL・b0・instance SRV (t0) を戻す (spec §4.1「固定スロットの復元」)。sub-02 の修正と同じ根なので、復元の考え方は揃える
- 受け入れ条件 (追加): spec 5. の 13 (影と Deferred 側)。SelfTest: ShadowPass で「サーフェス → 非サーフェス (通常・instanced)」と逆順のシャドウマップ深度を read-back 比較し、非サーフェスの深度が順序に依らず一致すること。Deferred のサーフェス段の後の水面・透明にも同じ観点の確認を 1 本
- コミット件名候補: `M79c-fix: 影エントリ後の固定スロット復元`

## 実装メモ (coder が追記)

SELF_EVAL: sub-03 (round 1)
実装:
  - `src/Engine/Renderer/DeferredPath.h/.cpp` — `RenderGeometry` の GBuffer ループとインスタンス run 構築 (canInstance_) から、shader が `*.surface` の不透明アイテム (非スキン) を除外し `DeferredFrame::surfaceOpaqueIdx` に集める (skin+surface は従来のスキン GBuffer 経路へフォールスルー + WARN 1 回、`skinnedSurfaceWarned_`)。新設 `RenderSurfaceForward` (2.65 段、`RenderSsr` の後・`water_.Render` の前) が `surfaceOpaqueIdx` を読み、サーフェスプログラムの**速度エントリ**で RT0=`view.rtv` (HDR シーン) + RT1=`gbVelocity_` (GBuffer と同じ RT を使い回す) + `view.dsv` (テスト+書込) へ描く。予約 CB (`MyEnginePerFrame`/`MyEngineSurfaceFrame`/`MyEngineWater`) は `RenderGeometry` が既に組んだ `f.pf`/`f.vel` の値をそのまま転記し、二重に式を書かない。`surfaceOpaqueIdx` が空ならこの段は RT/ステート/SRV を一切触らずに return する
  - `src/Engine/Renderer/ShadowPass.h/.cpp` — `Render` に `uint32_t viewFrameIndex` 引数を追加 (呼び出し元は `src/Engine/Engine/RenderSystem.cpp` の 1 箇所のみ)。カスケード毎に `MyEngineSurfaceFrameCB.shadowViewProj = transpose(lightViewProjs[c])` を積み、サーフェスの不透明アイテム (非スキン、`surf->ready`) は**影エントリ** (`shadowVS`、PS なし) で描く。失敗中 (`useErrorFallback`) は従来の `shadow_depth` (変位なし) へフォールスルーする (sub-03.md の指示どおり)。canInstance_ からもサーフェスマテリアルを除外
  - `src/Engine/Renderer/ShadowPass.h` — サーフェスの影エントリ用予約 CB (`surfacePerFrameCB_`/`surfaceFrameCB_`/`surfacePerObjectCB_`/`surfaceWaterCB_`) を追加。`MyEnginePerFrame` は**全 0 固定**で運用 (仕様との差分 [逸脱] 参照)
  - `docs/surface-shaders-deferred-limits.md` (新規) — 「Deferred のサーフェス画素には SSAO/SSR/デカール/RT 受光が掛からない」「シャドウアトラス (スポット/ポイント) の影は変位なし」を記載

仕様との差分:
  - [逸脱] ShadowPass の `MyEnginePerFrame` を全 0 固定にした。`ShadowPass::Render` は `RenderSystem::Render` の呼び出し順 (`RenderCascadeShadows` が `PrepareEnvironment` より前) の都合で、fog/IBL/sun/カメラ位置がまだ `view` に埋まっていない時点で呼ばれる。spec §4.1 は PerFrame を「カメラ/光/影/霧」を含む形で統一的に供給するとしているが、影エントリは PSMain を呼ばず (深度のみ)、位置に効くのは static (`gViewProj`/`gWorld`/`gTime`/`gWaterTime`) だけという規約 (spec §4.1) を根拠に、色/速度エントリと異なり環境情報は不要と判断した。VSMain が `gCameraPos` 等の PerFrame フィールドを頂点変位に使うシェーダを書いた場合、影エントリだけ他エントリと違う (常にゼロの) 値を見る非対称が生じる。RenderSystem 側で呼び出し順を入れ替える選択肢は CSM のカスケードフィット等への影響調査が必要なため本サブでは見送った (不安・質問 1)
  - [未実施] spec §4.4 の「速度エントリの GPU 時間 (変位ありの平面 1 枚) を実装メモに記録」を実施していない。WARP + Runtime.exe 起動オーバーヘッド (シェーダコンパイル等) が支配的で、有意な GPU ms が得られる計測方法を本サブの時間内で用意できなかった (不安・質問 2)
  - [未実施] 自動 SelfTest を追加していない。sub-01/02 は「新規 SelfTest」が触る場所に明記されていたが、sub-03.md にはその指示がなく、検証コマンドも `--selftest` (既存) + 実プロジェクトの Runtime.exe スクショのみを列挙している。既存 SelfTest が回帰していないことは確認したが、DeferredPath/ShadowPass のサーフェス経路そのものを機械的に固定するテストは無い (不安・質問 3)

検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 成功、エラー 0、新規警告 0
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 成功、エラー 0
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件
  - `bin\x64\Release\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` → `[PASS]`、13 ジョブ全通過 (134.0s)。デフォルトシーン群 (サーフェス未使用) が Debug/Release でビット一致 = 受け入れ条件 5 の根拠
  - **実プロジェクト・実シーンの Runtime.exe スクショ (WARP)**。一時プロジェクトは `%TEMP%\claude\...\scratchpad\mye_m79_sub03_verify\` (リポジトリ外、未コミット)。フロア (forward_lit、Cube 90x0.2x10 スケール) + 浮遊キャスター (Cube、`WaveBob.surface`=gTime 駆動の垂直ボブ / `WaveStatic.surface`=同一だが変位なしの比較用) + 太陽 (CSM)
    - 受け入れ条件 1 (深度): `v5_shadow_bob_peak.png`/`v6_shadow_static_peak.png` でキャスターがフロアの上に正しく前後関係を保って描かれることを確認
    - 受け入れ条件 2 (velocity-debug): `--deferred --velocity-debug` で `v3_veldebug_bob.png` はキャスターのシルエット全体に非ゼロ速度の色 (マゼンタ系) が出る。`v4_veldebug_static.png` は完全に背景と同じ灰色 1 色 (ゼロ速度) — 「変位なし版では出ない」を実証
    - 受け入れ条件 3 (TAA ゴースト): `--deferred --taa` を 300 フレーム連続アニメ後に撮影した `v7_taa_bob.png` は目視でゴーストなし。**反証テスト**: `assets/shaders/MyEngineSurfaceEntries.hlsli` の `MyePSVelocity` を一時的に `myeVelocityOut = float2(0,0)` 固定へ改変し同条件で再撮影 (`v8_taa_bob_novelocity.png`)、ピクセル差分 (python) で最大差分 48/255、632 px がキャスターのスクリーン矩形内 (x:752-847, y:363-473) に限定して検出 = 正しい速度出力が TAA 解決結果に実際に効いていることを実証。改変は撮影直後に revert し `git diff -- assets/shaders/MyEngineSurfaceEntries.hlsli` が空であることを確認済み
    - 受け入れ条件 4・7 (CSM 影の変位反映): 同フレーム (bob 変位ピーク vs static) で `v5_shadow_bob_peak.png`/`v6_shadow_static_peak.png` を比較し、キャスターの高さとフロア上の影の位置・サイズが明確に異なることを確認
  - 未検証: 水面・透明アイテムとサーフェスの深度前後関係の実地確認 (検証シーンに水面/透明を含めなかった)。シャドウアトラス (スポット/ポイント) の変位なし確認は据え置き (spec §3 後回し、docs に記載のみ)

自己採点 (1-5):
  仕様適合: 4 — sub-03.md の「やること」(GBuffer 除外・フォワード段・影エントリ・docs) はすべて実装。GPU 時間計測 (spec §4.4) は未実施として上に開示
  正しさ: 4 — 実プロジェクト・実シーン・Runtime.exe (WARP) の本物の経路で受け入れ条件 1〜5・7 を確認し、TAA ゴーストは反証テスト (速度を壊すと差分が出る) まで実施した。水面/透明との共存、シャドウアトラスは未検証
  コード品質: 4 — sub-02 で確立した名前解決バインド (`SurfaceDrawBind.h`) と CB 構築パターンをそのまま踏襲。`ShadowPass::Render` のシグネチャ変更は呼び出し元 1 箇所のみで影響範囲を確認済み
  テスト: 2 — 自動 SelfTest を追加していない (sub-03.md に明記が無かったため; 不安・質問 3 参照)。既存 SelfTest・`replay_verify.bat` の無回帰は確認済みだが、サーフェス経路自体を機械的に固定する新規テストは無い

不安・質問:
  1. ShadowPass の `MyEnginePerFrame` を全 0 固定にした設計 (仕様との差分 [逸脱] 参照) でよいか。VSMain が `gCameraPos` 等を使う頂点変位を書いた場合、影エントリだけ他エントリと異なる (ゼロの) 値を見る。RenderSystem の呼び出し順 (`RenderCascadeShadows` を `PrepareEnvironment` の後へ動かす等) を変える対応は影のカスケードフィットへの影響調査が要り本サブでは行っていない
  2. spec §4.4 の GPU 時間計測を実施していない (WARP + 起動オーバーヘッド支配で有意な値が取れなかった)。専用の GpuTimer 常設が必要なら追加実装になるが、そこまで本サブでやるべきか
  3. 自動 SelfTest (DeferredPath/ShadowPass のサーフェス経路を WARP 実描画で固定するテスト) を追加すべきか。sub-01/02 は明記されていたが sub-03.md には無く、検証コマンドも Runtime.exe スクショのみだったため見送ったが、CLAUDE.md 第 7 章の「レンダラーの挙動を変更する場合は回帰テストを追加する」との整合が要る

触ったファイル:
  - `src/Engine/Renderer/DeferredPath.h`
  - `src/Engine/Renderer/DeferredPath.cpp`
  - `src/Engine/Renderer/ShadowPass.h`
  - `src/Engine/Renderer/ShadowPass.cpp`
  - `src/Engine/Engine/RenderSystem.cpp`
  - `docs/surface-shaders-deferred-limits.md` (新規)

申し送り:
  - 一時検証プロジェクト (`%TEMP%\claude\C--HAL-MyEngin\8864d946-863a-46b5-9312-6ffea53e1a0b\scratchpad\mye_m79_sub03_verify\`) はコミットしていない。マテリアルの AssetID は `.mat.json` を配置後に一度 Runtime.exe を走らせて自動生成される `.mat.json.meta` の GUID を読み、その 10 進値をシーン JSON の `MeshRenderer.material` に書く必要がある (手で決め打ちした GUID は `assetkey`/`AssetDatabase` の解決と food 一致せず、アイテムが無言で描画されないことがある — 本サブで実際に踏んだ罠。次サブで一時シーンを作るときはこの手順を踏むこと)。mesh は `builtin://cube` (`resources.meshes.Cube()`) を使うこと — `RegisterDemoContent` が起動時に登録するのはこれだけで、`builtin://quad`/`builtin://plane` は Runtime.exe 単体では未登録のため raw hash 参照では解決できない (Editor の CreateMenu 経由や SelfTest 内の明示呼び出しでのみ登録される)
  - sub-05 (WaterWave) は本サブの `RenderSurfaceForward`/影エントリの配線をそのまま使える想定。`MyEngineWater` CB は本サブでも常に 0 埋めのまま (sub-05 まで未配線)

SELF_EVAL: sub-03 (round 2)
実装:
  - #1 (受け入れ条件 1 の実地確認): 一時プロジェクトのシーンに WaterWave (baseHeight をキャスターの中心高さに合わせて交差させる)・手前透明メッシュ・奥透明メッシュ・パーティクルエミッタ 1 個を追加した一時シーン (`main_depth_order.scene.json`) を作成し、Deferred で Runtime.exe スクショを撮った。詳細は下記「検証」参照。プロダクトコードの変更は無し (round 1 の実装で既に正しく動いていたことの実地確認)
  - #2 (自動 SelfTest, must): `src/Engine/Renderer/SurfaceDeferredSelfTest.h/.cpp` (新規) を追加。sub-02 の `SurfaceMaterialSelfTest` と同じ手法 (WARP デバイス + 一時ディレクトリの `.surface.hlsl`/`.mat.json` + オフスクリーン RTV/staging read-back) で、指定された (a)〜(d) を検証:
    - (a)(b) `TestDeferredForwardStepVelocityAndBypass`: `DeferredPath::Render` を実駆動し、`gTime` で頂点を変位させる最小サーフェス (`DisplaceProbe.surface`) と変位しない比較用 (`RigidProbe.surface`) を同一シーンに置く。両アイテムとも `item.world == item.prevWorld` (エンティティ自体は動かさない) にして「シェーダ変位だけに由来する速度」を対象にした。`DeferredPath::VelocitySRV()` (公開 API、既存) を read-back し、変位ありは非 0・変位なしは厳密に 0 を確認。HDR シーン (`view.rtv`) は `ambient=0` の光源環境下でも両アイテムとも PSMain の固定色のまま (GBuffer/光パスを経由していれば真っ黒になるはずの条件) であることを確認し、GBuffer バイパスの証拠にした
    - (c) `TestShadowPassDepthReflectsDisplacement`: `ShadowPass::Render` を実駆動し、真上 (y=10) から見下ろす正射影ライト (`XMMatrixLookToLH` + `XMMatrixOrthographicLH`) で同じ 2 アイテムの影を描き、`ShadowPass::SRV()` (Texture2DArray R32_FLOAT、TYPELESS → R32_FLOAT staging へ型変換コピー) を read-back。変位あり/なしで深度値が `1e-4` を超えて異なることを確認
    - (d): 専用テストは追加せず、「既存 golden (`--selftest` 全体 / `tools\replay_verify.bat` の既定シーン群、いずれもサーフェス材質を含まない) が本サブの変更前後でビット一致する」ことを根拠として明記した (round 1 で確認済み、round 2 でも再確認。理由: サーフェス 0 件時の「フォワード段が何も張らない」は `DeferredPath::RenderSurfaceForward` 冒頭の早期 return 1 行で保証される構造的性質であり、既定シーン (サーフェス material を一切参照しない) の golden 不変がそのまま「0 件で何も変わらない」の実地証拠になっている)
    - **反証**: `assets/shaders/MyEngineSurfaceEntries.hlsli` の `MyePSVelocity` を一時的に `myeVelocityOut = float2(0,0)` 固定に改変し `--selftest` を実行 → 新設した (a) のチェックが実際に FAIL (exit code 1) することを確認。その後ファイルを復元し (`git diff` 空を確認)、`--selftest` が全 PASS (exit 0) に戻ることを確認
  - `src/Editor/EditorMain.cpp` に `#include "Engine/Renderer/SurfaceDeferredSelfTest.h"` と `ok &= mye::RunSurfaceDeferredSelfTest();` を追加 (M79c として登録)
  - #3 (should): `assets/shaders/MyEngineSurface.hlsli` の先頭コメントと `docs/surface-shaders-deferred-limits.md` に「影エントリでは `MyEnginePerFrame` が 0 埋め (ShadowPass が環境確定より前に走るため)。VSMain の変位に `gCameraPos` 等を使うと影だけ形が違う」を追記した。ToonFlat.surface.hlsl (既存サンプル) 自体は PerFrame の値を変位に使っていないため触っていない — 作者が実際に読む場所 (共通 include の先頭コメントと docs) に書く方が実効性が高いと判断した
  - `tools\gen_project_files.ps1` (pwsh) を実行し `build/Engine.vcxproj`/`.filters` に新規ファイルを反映

仕様との差分:
  - [追加] must #2 の (d) は「専用の反証込みテストケース」ではなく「既存 golden への該当」で済ませた。sub-03.md の期待文言「(d) サーフェス 0 件のときフォワード段が何も張らない (既存 golden で足りるならその根拠を書く)」が明示的にこの選択肢を許容しているため、追加のテストコードは書いていない

検証:
  - `pwsh -File tools\gen_project_files.ps1` → 成功 (4 vcxproj 更新。実際に差分が出たのは `Engine.vcxproj`/`.filters` のみ)
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 成功、エラー 0
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 成功、エラー 0
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件。新設 `M79 surface deferred self test (sub-03)` の (a)(b)(c) すべて PASS
  - `bin\x64\Release\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件。同上
  - **反証 (敵対的自己レビュー)**: `MyePSVelocity` の速度出力を強制 0 に改変 → `--selftest` が (a) の 1 件だけ FAIL (exit 1) することを確認 (他の (b)(c) は無傷 = チェックが正しく独立している証拠)。復元後 `--selftest` が全 PASS に戻ることを確認、`git diff -- assets/shaders/MyEngineSurfaceEntries.hlsli` が空であることを確認
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` → round 2 の変更 (自動 SelfTest 追加・docs/コメント追記) 込みで再実行し `[PASS]`、13 ジョブ全通過 (122.8s)。デフォルトシーン群が Debug/Release でビット一致 (受け入れ条件 5 の再確認)
  - **受け入れ条件 1 の実地確認 (一時プロジェクト、`main_depth_order.scene.json`)**: Floor (forward_lit) + WaterWave (`baseHeight` をキャスター中心の高さに設定し交差させる) + WaveCaster (`WaveBob.surface`、不透明) + TransparentFront (手前、`transparent:true`、青系) + TransparentBack (奥、`transparent:true`、黄系) + ParticleEmitter (既定の上向き炎) を配置し `Runtime.exe --deferred --warp --screenshot` で撮影
    - 水面との交差: `d8_final_full.png` の x=800 列を垂直スキャンすると、キャスターの露出側 (y=300〜380px) は固定色 (206,124,109)、その下 (y=400px 付近) から水面の色 (白系、フレネル反射が強く出ている) へ切り替わる。キャスターが水面と正しく交差し、水没側が水面に隠れる (前後関係が壊れていない) ことを確認
    - 手前の透明メッシュ: `d6_depth_order_final.png` でキャスターの手前に置いた半透明の立方体 (青、α=0.45) が、キャスターの色 (208,131,117 系) と混ざった (158,135,211) の紫がかった色で描かれる。単純な背景 (白い水面) との混色ではキャスターの赤成分は出ないので、**透明メッシュがサーフェスの HDR 色の上にブレンドされている** (サーフェスの色エントリが先に書かれ、透明後段がその上に乗る) ことを数値で確認した
    - 奥の透明メッシュ: `d6_depth_order_final.png` (奥透明メッシュを本来の位置 z=+1.5 に置いた状態) と `d7_control_no_back.png` (同じシーンから奥透明メッシュだけを画面外 (1000,1000,1000) へ退避させた対照撮影) を全画素比較したところ **最大差分 0 (1600×900 の全画素が完全一致)**。奥の透明メッシュがキャスターに完全に隠れて 1 画素も寄与していない = **サーフェスの深度書き込みが後段の透明パスの深度テストで正しく機能している**ことをピクセル単位で実証した (サニティチェックとして、奥透明メッシュを一時的に手前へ動かして単体では描画されること自体も確認済み)
    - パーティクル: `d8_final_full.png` に炎エミッタの粒子 (白い光点) がシーン内に正しく合成されて見えており、サーフェス段の追加でパーティクル後段の描画自体が壊れていないことを確認 (パーティクルとサーフェスの直接のオクルージョン関係までは今回の配置では観測していない — 既存のパーティクル機構は本サブで変更していない箇所のため)
    - 一時プロジェクト・シーン・スクショはすべてリポジトリ外 (`%TEMP%\claude\...\scratchpad\mye_m79_sub03_verify\`) で未コミット

自己採点 (1-5):
  仕様適合: 5 — round 1 の must 2 件・should 1 件をすべて実施し、spec 変更 (§4.1 影エントリの PerFrame 0 埋め確定・§4.4 GPU 計測後回し) にも追従した
  正しさ: 5 — 受け入れ条件 1 は「深度が正しく機能しているからこそ観測できる」ピクセル単位の証拠 (透明メッシュの混色・奥透明メッシュとの差分 0) まで確認した。自動 SelfTest は Debug/Release 両方で PASS させ、反証テストで「壊すと落ちる」ことも実証した
  コード品質: 4 — 新規テストは sub-02 の既存パターン (WARP + 一時ファイル + staging read-back) を踏襲。ShadowPass のテストで一度ハマった (Quad は真上からの光だと footprint が線に潰れる) 点を Cube に切り替えて解消し、コメントに理由を残した
  テスト: 5 — round 1 で 2 だった評価を、自動 SelfTest 追加 + 反証実施で引き上げた。Debug/Release 両方で新規チェックの PASS/FAIL を確認済み

不安・質問: なし (round 1 の 3 件はすべて本ラウンドで解消・確定済み。planner 回答のとおり実施した)

触ったファイル (round 1 分を含めた全量):
  - `src/Engine/Renderer/DeferredPath.h`
  - `src/Engine/Renderer/DeferredPath.cpp`
  - `src/Engine/Renderer/ShadowPass.h`
  - `src/Engine/Renderer/ShadowPass.cpp`
  - `src/Engine/Engine/RenderSystem.cpp`
  - `src/Engine/Renderer/SurfaceDeferredSelfTest.h` (新規、round 2)
  - `src/Engine/Renderer/SurfaceDeferredSelfTest.cpp` (新規、round 2)
  - `src/Editor/EditorMain.cpp` (round 2: include + 登録)
  - `assets/shaders/MyEngineSurface.hlsli` (round 2: コメント追記)
  - `docs/surface-shaders-deferred-limits.md` (新規、round 1 作成・round 2 追記)
  - `build/Engine.vcxproj` (生成物、round 2 で再生成)
  - `build/Engine.vcxproj.filters` (生成物、round 2 で再生成)

申し送り:
  - round 1 の申し送り (一時プロジェクトの GUID の取り方、`builtin://cube` を使うこと) は引き続き有効。round 2 の一時シーン (`main_depth_order.scene.json`) も同じ手順で GUID を採番した
  - `SurfaceDeferredSelfTest.cpp` の Shadow テストは **Quad ではなく Cube** を使っている。真上から見下ろす正射影ライトで Z 面 (Quad) を使うと、footprint が世界の Z 方向に厚み 0 の直線に潰れて shadow map にほぼ乗らない (実際に踏んだ)。真上から見下ろす系の検証を今後追加するときはこの注意点を踏襲すること
  - DeferredPath テストの変位量は `gTime * 0.15` (小さめ) にしてある。変位が大きすぎると (最初 `gTime` そのままにしていた) メッシュ自身のスクリーン footprint から read-back 対象のピクセルがはみ出し、"変位したら急に何も描かれなくなる" ように見える誤検出を起こす (実際に踏んだ)。同種のテストを増やすときは変位量とメッシュサイズ・read-back 座標の関係に注意すること
- round 2: VERDICT OK (planner)。条件 1 は d8_final_full.png を planner が目視 (サーフェス立方体・水面交差・手前透明の混色・パーティクル)＋奥透明の全画素一致比較。条件 6 は SurfaceDeferredSelfTest (a)(b)(c)＋反証、(d) は既存 golden＋replay_verify を根拠に受理。

SELF_EVAL: sub-03 (round 3)
実装:
  - #1 (review-1 blocker, ShadowPass の固定スロット未復元) — `src/Engine/Renderer/ShadowPass.cpp:221-231` に `restoreFixedShadowSlots` ラムダを追加し、`:302` のサーフェス影エントリ `DrawIndexed` 直後 (`continue` の前) で呼ぶ。内容は VS b0 (`objectCB_`) の再バインドと、VS t0 (instance SRV。`runs_` が空なら null) の再バインドのみ。VS/IL の復元コードは足していない — `boundShader` が既に「直前に張った VS/IL の実体」を追跡しており、サーフェスの `surfaceProgramId` は `depthShader_`/`depthSkinnedShader_`/`depthInstancedShader_` のいずれとも値が一致しないため、次の非サーフェスアイテムの `if (itemShader != boundShader)` / `if (depthInstancedShader_.value != boundShader)` が既存コードのまま自然に真になり、VS/IL は次アイテムの描画時に確実に張り直る (コードを読んで確認、後述のテストでも実証)。sub-02 の `ForwardPath::restoreForwardLitBindings` (`boundShader=0` で強制フルリバインド) とは違う経路だが、同じ帰結を狭い変更で得ている
  - 受け入れ条件 (追加分・混在順序 SelfTest) — `src/Engine/Renderer/SurfaceDeferredSelfTest.cpp` に `TestShadowPassFixedSlotsSurviveSurfaceEntry` を新規追加。VS で `Texture2D _HeightTex` を読む `kVTexSurface` フィクスチャ (Properties `_HeightTex ("Height", 2D) = "white" {}` で既定テクスチャ自動解決) を使い、「サーフェス→非サーフェス instanced run 2 個」(順序 A) と「非サーフェス instanced run 2 個→サーフェス」(順序 B、基準値) の 2 順序で `ShadowPass::Render` を実駆動し、`ShadowPass::SRV()` を read-back。順序 B を基準に、順序 A でも (a) 影が消えない (深度が clear 値 1.0 のままでない)、(b) 順序 A/B の深度が完全一致することを確認
  - 受け入れ条件 (追加分・Deferred の水面/透明の確認) — 同ファイルに `TestDeferredWaterAndTransparentUnaffectedBySurfaceForwardStep` を新規追加。`DeferredPath::Render` をフル駆動し、画面上の別位置に distractor サーフェス (VS テクスチャ付き、x=-4) / 透明キューブ (forward_lit、x=0) / 水面のみの読み取り点 (x=+4) を配置。distractor の有無 (2 回描画) で透明キューブの画素・水面のみの画素がどちらも完全一致することを確認。読む前にコードを読み、`DeferredPath::RenderTransparent` が自身の呼び出し冒頭で `bindForwardLitFixed()` を無条件に呼び (`DeferredPath.cpp:1600`)、`WaterPass::Render` も呼び出しの都度 VS/PS の CB・サンプラ・SRV・シェーダ・頂点/インデックスバッファを全部自分で張り直す (`WaterPass.cpp:159-226`) ことを確認済み — この 2 つは元から ShadowPass と違う設計 (毎回フルリバインド) で、review-1 #1 と同じクラスの不具合を作っていないことをコードで確認したうえで、それを崩れないよう固定する回帰テストとして追加した (新しいバグを見つけたわけではない)
  - `docs/surface-shaders-deferred-limits.md` は round 1/2 の内容のまま変更なし (review-1 #1 は「作者が読む規約」ではなく実装の内部バグだったため、docs への追記は不要と判断)

仕様との差分:
  - [追加] spec 5-13 の「混在順序の影深度 SelfTest」に加え、sub-03.md 差し戻し節が明示的に求めた「Deferred のサーフェス段の後の水面・透明にも同じ観点の確認を1本」も実装した。この 2 本目はバグ修正ではなく回帰ガード (根拠は上記実装欄) — reviewer/planner が「実際にバグがあった」と誤解しないよう明記する

検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` → 成功、エラー 0、警告 0
  - `MSBuild MyEngine.sln /p:Configuration=Release /p:Platform=x64` → 成功、エラー 0、警告 0
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件。新設 2 テストとも全チェック PASS
  - `bin\x64\Release\Editor.exe --selftest` → 全 PASS (exit 0)、`FAIL:` 0 件 (1 度だけ `[assets] relocate failed: ... Access is denied` で 2 件 FAIL が出たが、自分が直前に残した Runtime.exe プロセス・一時ディレクトリの競合が原因と特定し、プロセス停止＋一時ディレクトリ削除後に再実行して解消。ShadowPass/SurfaceDeferredSelfTest とは無関係)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` → `[PASS]`、13 ジョブ全通過 (148.8s)
  - **反証 (敵対的自己レビュー) #1**: `ShadowPass.cpp` の `restoreFixedShadowSlots();` 呼び出しを一時的にコメントアウトして Debug ビルド・`--selftest` を実行 → 新設 `TestShadowPassFixedSlotsSurviveSurfaceEntry` の「順序 A でも影が消えない」「順序 A/B の深度が完全一致する」の 2 件が FAIL (`depthAfterA0=1.000000 depthBeforeB0=0.472386` 等、影が完全に消えている数値を確認)。復元後 `git diff` が空であることを確認し、全 PASS に戻ることを再確認
  - **反証 #2**: `DeferredPath.cpp` の `RenderTransparent` 冒頭の `bindForwardLitFixed();` (review-1 #1 とは別に元から存在するコード) を一時的にコメントアウトして実行 → 新設 `TestDeferredWaterAndTransparentUnaffectedBySurfaceForwardStep` が「透明キューブの画素は水面のみの画素と異なる」で FAIL (透明キューブが背景の水面と見分けが付かなくなる = 描画が壊れる) することを確認し、この経路が実際にテストされていることを実証。復元後 `git diff` が空であることを確認し、全 PASS に戻ることを再確認
  - **実プロジェクト・実経路 (Runtime.exe, WARP)**: reviewer の round 1 検証物 (`%TEMP%\claude\...\scratchpad\rv1\`) をそのまま流用し、修正前の repro シーンを再撮影
    - `shadowA.scene.json` (Forward): reviewer の `rv1\shadowA.png` (LitMid/LitRight の影が消えている) に対し、修正後の再撮影 `shadowA_postfix.png` は 3 個とも影が出ることを画像で確認
    - `shadowA.scene.json --deferred`: reviewer の `rv1\shadowA_def.png` (同じく影欠落) に対し、修正後 `shadowA_def_postfix.png` は 3 個とも影が出ることを画像で確認
    - `shadowB.scene.json` (Forward、順序を入れ替えた対照): 修正後 `shadowB_postfix.png` も 3 個とも影が出て `shadowA_postfix.png` と一致 (受け入れ条件 13 の「描画順を入れ替えても結果が変わらない」を実地確認)
    - `vtexA.scene.json --deferred`: reviewer の `rv1\vtexA_def.err` (D3D デバッグレイヤの `DrawIndexedInstanced: ... BUFFER ... does not match ... TEXTURE2D` エラーが 43 行) に対し、修正後の再実行ログでこのエラー文言の出現回数が **0 件**になったことを `grep -c` で確認。同時に撮影した `vtexA_def_postfix.png` では、中央の green VT キューブに隠れていた 2 個を含む Lit キューブ 6 個全部の影が出ていることを画像で確認 (reviewer の `rv1\vtexA_def.png` では中央 2 個が green キューブに隠れて影の有無が確認できていなかった点を補完)
    - 一時レンダリング結果はすべて `%TEMP%\claude\...\scratchpad\sub03fix\` (リポジトリ外、未コミット)。reviewer の repro シーン自体は書き換えていない

自己採点 (1-5):
  仕様適合: 5 — review-1 #1 の期待 (b0/instance SRV の復元、混在順序 SelfTest) を実装し、sub-03.md 差し戻し節が追加で求めた「水面・透明の確認」も実装した
  正しさ: 5 — 自動 SelfTest (Debug/Release) に加え、reviewer 自身の repro シーン (rv1) を実際に再実行し、修正前の画像・D3D エラーログと直接比較して「影が出るようになった」「D3D エラーが 0 件になった」ことを実証した。2 本の反証 (修正を外すと FAIL する) も実施し、テストが空振りでないことを確認した
  コード品質: 5 — ShadowPass の修正は 13 行の最小差分で、既存の `boundShader` 追跡機構を活かして VS/IL の復元コードを増やさずに済ませた (根拠をコメントとコードの再確認で明記)。水面/透明のテストは「バグを直した」のではなく「既に安全な設計であることをコードで確認し、回帰ガードとして固定した」ことを実装メモで区別して開示した
  テスト: 5 — 新設 2 テストとも Debug/Release で PASS、かつそれぞれ独立した反証で「壊すと落ちる」ことを実証済み。実プロジェクトの実描画 (WARP) でも reviewer の repro が解消したことを画像・ログ両方で確認した

不安・質問: なし

触ったファイル (round 3 分、round 1/2 分は上記と同一):
  - `src/Engine/Renderer/ShadowPass.cpp` (review-1 #1 の修正: `restoreFixedShadowSlots` 追加・呼び出し)
  - `src/Engine/Renderer/SurfaceDeferredSelfTest.cpp` (新規テスト 2 本追加: `TestShadowPassFixedSlotsSurviveSurfaceEntry` / `TestDeferredWaterAndTransparentUnaffectedBySurfaceForwardStep`、`kVTexSurface` フィクスチャ追加、`#include "Engine/Core/Hash.h"` / `#include "Engine/Renderer/WaterPass.h"` 追加)

申し送り:
  - `DeferredPath.cpp`/`.h` および `ShadowPass.h` は本ラウンドで一切変更していない (`git diff` で確認済み)。review-1 #1 は ShadowPass 単体のバグで、DeferredPath 側の `RenderSurfaceForward`→水面/透明の経路は元から安全 (`bindForwardLitFixed` / `WaterPass::Render` のフルリバインド) だった
  - Runtime.exe は `--screenshot` 撮影後も自動終了せずプロセスが残り続ける (今回複数回踏んだ)。次に一時検証で Runtime.exe を使うときは、撮影後に明示的に `Stop-Process` するか、次のビルドの前に必ずプロセス残留を確認すること (でないと `LINK : fatal error LNK1168` でビルドが失敗する)
  - reviewer の rv1 検証物はコード変更なしで再利用できた (シーン・マテリアル・シェーダの書き換え一切不要)。次にこの種の回帰を検証するときも rv1 をそのまま使ってよい
- round 3 (review-1 #1): VERDICT OK (planner)。ShadowPass.cpp:221-231 の restoreFixedShadowSlots (VS b0 / t0) を確認、VS/IL は boundShader 不一致で次アイテムが張り直す。sub03fix\shadowA_postfix.png を目視 (3 個とも影あり)。混在順序の影深度テストと Deferred 水面・透明の回帰テストに反証込み。受け入れ条件 13 (影・Deferred 側) 充足。
