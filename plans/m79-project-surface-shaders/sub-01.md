# sub-01: サーフェス規約の成立確認: 索引・共通 include・生成エントリ・名前バインド表

- 依存: なし
- 状態: OK (コミット待ち)
- 往復: 1

## やること

spec §4.1「作者規約」「共通 include」「エンジン生成エントリ」と §4.2「予約 CB のレイアウト」を、**描画に繋ぐ前にシェーダ側だけで成立させる**。M79 で最もリスクの高い未知 3 つをここで潰す:

1. **static 代入による再評価**: include の `static float4x4 gViewProj; static float4x4 gWorld; static float gTime; static float gWaterTime;` を生成エントリが代入してから作者の `VSMain` を呼ぶ。速度エントリは前/今で 2 回呼ぶ。fxc がこれを正しくコンパイルし、2 回の評価が別の値を使うこと
2. **register なしの名前バインド**: 予約 CB (`MyEnginePerFrame` / `MyEngineSurfaceFrame` / `MyEnginePerObject` / `MyEngineWater`)、予約テクスチャ・サンプラ、作者の `MyEnginePerMaterial` と `Texture2D` のスロットを D3DReflect の名前で引ける表 (VS / PS 別) を作る。作者は register を書かない
3. **MeshVertex 固定オフセットの入力レイアウト**: 作者の `VSIn` が `POSITION` / `NORMAL` / `TEXCOORD0` の部分集合・順不同でも正しいオフセットになる (既存 `BuildInputLayout` の `APPEND_ALIGNED` は使わない)

具体的な成果物:
- `ShaderManager`: `*.surface.hlsl` を M78f の assets 全域索引に追加 (短名 `X.surface`)。サーフェス用のロード口 (例 `LoadSurface`) を足し、作者ファイル＋エンジン生成エントリ (色 VS/PS・速度 VS/PS・影 VS) をコンパイルして 1 つの「サーフェスプログラム」に束ねる。VS / PS のリフレクション結果 (名前→スロット、`MyEnginePerMaterial` の変数名→オフセット/サイズ) を保持する。規約違反でコンパイルが落ちたときはエラー文の先頭に「サーフェス規約: VSOut VSMain(VSIn) / float4 PSMain(VSOut) : SV_Target / VSOut.pos : SV_Position」を足す
- `assets/shaders/MyEngineSurface.hlsli` (＋ `.meta`): 予約 CB・予約テクスチャ/サンプラ・位置用 static・ヘルパ (`MyeSunShadow` / `MyeApplyFog` / 太陽の方向と色 / 環境光)。生成エントリの本体は別 include (例 `MyEngineSurfaceEntries.hlsli`) にしてもよい
- 予約 CB の C++ 構造体 (正本。Renderer 層、既存 `MeshBind.h` / `RenderTypes.h` 流儀)。`MyEnginePerFrame` を既存 PerFrameCB の使い回しにするなら、その構造体を ForwardPath / DeferredPath の匿名名前空間から共有位置へ出すかどうかは coder 判断 (出すならこのサブで)
- `gTime` の定義 = `viewFrameIndex / 60` [秒]、前時刻 = `(viewFrameIndex - 1) / 60` (spec §2 時計の行)。値を詰める関数はこのサブで作ってよいが、描画パスへの配線は sub-02 以降
- 組込みエラーシェーダ `assets/shaders/surface_error.hlsl` (＋ `.meta`): 変位なし・マゼンタ (1,0,1)。サーフェスと同じ予約 CB で描ける形にする (sub-02 / sub-03 がそのまま使う)
- SelfTest: フィクスチャ HLSL (規約どおり・VSIn 部分集合・register なし・Properties あり) をコンパイルし、(a) 3 種の生成エントリが揃う (b) 予約資源と作者資源が名前で引ける (c) 規約違反フィクスチャが「サーフェス規約」付きで失敗する (d) 入力レイアウトのオフセットが MeshVertex と一致する、を検査。予約 CB の HLSL 変数オフセットを C++ の `offsetof` と照合する

## やらないこと (このサブでは)

- Forward / Deferred / Shadow パスでの実描画 (sub-02 / sub-03)
- Material の遅延 Load・`.mat.json` の `properties` (sub-02)
- Inspector・作成メニュー (sub-04)
- WaterWave 側の配線 (sub-05。`MyEngineWater` の CB 定義と include 宣言だけはここで入れてよい)

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/ShaderManager.h/.cpp` — `IsProjectIndexedShaderFile` (`ShaderManager.cpp:130-136`) に `.surface.hlsl`、サーフェス用ロード・生成エントリ付きコンパイル・リフレクション保持。`ShaderProgram` (`ShaderManager.h:20-32`) に VS/PS バイトコードかバインド表を足すか、サーフェス専用の構造体を別に持つかは coder 判断。バイトコードキャッシュ (`TryLoadCached` / `Instantiate`) との整合 (生成エントリを含むソースでキー化) に注意
- 入力レイアウト: `BuildInputLayout` (`ShaderManager.cpp:83-128`) とは別に MeshVertex (`GpuResources.h:30-39`) の固定オフセット版
- リフレクションの前例: `ComputeAbiRunner.cpp:79-134`
- 速度の式: `assets/shaders/common.hlsli` の `ComputeVelocityUv`、`DeferredPath.cpp:156-165` の VelocityCB
- `src/Engine/Renderer/ShaderManagerProjectIndexSelfTest.cpp` — surface ケース追加
- 新規 SelfTest (例 `SurfaceShaderSelfTest.cpp/.h`) と `EditorMain.cpp` の登録 (M78 と同じ `ok &=` 集約・末尾 append)
- ファイル追加後 `tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)

1. `X.surface.hlsl` が短名 `X.surface` で索引され、同名 2 本は両方無効＋ERROR — `--selftest`
2. 規約どおりのフィクスチャで色・速度・影の生成エントリがコンパイルでき、予約資源と作者資源 (`MyEnginePerMaterial` の各変数オフセット、作者 `Texture2D`) が名前で解決される。速度エントリが前/今の 2 回評価になっていること (生成ソースまたはリフレクションで確認できる形で) — `--selftest`
3. 規約違反フィクスチャ (VSMain の型名違い、`pos` 欠落) が「サーフェス規約」付きエラーで失敗し、落ちない — `--selftest`
4. 入力レイアウトが VSIn の部分集合・順不同で MeshVertex のオフセットと一致 — `--selftest`
5. 予約 CB の HLSL レイアウトが C++ 構造体と一致 (変数ごと) — `--selftest`
6. 既存の描画・既存 SelfTest・check_rules に変化なし (サーフェス経路は未配線) — `--selftest`、`tools\check_rules.ps1`

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 ビルド）
bin\x64\Debug\Editor.exe --selftest
tools\check_rules.ps1
```

成立しなかった未知 (特に 1. static 代入) があれば、無理に回避策を積まずに SELF_EVAL の「不安・質問」で planner へ返すこと (方式の切り替え判断になる)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1)
実装:
  - `src/Engine/Renderer/SurfaceProgram.h` — `SurfaceProgram`/`SurfaceEntryReflection`/`SurfaceReflectedResource`/`SurfaceReflectedVar` を新設。色/速度/影の 3 系統 5 エントリ (VS/PS) のバイトコード・入力レイアウト・リフレクション表・エラー文を保持
  - `src/Engine/Renderer/SurfaceShaderTypes.h` — 予約 CB の C++ 正本 (`MyEnginePerFrameCB`/`MyEngineSurfaceFrameCB`/`MyEnginePerObjectCB`/`MyEngineWaterCB`)。`GpuLight`/`ShadowTileCB`/`FroxelForwardCB`/`AcousticCB`/`kMaxLights`/`kMaxShadowTiles` (RenderTypes.h) と `GerstnerWave` (WaveMath.h) を再利用
  - `assets/shaders/MyEngineSurface.hlsli`(+.meta) — 位置に効く `static` (`gViewProj`/`gWorld`/`gTime`/`gWaterTime`)、予約 CB 4 本、予約テクスチャ/サンプラ、ヘルパ (`MyeSunShadow`/`MyeApplyFog`)
  - `assets/shaders/MyEngineSurfaceEntries.hlsli`(+.meta) — 生成エントリ本体 (`MyeVSColor`/`MyePSColor`/`MyeVSShadow`/`MyeVSVelocity`/`MyePSVelocity`)。速度は `VSMain` を前後 2 回評価し、`SV_Position` が PS ではピクセル座標に化ける問題を避けるため `myeCurClip`/`myePrevClip` を別セマンティクスで複製 (`deferred_gbuffer.hlsl` の既存手口を踏襲)
  - `assets/shaders/surface_error.hlsl`(+.meta) — 組込みマゼンタ・変位なし。他の `*.surface.hlsl` と同じ `LoadSurface` 経路でコンパイルできる形
  - `src/Engine/Renderer/ShaderManager.h/.cpp` — `IsProjectIndexedShaderFile` に `.surface.hlsl` を追加 (索引・短名解決)。`LoadSurface`/`GetSurface`/`CompileSurfaceProgram` を新設。作者ソース + `MyEngineSurfaceEntries.hlsli` を 1 つの翻訳単位として 5 エントリを個別 `D3DCompile`。失敗時は「サーフェス規約: ...」を先頭に付ける。`BuildSurfaceInputLayout` (MeshVertex 固定オフセット、`APPEND_ALIGNED` 不使用) と `ReflectSurfaceBytecode` (D3DReflect、`D3D_SVF_USED` で絞らない全列挙) を追加
  - `src/Engine/Renderer/ShaderManagerProjectIndexSelfTest.cpp` — `*.surface.hlsl` の一意/重複ケースを追加
  - `src/Engine/Renderer/SurfaceShaderSelfTest.{h,cpp}` (新規、`EditorMain.cpp` 末尾 append で登録) — 規約どおり/規約違反の 2 フィクスチャのコンパイル・リフレクション名前解決・予約 CB オフセット照合に加え、**実 WARP デバイスで実描画** して検証: (a) `VSIn` を `TEXCOORD0` 先・`POSITION` 後・`NORMAL` 省略の部分集合・順不同にし、`MeshVertex` 固定オフセットで正しく頂点を読めることを velocity の read-back 値で確認、(b) 速度エントリが `gWorld` の前後差・`gTime` の前後差をそれぞれ独立に正しく速度へ反映することを 2 回の Draw + 1x1 ターゲット read-back で数値照合 (期待値を手計算し `Check` で突合)
  - `tools\gen_project_files.ps1` を実行 (`build/Engine.vcxproj`・`.filters` に新規ファイルを反映)

仕様との差分:
  - [逸脱] `MyEnginePerFrame` は既存 `forward_lit.hlsl` の `PerFrame` 先頭の `gViewProj` (フレームの camera VP) を持たない。理由: 同名の予約 `static gViewProj` (位置に効く値) と衝突するため。spec は「既存 PerFrame バッファを使い回してよい」としているが、名前衝突のため GPU バッファそのものの共有はできず、「内容 (RenderView から詰める式) の共有」に解釈を落とした。それ以外のフィールドは既存と同順・同型。C++ 側 `MyEnginePerFrameCB` も同様に先頭を持たない (offsetof は前提が変わるだけで整合)
  - [簡略化] `MyeApplyFog` は解析フォグ (`ApplyFog`) のみを呼び、`deferred_light.hlsl`/`forward_lit.hlsl` にあるフロクセル合成分岐は実装していない。`MyEnginePerFrame` の該当フィールドはレイアウト上確保済み (バイト互換) だが v1 のヘルパからは未使用。spec §4.1 のヘルパ一覧が「太陽方向色・MyeSunShadow・MyeApplyFog・環境光」の 4 点のみを明示しており、フロクセル/局所影アトラス/音響の合成は §3 の「後回し」項目 (SSAO/SSR/デカール/RT受光) と同じ精神と判断したが、明示はされていないため差分として記録
  - [未実装] `ShaderManager::RequestRecompileForFile`/`PollAsyncCompiles` (ホットリロード) と `TryLoadCached`/`Instantiate` (バイトコードキャッシュ) は `surfacePrograms_`/`SurfaceProgram` を対象にしていない。`LoadSurface` は毎回フルコンパイルする。sub-01 の受け入れ条件 1〜6 はこの配線を要求しておらず、方式の成立確認 (static 代入・名前バインド・固定オフセット入力レイアウト) を優先したため後回しにした
  - [発見・未修正] 既存 `IsProjectIndexedShaderFile` の `.cs.hlsl` 判定に off-by-one を発見した。`filename.compare(filename.size()-9, 9, L".cs.hlsl")` は 9 文字を切り出して 8 文字の `L".cs.hlsl"` と比較しており、実在の `.cs.hlsl` ファイルでも一致しない (単体で `L"abc.cs.hlsl".compare(...)` を再現し `r=1` を確認、`.post.hlsl` 側の正しい書き方 `count==リテラル長` では `r=0`)。つまり `*.cs.hlsl` プロジェクトシェーダは現状、短名索引に一切載っていない可能性が高い。私の `.surface.hlsl` 分岐は `count` をリテラル長 (13) に合わせて書いており、自己テストで実際に一致を確認済み。M79 と無関係の既存コード (M78d/e) なので修正はせず報告のみ

検証:
  - `powershell -File tools\gen_project_files.ps1` → 成功 (Engine/Editor/Runtime/GameLogic の 4 vcxproj 更新)
  - MSBuild (`MyEngine.sln`, Debug|x64) → 成功、警告 0・エラー 0 (ログで `error` 検索 0 件、exit code 0)
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (exit code 0)。ログ全体で `FAIL` 0 件。新設 `M79 surface shader self test` は 30 件超のチェックすべて PASS (索引・規約違反・リフレクション名前解決・予約 CB オフセット照合・実描画での入力レイアウト/2 回評価の数値一致を含む)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - Release 構成のビルド/selftest は未実行 (sub-01 の検証コマンドに含まれていないため)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜6 は全て満たすが、「仕様との差分」の 3 点 (gViewProj 逸脱・フロクセル簡略化・ホットリロード/キャッシュ未配線) を disclose している
  正しさ: 5 — 実 WARP デバイスでの実描画 + read-back により、静的再代入 (risk 1)・名前バインド (risk 2)・MeshVertex 固定オフセット入力レイアウト (risk 3) の 3 つの最大リスクをすべて数値で実証した。既存 SelfTest 全件・check_rules も無変化で通過
  コード品質: 4 — 既存流儀 (ComputeAbiRunner のリフレクション手法、既存 CB 構造) を踏襲。`ReflectSurfaceBytecode` は `ComputeAbiRunner::ReflectBytecode` と処理が近く重複がある (意図的に分離。理由はコード内コメント参照)
  テスト: 5 — 索引・規約違反・リフレクション・入力レイアウト・2 回評価のすべてに機械的検証を追加し、実行して確認済み

不安・質問:
  1. `MyEnginePerFrame` から先頭の `gViewProj` を落とした解釈 (仕様との差分 [逸脱] 参照) でよいか。逆に「バイト単位で既存 PerFrameCB と完全一致させたい」なら、位置用 static を `gMyeViewProj` のような別名にして `gViewProj`(CB) と衝突を避ける設計に直す必要がある (sub-02 着手前に決めたい)
  2. ホットリロード (`RequestRecompileForFile`/`PollAsyncCompiles`) とバイトコードキャッシュを `SurfaceProgram` へ配線するのは sub-01 に戻すべきか、sub-02 以降でよいか。spec §4.1 の「保存するとホットリロード」は M79 全体の要求だが、sub-01 の受け入れ条件には含まれていない
触ったファイル:
  - `src/Engine/Renderer/ShaderManager.h`
  - `src/Engine/Renderer/ShaderManager.cpp`
  - `src/Engine/Renderer/ShaderManagerProjectIndexSelfTest.cpp`
  - `src/Editor/EditorMain.cpp`
  - `src/Engine/Renderer/SurfaceProgram.h` (新規)
  - `src/Engine/Renderer/SurfaceShaderTypes.h` (新規)
  - `src/Engine/Renderer/SurfaceShaderSelfTest.h` (新規)
  - `src/Engine/Renderer/SurfaceShaderSelfTest.cpp` (新規)
  - `assets/shaders/MyEngineSurface.hlsli` (新規)
  - `assets/shaders/MyEngineSurface.hlsli.meta` (新規)
  - `assets/shaders/MyEngineSurfaceEntries.hlsli` (新規)
  - `assets/shaders/MyEngineSurfaceEntries.hlsli.meta` (新規)
  - `assets/shaders/surface_error.hlsl` (新規)
  - `assets/shaders/surface_error.hlsl.meta` (新規)
申し送り:
  - `tools\gen_project_files.ps1` を実行済み。`build/Engine.vcxproj`・`build/Engine.vcxproj.filters` が新規ファイルを反映して変化している (生成物なので上の「触ったファイル」には含めていないが、コミット対象に入れないとビルドできない)
  - 発見した `.cs.hlsl` 索引の off-by-one (上記) は M79 と無関係の既存不具合。修正はせず記録のみ (影響: プロジェクト側 `*.cs.hlsl` の短名索引が機能していない可能性)
  - `MyEngineWater` の CB フィールド (waves[4]・baseHeight・overallScale・deepColor・shallowColor) は sub-01 の裁量で仮決めした形。sub-05 で内容を再確認すること
  - `MyeApplyFog`/`MyeSunShadow` 以外のヘルパ (IBL・局所影アトラス・音響・フロクセル合成) は v1 で未実装。CB のフィールドはバイト互換で確保済みなので、後続サブで足すときは field 位置を変えないこと

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1-6 を実 WARP 描画の数値照合込みで確認。質問 1 = viewProj を除く解釈で確定 (spec §4.1)、質問 2 = ホットリロード / キャッシュは sub-02 へ。`.cs.hlsl` の off-by-one は sub-04 へ。
