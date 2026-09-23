# sub-02: Forward 描画とマテリアル (遅延 Load・横テーブル・properties・マゼンタ)

- 依存: sub-01
- 状態: OK (コミット待ち)
- 往復: 2

## やること

spec §4.1「パスへの組み込み」の Forward 行、「失敗時」、§4.2 `.mat.json` を実装し、**Forward パスで作者の式のメッシュが見える**縦切りを通す。

- `MaterialLibrary`: `.mat.json` の `shader` が `*.surface` のとき、シェーダを**遅延 Load** (sub-01 のサーフェス用ロード) し、`Material` POD の外の**横テーブル** (マテリアル AssetID → シェーダ名・Properties 値・パック済み PerMaterial バイト列・Tex2D の AssetID) に持つ。`properties` の JSON 符号化は fxstack と同じ (`FxStackAsset.cpp:19`)。スキーマに無いキーは保持して CB には書かない
- PerMaterial のパックは **リフレクションのオフセット**で行う (spec §2 の PerMaterial 行)。Properties にあって cbuffer に無い名前は WARN、サイズ不一致は WARN＋書かない。Tex2D は作者 `Texture2D` の名前スロットへ (未割当 / 未解決は M78 と同じ組込み既定、不明名は white＋WARN)
- `.mat.json` のホットリロード / シェーダのホットリロードで横テーブルとパックを作り直す (既存の再読込経路に乗せる)
- ForwardPath: 不透明ループと透明ループで、サーフェスマテリアルのアイテムはサーフェスプログラムの**色エントリ**で描き、予約 CB (PerFrame / SurfaceFrame / PerObject、`MyEngineWater` は 0 埋め) と予約テクスチャ・サンプラ・PerMaterial・作者テクスチャを**名前スロットへ**張る。描いた後に既存 forward_lit のバインド前提 (b0-b2, t0-t9, s0-s2) を壊さないこと (次のアイテムが forward_lit のとき)
- 失敗 (名前なし / コンパイル失敗 / 規約違反 / Properties パース失敗) は `surface_error` でマゼンタ描画＋ Console ERROR (同じマテリアルで毎フレーム出さない)。ホットリロード失敗は旧プログラム維持
- スキン＋サーフェスは従来のスキン経路＋WARN 1 回。インスタンス run はもともと forward_lit 限定なので変更不要であることを確認
- **(sub-01 から移管・must)** `SurfaceProgram` をホットリロード (`RequestRecompileForFile` / `PollAsyncCompiles` の include 依存グラフ。作者ファイル・`MyEngineSurface.hlsli`・`MyEngineSurfaceEntries.hlsli` の変更で再コンパイル、失敗は旧プログラム維持＋WARN) とバイトコードキャッシュ (生成エントリ込みのソース全体をキー、spec §4.4) に乗せる。同じ名前の `LoadSurface` を繰り返しても再コンパイルしないこと
- **(should)** `MyeApplyFog` にフロクセル合成を足す (既存 forward_lit と同じ判定。CB フィールドは sub-01 で確保済み、位置を変えない)
- Editor が使う公開 API: 「マテリアルのシェーダ状態 (OK / 失敗とエラー文)」を取れる口 (sub-04 のバナーが使う)。形は coder 判断
- サンプル: エンジン `assets/shaders/` に平塗り (トゥーン) の `*.surface.hlsl` を 1 本 (M78 の `MyTint.post.hlsl` と同じ位置付け。既定シーンからは参照しない)

## やらないこと (このサブでは)

- Deferred パス・速度・影 (sub-03)。**Deferred では従来どおり GBuffer に描かれてよい** (このサブの時点では)
- Inspector・作成メニュー (sub-04)
- WaterWave (sub-05)

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/GpuResources.h/.cpp` — `MaterialLibrary`、`ParseMaterialJson` (`GpuResources.cpp:1046-`、「フィールドを足すときはここだけ」の唯一の本体)。遅延 Load には ShaderManager への参照が要る — 読み込み時に渡すか、描画時に名前から解決するかは coder 判断 (層の向きに注意: Renderer 内で閉じる)
- `src/Engine/Renderer/ForwardPath.cpp` — `DrawItems` (`ForwardPath.cpp:392-476`)
- `src/Engine/Renderer/ProjectShaderProperties.*` — パース再利用。リフレクションオフセット版のパックは新関数でよい (M78 の `PackProperties` の意味は変えない)
- マテリアルのホットリロード経路 (`src/Engine/Engine/HotReload/` 周辺)
- SelfTest: `.mat.json` の `properties` 読み込み→リフレクションオフセットでのパック、失敗時にエラーシェーダへ落ちる判定、既存 `CookedCacheSelfTest` が無変更で通る (`sizeof(Material)` 不変)

## 受け入れ条件 (このサブ)

1. `shader: "X.surface"` のメッシュが Forward パスで作者の式で描かれる (不透明・透明とも) — 一時シーン＋`Runtime.exe --screenshot`
2. `properties` の Float/Range/Color/Vector/2D が描画に効く — 値違い 2 枚のスクショ＋`--selftest` (パック)
3. 失敗 4 種でマゼンタ＋Console エラー、クラッシュなし。ホットリロード失敗で旧絵維持 — スクショ＋ログ抜粋
4. サーフェスの無いシーンは Forward の絵が不変、`sizeof(Material)` 不変 — `--selftest` (既存 golden / Cooked 系)
5. サーフェスとforward_lit のアイテムが混在しても forward_lit 側の見た目が壊れない (バインドの張り直し) — 混在一時シーンのスクショ

## 検証コマンド

```
tools\gen_project_files.ps1   (ファイル追加時)
bin\x64\Debug\Editor.exe --selftest
tools\check_rules.ps1
Runtime.exe --project <一時プロジェクト or Water のコピー> --scene <一時シーン> --screenshot <png>
```

一時シーン・一時アセットはコミットに残さない (Water プロジェクトの本物のファイルは書き換えない。必要ならコピーで検証)。スクショの取り方はメモリ `screenshot-probe-recipes.md` (シーン撮りは Runtime.exe)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
実装:
  - `src/Engine/Renderer/GpuResources.h/.cpp` — `SurfaceMaterialState`(公開構造体: ready/useErrorFallback/errorMessage/perMaterialCB/perMaterialGpuCB/textures) を新設。`MaterialLibrary` に横テーブル `surfaceSources_`(shaderName・properties JSON テキスト・assetsRoot・revision) と `GetOrBuildSurfaceState(materialId, shaders, textures, device)` を追加。`ParseMaterialJson` に `shaderNameOut`/`propertiesJsonOut` の出力引数を追加 (唯一の本体はそのまま)。`LoadFromFile` が shader 名の `.surface` 接尾辞判定で横テーブルを更新/削除する。Properties→PerMaterial のパックはリフレクションのオフセットで行い (`ProjectShaderProperties::PackPropertiesReflected` 新設)、Tex2D は数値 GUID (Material の texture/normalMap と同じ規約) / 文字列 (ビルトイン名・16 進 GUID・相対パス) を受理して解決する
  - `src/Engine/Renderer/ProjectShaderProperties.h/.cpp` — `PackPropertiesReflected`/`ReflectedVarSlot` を追加 (`PackProperties` のパース順パックとは別関数、意味は変えない)
  - `src/Engine/Renderer/SurfaceProgram.h` — `SurfaceProgram` に `propertiesSchema`(Properties DSL のパース結果) と `generation`(ホットリロード世代番号) を追加
  - `src/Engine/Renderer/ShaderCache.h/.cpp` — `ShaderCacheEntry::isSurface` を追加 (5 blob: 色VS/PS・影VS・速度VS/PS)、`SurfaceShaderCacheConfigKey`/`SurfaceShaderCacheFileName` を新設。`kVersion` を 1→2 (blob 数の妥当性検査に isSurface を混ぜたため。既存 CS/VS+PS キャッシュも含め次回コンパイル時に 1 回だけ再生成されるが、バイト列自体は変わらないので描画結果に影響しない)
  - `src/Engine/Renderer/ShaderManager.h/.cpp` — `CompileSurfaceProgram` に `previousGeneration` 引数、Properties パース (失敗=シェーダ全体無効)、バイトコードキャッシュ (`TryLoadCachedSurface`/`InstantiateSurface`、キーは生成エントリ込みの結合ソース全体のハッシュ + include 依存の中身ハッシュ) を追加。`RequestRecompileForFile`/`PollAsyncCompiles` に `surfacePrograms_` 用の非同期経路 (`asyncSurface_`) を追加し、失敗時は旧プログラム (generation 含め) を維持する
  - `src/Engine/Renderer/SurfaceDrawBind.h` (新規) — `BindSurfaceNamedCB/SRV/Sampler`(D3DReflect の名前解決で VS/PS 両方へバインド)。Forward 専用にせず共有ヘッダに切り出し、sub-03 (Deferred) からも使える形にした
  - `src/Engine/Renderer/ForwardPath.h/.cpp` — サーフェス予約 CB 4 本 (`surfacePerFrameCB_`/`surfaceFrameCB_`/`surfacePerObjectCB_`/`surfaceWaterCB_`、forward_lit の b0-b2 とは別バッファ) と `surfaceErrorId_`(`surface_error` を `LoadSurface` 経由でプリロード) を追加。`Render` で `MyEnginePerFrameCB`/`MyEngineSurfaceFrameCB`(spec §2 の時計 `viewFrameIndex/60`)/`MyEngineWaterCB`(sub-05 まで 0 埋め) を構築・アップロード。`DrawItems` に `GetOrBuildSurfaceState` 判定を追加し、サーフェスマテリアルは新設 `DrawSurfaceItem`(色エントリのみ、D3DReflect 名前解決で予約 CB・予約テクスチャ/サンプラ・`MyEnginePerMaterial`・作者 Texture2D をバインド) で描画。スキン+サーフェスは従来のスキン経路へフォールスルーしつつ WARN 1 回 (`skinnedSurfaceWarned_` でマテリアル毎に重複抑止)。サーフェス描画の直後に `restoreForwardLitBindings`(ラムダ) で forward_lit の b0-b2・s0-s2・t1-t9・IA/VS/PS 前提を必ず復元し、続くアイテムがどちらの種類でも壊れないようにした
  - `assets/shaders/MyEngineSurface.hlsli` — `MyeApplyFog` にフロクセル合成を追加 (should)。`screenPos` は SV_Position を引数に足さず `posW` を `gViewProj` で再投影して求める (作者規約のシグネチャ `(color, posW)` を変えないための代償)。`clip.w<=0` は解析フォグのみへ縮退 (0 除算/NaN を作らない。`ComputeVelocityUv` と同じ規約)。fxc の X4000/X4008 誤検知警告は「if の両分岐で return せず 1 つの result に代入して最後に 1 回だけ return する」形へ直して解消した (実害のある警告ではないが、作者が書く同種のコードで無用な警告が出ないよう解消しておいた)
  - `assets/shaders/ToonFlat.surface.hlsl`(+.meta) — サンプル (M78 の `MyTint.post.hlsl` と同じ位置付け。平塗りトゥーン、Color/Range/Tex2D の 3 種のプロパティを使用。既定シーンからは参照しない)
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.{h,cpp}` (新規、`EditorMain.cpp` 末尾 append で登録) — (1) `PackPropertiesReflected` がパース順ではなくリフレクションのオフセットで詰めることの単体検証、(2) `ShaderManager`: `LoadSurface` の冪等性 (同名を繰り返しても再コンパイルしない)・ホットリロード成功/失敗 (世代番号・旧プログラム維持)・バイトコードキャッシュの hit/miss/ソース変更時の無効化、(3) `MaterialLibrary::GetOrBuildSurfaceState`: Properties (Color/Range/Tex2D) の実コンパイル経由でのパック照合・Tex2D 既定フォールバック・`.mat.json` 再読込での再パック・失敗時 (存在しない shader 名) のエラーフォールバック・対象外マテリアルで nullptr、(4) **`ForwardPath` を実 WARP デバイスで実際に `Render` し** オフスクリーン RTV を read-back: サーフェス色エントリの固定色描画・forward_lit 単体との比較によるバインド非破壊の検証・存在しない shader 名でのマゼンタ描画・properties._Color の値違いが実描画に反映されること (2 マテリアルで別の色になることも含む)、(5) `assets/shaders/ToonFlat.surface.hlsl` がそのままコンパイルできること
  - `tools\gen_project_files.ps1` を実行 (`build/Engine.vcxproj`・`.filters` に新規ファイルを反映)

仕様との差分:
  - [追加] `.mat.json` の `properties` にある Tex2D 値の符号化規則を明示的に決めた。spec §4.2 は「fxstack (`FxStackAsset.cpp:19`) と同じ」と書くが、fxstack の `ParseProperties` は文字列 (ビルトイン名/16進 GUID) のみを Tex2D として扱う一方、`Material` 本体の `texture`/`normalMap` フィールドは**数値 GUID** を使う (両者は同じファイル内で矛盾する規約)。数値=GUID (Material 本体と同じ)・文字列=ビルトイン名/16進GUID (fxstack と同じ)/assetsRoot 相対パスの**両方**を受理する規則にした (`ResolveSurfaceTexProperty`、GpuResources.cpp)。sRGB は常に true でロードする (Properties DSL に sRGB 指定が無いため。ノーマルマップ用途の Tex2D プロパティでは色が歪みうる — 後回し)
  - [追加] `MyeApplyFog` のフロクセル合成 (should) で `screenPos` を SV_Position 引数無しに `posW`×`gViewProj` の再投影で求める設計にした。色エントリ (Forward) では `gViewProj`=今フレームの VP なので正しいが、速度エントリの `PSMain` 呼び出し (`MyePSVelocity` 内、sub-03) では `gViewProj` が「前」側のまま呼ばれるため screenPos がずれる。sub-02 は Forward (色エントリのみ) の範囲でこの経路を使わないため実害は無いが、sub-03 実装時に確認/対処が要る (コード内コメントに明記)
  - [未実装] Editor 公開 API (sub-04 のバナー用) は専用の新関数を作らず、既存の `MaterialLibrary::GetOrBuildSurfaceState` + 公開構造体 `SurfaceMaterialState`(ready/useErrorFallback/errorMessage) を Editor が `ctx.resources`/`ctx.shaders`/`ctx.device` 経由でそのまま呼ぶ形にした (sub-02.md「形は coder 判断」の範囲)
  - [未検証] sub-02.md の検証コマンドが挙げる `Runtime.exe --project <一時プロジェクト> --scene <一時シーン> --screenshot` は実行していない。代わりに `ForwardPath::Render`/`DrawSurfaceItem` を実 WARP デバイスで直接駆動し、オフスクリーン RTV の read-back で (a) サーフェス色エントリの固定色描画 (b) forward_lit 混在時の非破壊 (c) 失敗時マゼンタ (d) `.mat.json` の Properties 値違いが実描画に反映されること、を数値で確認した (`TestForwardPathDrawsSurfaceItems`)。理由: 一時プロジェクト/シーン JSON の形式を手探りで組むより、`ForwardPath` 自体を直接検証する方が対象コードへの被覆が高いと判断したため。ただし RenderSystem の収集 (シーン→RenderQueue) を経由する本物の E2E 経路 (プロジェクトマネージャ/アセットブラウザ経由の `.mat.json` 参照・Runtime.exe の CLI 起動) は未確認
  - [未実装] transparent キューでのサーフェス描画は `DrawItems` を共有するため理論上は動くはずだが、専用の自動テストは無い (実描画テストは opaque のみ)
  - [未実装] スキン+サーフェスの WARN 経路 (`skinnedSurfaceWarned_`) は自動テストが無い (コードレビューのみ)
  - [未実装] シェーダのホットリロード (`SurfaceProgram::generation` の変化) によるマテリアル側の再パックは、`.mat.json` 再読込 (revision 変化) による再パックほど直接的な自動テストが無い (同じコードパスを通るため対称性から正しいと判断)

検証:
  - `pwsh -File tools\gen_project_files.ps1` → 成功 (Engine/Editor/Runtime/GameLogic の 4 vcxproj 更新)
  - MSBuild (`MyEngine.sln`, Debug|x64, `/m`) → 成功、エラー 0。警告は既存 `ProjectComputeRunnerSelfTest.cpp` の C4127 (M78d、無関係) 7 件のみで新規警告 0 件
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit code 0)。ログ全体で `FAIL:` 0 件。新設 `M79 surface material self test` は PackPropertiesReflected 単体・ShaderManager (冪等性/ホットリロード成功失敗/バイトコードキャッシュ hit-miss)・MaterialLibrary (パック照合/Tex2D 既定/再読込/失敗フォールバック)・ForwardPath 実描画 (固定色/混在非破壊/失敗マゼンタ/Properties 値違い)・サンプルシェーダコンパイル、すべて PASS
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - HLSL コンパイル警告: `MyeApplyFog` の追加直後は fxc が X4000 (未初期化の疑い)/X4008 (0 除算の疑い) を誤検知していたのを確認し、単一 return 点への書き直しで解消 (再実行して 0 件を確認)
  - Release 構成のビルド/selftest・`tools\replay_verify.bat` は未実行 (sub-01 と同様、このサブの検証コマンドに含まれていないため)

自己採点 (1-5):
  仕様適合: 4 — sub-02.md の「やること」(遅延Load・横テーブル・properties・マゼンタ・must のホットリロード/キャッシュ・should のフロクセル・サンプル) は全て実装した。「仕様との差分」に記載した Tex2D 符号化の解釈追加と、Editor 公開 API を専用関数にしなかった判断は disclose 済み
  正しさ: 4 — 実 WARP デバイスで `ForwardPath::Render` を直接駆動し、色エントリ描画・forward_lit との混在非破壊・失敗時マゼンタ・Properties 値違いの反映をピクセル数値で確認した。ShaderManager のホットリロード/キャッシュも実際の非同期経路で確認済み。未確認なのは transparent キュー経路とプロジェクト/シーン JSON を経由する本物の E2E (Runtime.exe 起動) のみ
  コード品質: 4 — 既存の名前解決パターン (sub-01 の `SurfaceEntryReflection`、M78e `ComputeAbiRunner` のリフレクション手法) を踏襲。バインド復元 (`restoreForwardLitBindings`) を明示的な関数として切り出し、名前解決バインドは `SurfaceDrawBind.h` として Deferred (sub-03) からも使える形に共有した
  テスト: 5 — CPU 側 (Properties パース/パック)・ShaderManager (ホットリロード/キャッシュ)・MaterialLibrary (統合)・ForwardPath (実 WARP 描画+read-back) の 4 層すべてに機械的検証を追加し、実行して確認済み

不安・質問:
  1. Tex2D プロパティの GUID 符号化 (数値 GUID + 文字列の両方を受理) は spec の「fxstack と同じ」という記述と厳密には一致しない解釈拡張。sub-04 (マテリアル Inspector) がテクスチャピッカーで properties を書き出すとき、この規則 (両対応) を前提にしてよいか確認したい
  2. `MyeApplyFog` のフロクセル合成が速度エントリ (sub-03) から見ると `gViewProj` が「前」側のままになりうる件 (仕様との差分参照)。sub-03 で speed エントリの `PSMain` 内から `MyeApplyFog` を呼ぶ場合は、専用の再投影ロジック (速度エントリ専用の static や別ヘルパ) が要るかもしれない。方式は sub-03 の coder 判断でよいか、それとも sub-02 の時点で用意すべきだったか
触ったファイル:
  - `src/Engine/Renderer/GpuResources.h`
  - `src/Engine/Renderer/GpuResources.cpp`
  - `src/Engine/Renderer/ProjectShaderProperties.h`
  - `src/Engine/Renderer/ProjectShaderProperties.cpp`
  - `src/Engine/Renderer/SurfaceProgram.h`
  - `src/Engine/Renderer/ShaderCache.h`
  - `src/Engine/Renderer/ShaderCache.cpp`
  - `src/Engine/Renderer/ShaderManager.h`
  - `src/Engine/Renderer/ShaderManager.cpp`
  - `src/Engine/Renderer/ForwardPath.h`
  - `src/Engine/Renderer/ForwardPath.cpp`
  - `src/Engine/Renderer/SurfaceDrawBind.h` (新規)
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.h` (新規)
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.cpp` (新規)
  - `src/Editor/EditorMain.cpp`
  - `assets/shaders/MyEngineSurface.hlsli`
  - `assets/shaders/ToonFlat.surface.hlsl` (新規)
  - `assets/shaders/ToonFlat.surface.hlsl.meta` (新規)
  - `build/Engine.vcxproj` (生成物。`tools\gen_project_files.ps1` 実行結果。harness.md 申し送りに従い明記)
  - `build/Engine.vcxproj.filters` (同上)
申し送り:
  - `tools\gen_project_files.ps1` を実行済み。`build/Engine.vcxproj`・`build/Engine.vcxproj.filters` が新規ファイルを反映して変化している (コミット対象に入れないとビルドできない)
  - **環境メモ**: `powershell -File tools\gen_project_files.ps1` (Windows PowerShell 5.1) は UTF-8 の日本語コメントを誤読して構文エラーになった。`pwsh -File ...` (PowerShell 7) なら正常に動く。M79 と無関係の環境差だが、次サブ以降も `pwsh` を使うこと
  - `Editor.exe --selftest` を実行すると `assets/deepmodal/deepmodal_creatures.dmnet` という新規ファイルが untracked で作られる (実行時刻から M79 のテストではなく既存の deepmodal 系 SelfTest の副作用と思われる)。M79 の範囲外なので触っていないが、コミット時に紛れ込まないよう注意
  - ShaderCache の形式バージョンを 1→2 に上げた (`isSurface` フィールド追加のため)。ディスク上の既存キャッシュファイル (`*.shc`、あれば) は次回コンパイル時に 1 回だけ再生成される (バイト列自体は変わらないので描画結果に影響しない)
  - sub-03 (Deferred) は `SurfaceDrawBind.h` の名前解決バインドと `SurfaceMaterialState`/`GetOrBuildSurfaceState` をそのまま再利用できる想定。ただし「不安・質問 2」の `MyeApplyFog` フロクセル合成 (速度エントリからの呼び出し時の `gViewProj` 問題) は要確認
  - sub-04 (Inspector) は `MaterialLibrary::GetOrBuildSurfaceState(matId, *ctx.shaders, ctx.resources->textures, *ctx.device)` を呼べば `SurfaceMaterialState::ready`/`errorMessage` でバナー表示ができる (専用 API は用意していない)

SELF_EVAL: sub-02 (round 2)
実装:
  - `assets/shaders/MyEngineSurfaceEntries.hlsli` — **must #1 の修正**。`MyePSColor` と `MyePSVelocity` (PS エントリ) が `PSMain` を呼ぶ前に、位置用 static (`gViewProj`/`gWorld`/`gTime`/`gWaterTime`) へ「今フレーム」の値 (`gMyeCurViewProj`/`gMyeWorld`/`gMyeCurTime`/`gMyeCurWaterTime`) を代入し直すようにした。VS と PS は別プログラム (別々の D3DCompile) なので、VS エントリでの代入は PS には届かず、修正前は PS から見た static が常に未代入 (ゼロ行列 / 0) だった。ファイル冒頭にも「VS と PS は別プログラム」の注記を追加
  - `assets/shaders/MyEngineSurface.hlsli` — `MyeApplyFog` のコメントを修正 (「速度エントリの PSMain から見ると前側のまま」は上の修正で解消したので、「PS エントリは今フレームの値を代入し直すので常に今フレームの VP になる」に書き換え)
  - `src/Engine/Renderer/SurfaceShaderSelfTest.cpp` — **回帰テスト**。`VelocityProbe.surface` の `PSMain` を `float4(gTime, 0, 0, 1)` に変更 (色 read-back を追加)。draw A (`curTime=0.7`) で PS 側の gTime が今フレーム値 (0.7) を見ることを確認 — 修正前はここが 0 になり FAIL することを実際に確認済み (下記「検証」参照)。draw B は read-back 経路自体の生存確認として残す (curTime=0.0 なので単独では未代入と区別できないことをコメントで明記)
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.cpp` — **回帰テスト (ForwardPath 経由)**。`PsStaticsProbe.surface` (gTime を R、posW を gViewProj で再投影した NDC.x を G へ出す) を追加。`view.viewFrameIndex=30` (gTime=0.5) で off-center (x=-2) のクワッドを描き、R≈127・G≈42 (今フレームの値) を確認。修正前は R=0・G≈127 (未代入=ゼロ行列の再投影) になることを実際に確認済み
  - **must #2: 実プロジェクト/実シーン経由の screenshot 検証**。`%TEMP%\claude\...\scratchpad\mye_m79_sub02_verify\` に一時プロジェクトを作成 (assets/shaders・assets/materials・assets/scenes、リポジトリ外・コミットしない)。`.mat.json` ごとに `.meta` を自作し GUID を固定値にすることで (`EnsureMeta` が既存 GUID を尊重する規則を利用)、`MeshRenderer.material` の数値をハッシュ計算に頼らず確定させた。`Runtime.exe --project <一時> --scene <一時> --warp --no-audio --frames 10 --shot-frame 5 --screenshot <png>` で撮影。詳細は下記「検証」参照

仕様との差分:
  - [発見・修正] must #1 で指摘された PS 側 static 未代入は、round 1 の「MyeApplyFog は速度エントリから見ると前側のままな場合がある (sub-03 で解消)」という差分記載そのものが**誤りだった**根本原因だったことが判明。round 1 の記載は撤回し、spec §4.1 の明文化・本ラウンドの修正で解消済み

検証:
  - MSBuild (`MyEngine.sln`, Debug|x64, `/m`) → 成功、エラー 0、新規警告 0
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit 0)。`FAIL:` 0 件
  - **反証テスト (敵対的自己レビュー)**: `MyePSColor`/`MyePSVelocity` の static 代入を一時的にコメントアウトして `--selftest` を実行 → 新設した 2 件の回帰チェック (`velocity entry の PSMain は gTime に今フレームの値 (0.7) を見る` / `色エントリの PSMain は gTime/gViewProj に今フレームの値を見る`) が**実際に FAIL** (exit code 1) することを確認。その後ファイルを復元し、`--selftest` が全 PASS (exit 0) に戻ることを確認。回帰テストが「0 のままなら落ちる」ことを実地で立証した
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - **Runtime.exe による実経路スクショ (accept 1/2/3/5)**:
    - `gallery.png` (5 クワッド: ToonOpaque(不透明)・ToonTransparent(`transparent:true`)・LitMixed(`forward_lit`)・PropsA・PropsB(同じシェーダで `_Tint` 違い)) — 全て意図した色で描画され、forward_lit と混在しても互いに壊れないことを確認。ログ (`gallery.log`) にエラーなし
    - `blend_check.png` (同じ `_Tint`=(0.9,0.1,0.1,0.5) を `transparent:false`/`true` で並べたもの) — opaque 側は不透明な赤、transparent 側は空 (背景グラデーション) と混ざった淡いピンクになり、アルファブレンドが実際に効いていることを視覚確認
    - `failures.png` (4 クワッド: 名前なし `DoesNotExist.surface`・コンパイル失敗 `FailCompile.surface` (構文エラー)・規約違反 `FailRegulation.surface` (`pos`→`position`)・Properties パース失敗 `FailPropsParse.surface`) — 4 枚ともマゼンタで描画、クラッシュなし。`failures.log` に 4 種それぞれ異なるエラー文 (`shader file not found` / `error X3000: syntax error` / `error X3018: invalid subscript 'pos'` / `プロパティ行のパース失敗`) を確認
    - **ホットリロード (Editor 実行中相当、`--shot-every`)**: `Runtime.exe --frames 900 --shot-every 90` をバックグラウンド起動し、実行中に `ToonColor.surface.hlsl` へ意図的に構文エラーを追記。ログに `[reload] surface shader recompiling` → `[WARN] [reload] surface shader compile failed - keeping previous shader: ...error X3000: unrecognized identifier 'this'` を確認。破壊後の `hotreload.00810.png` が破壊前の `gallery.png` と同一 (5 クワッドとも元の色のまま) であることを画像で確認 — 旧い絵が維持されることを実証
    - 画像/ログの絶対パス (すべて `%TEMP%\claude\C--HAL-MyEngin\8864d946-863a-46b5-9312-6ffea53e1a0b\scratchpad\mye_m79_sub02_verify\` 配下、リポジトリ外・未コミット):
      `gallery.png`, `gallery.log`, `blend_check.png`, `failures.png`, `failures.log`,
      `hotreload.00090.png` 〜 `hotreload.00810.png` (9 枚), `hotreload.log`
  - 未実行: Release 構成のビルド/selftest、`tools\replay_verify.bat` (このサブの検証コマンドに含まれていないため)

自己採点 (1-5):
  仕様適合: 5 — round 1 の must 指摘 2 件をどちらも解消。spec §4.1 (PS 側 static の明文化) どおりに実装し、§4.2 の Tex2D 両受理も round 1 のまま据え置き (planner 確定済み)
  正しさ: 5 — PS 側 static バグは「壊すと落ちる・直すと通る」の両方を実地で確認 (反証テスト)。accept 1/2/3/5 は実プロジェクト・実シーン・Runtime.exe の本物の経路 (シーン読み込み→MeshRenderer→.mat.json→遅延Load→RenderSystem→ForwardPath) でスクショ確認済み。transparent キューも blend_check.png で明確に確認 (round 1 で未検証としていた透明経路もここで解消)
  コード品質: 4 — 修正は最小 (4 行 × 2 箇所) で規約どおり。回帰テストは既存の低レベル (SurfaceShaderSelfTest) と ForwardPath 経由 (SurfaceMaterialSelfTest) の両方に足した
  テスト: 5 — 単体 (ShaderManager 直接描画) と統合 (ForwardPath 実描画) の両方で「未代入なら FAIL する」ことを反証テストで確認済み。実プロジェクト E2E もスクショ + ログで実施

不安・質問: なし (round 1 の 2 件はどちらも本ラウンドで解消・確定済み)

触ったファイル (round 1 分を含む全量):
  - `src/Engine/Renderer/GpuResources.h`
  - `src/Engine/Renderer/GpuResources.cpp`
  - `src/Engine/Renderer/ProjectShaderProperties.h`
  - `src/Engine/Renderer/ProjectShaderProperties.cpp`
  - `src/Engine/Renderer/SurfaceProgram.h`
  - `src/Engine/Renderer/ShaderCache.h`
  - `src/Engine/Renderer/ShaderCache.cpp`
  - `src/Engine/Renderer/ShaderManager.h`
  - `src/Engine/Renderer/ShaderManager.cpp`
  - `src/Engine/Renderer/ForwardPath.h`
  - `src/Engine/Renderer/ForwardPath.cpp`
  - `src/Engine/Renderer/SurfaceDrawBind.h` (新規)
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.h` (新規)
  - `src/Engine/Renderer/SurfaceMaterialSelfTest.cpp` (新規)
  - `src/Engine/Renderer/SurfaceShaderSelfTest.cpp` (round 2 で追記)
  - `src/Editor/EditorMain.cpp`
  - `assets/shaders/MyEngineSurface.hlsli`
  - `assets/shaders/MyEngineSurfaceEntries.hlsli` (round 2 で修正、sub-01 由来のファイル)
  - `assets/shaders/ToonFlat.surface.hlsl` (新規)
  - `assets/shaders/ToonFlat.surface.hlsl.meta` (新規)
  - `build/Engine.vcxproj` (生成物)
  - `build/Engine.vcxproj.filters` (生成物)
申し送り:
  - round 1 の申し送り (環境メモ `pwsh` / `deepmodal_creatures.dmnet` 副作用 / ShaderCache v1→v2 / sub-03・sub-04 向けメモ) は継続して有効
  - 一時検証プロジェクト (`%TEMP%\claude\C--HAL-MyEngin\8864d946-863a-46b5-9312-6ffea53e1a0b\scratchpad\mye_m79_sub02_verify\`) はコミットしていない (指示どおりリポジトリ外)。再現したい場合は本ラウンドの「検証」節の手順 (`.mat.json` + 自作 `.meta` (固定 GUID) + `MeshRenderer.material` にその GUID の 10 進値、mesh は `builtin://quad` の FNV-1a ハッシュ `96799893311522987`) で再構築できる
  - sub-03 (Deferred/速度) は、修正後の `MyePSVelocity` (PS は常に「今」を見る) を前提にしてよい。round 1 で示唆した「速度エントリの PSMain 内で前側になる」問題はもう存在しない

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 2 件: (1) PS エントリが static を未代入 (`MyePSColor` / `MyePSVelocity`)。spec §4.1 に明文化、PS で今フレーム値を代入＋read-back テスト (2) 受け入れ条件 1/2/3/5 の実経路スクショ (Runtime.exe、透明込み) 未実施。Tex2D 符号化は両受理で確定 (spec §4.2)。
- round 2: VERDICT OK (planner)。PS エントリの static 代入を実コードで確認 (反証テストで壊すと落ちることも coder が実施)。Runtime.exe 実経路スクショ gallery.png / failures.png を planner が目視 (5 クワッド: 不透明・透明・forward_lit 混在・Properties 違い、失敗 4 種マゼンタ)。
