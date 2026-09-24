# M79 プロジェクト側サーフェスシェーダー — 仕様書

- slug: m79-project-surface-shaders
- 状態: 確定 (2026-09-24。§7 の `[ユーザーに聞ける]` 5 件はユーザーが全件 planner 裁定どおりで確定)
- 依頼原文: M79: プロジェクト側サーフェスシェーダー。M78 (プロジェクト側ポスト/コンピュート, plans/m78-project-shaders/) と同じ作法で、プロジェクト assets に `*.surface.hlsl` (生 HLSL, VSMain/PSMain を作者が書く) を置き、先頭 `/*@MyEngineProperties ... @*/` で Inspector にマテリアル単位のパラメータが出る。`.mat.json` の `shader` で参照 (Inspector で選択可)。エンジンは PerFrame(カメラ/光/影/霧)・PerObject を名前付き CB＋共通 include で供給し作者は register を書かない。失敗時はマゼンタ＋エラー表示。バリアントなし。初版はフォワードのみ (ディファード後回し)、ABI 追加なし想定。論点: 頂点変位を深度/影/速度(TAA)パスへどう反映するか (VSMain 使い回し vs 任意の VSShadow 等の追加エントリ)。動機: Water プロジェクト (C:\Users\akita\Documents\MyEngineProjects\Water) の main シーンで浮世絵風の動画を作るため (トゥーン/平塗りライティング、Gerstner 頂点変位の水面、作り直す大波の巻き込みアニメ)。既存参考: plans/m78-project-shaders/reference-unity-ue.md §1 サーフェス、Water の assets/shaders/water_surface.hlsl・ukiyoe_flat.hlsl (現状 cbuffer 40 行を手写し・未使用)。

## 1. 目的 (なぜ作るか)

作者 (Water プロジェクトで浮世絵風の動画を撮る本人) が、エンジンを再ビルドせずに **メッシュ 1 つ 1 つの見た目 (ライティングの式・頂点の動き) をプロジェクト資産の HLSL で差し替え**、しかもそれが **TAA 付きの動画で破綻しない** 状態にする。

達成したい状態:
- `*.surface.hlsl` に `VSMain` / `PSMain` を書き、先頭の Properties だけでマテリアル Inspector にパラメータが生える。PerFrame (カメラ・光・影・霧) / PerObject / 時間は **共通 include 1 行**で使え、cbuffer の手写し (現 Water の 40 行) が消える
- `.mat.json` の `shader` をそのシェーダにすれば、そのマテリアルのメッシュが作者の式で描かれる (Inspector で選べる)
- **頂点変位 (Gerstner 水面・大波の巻き込み) が、影・深度・速度 (TAA) に作者の追加作業なしで反映される** → 動画でゴースト・影ずれが出ない
- 失敗したら黙って消えず、マゼンタで描かれエラーが見える
- サーフェスシェーダを使わないシーンの絵・決定論・ABI は 1 ビットも変わらない

## 2. 疑った点と結論

| 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|
| **「初版はフォワードのみ」と「動画で TAA」が両立するか** | TAA は **Deferred パスのみ**: `TaaPass.h:20-22` 成立条件② `velocitySRV != nullptr = Deferred パスのみ (v1 制限)`。velocity RT は `DeferredPath.cpp:705,760-771` の GBuffer RT4 だけ。Forward (`ForwardPath.cpp`) には velocity も深度プリパスも無い。レンダパス既定は Forward (`EngineLoop.cpp:187`)、Deferred は `--deferred` / メニューの**セッション切替** | 聞けず (planner 裁定) `[ユーザーに聞ける]` #1 | 「フォワード」を **シェーディング方式 (作者の PS が最終色を出す。GBuffer に書かない)** と解釈し、**Forward パスと Deferred パスの両方で描く**。Deferred では光パス後に「フォワード段」を設けて HDR＋velocity＋深度へ書く (HDRP の forward-only マテリアルと同じ)。GBuffer へ書く作者規約は後回しのまま。却下案: (a) Forward パスのみ = TAA が使えず動機を満たさない (b) Forward パスへ velocity＋TAA を追加 = forward_lit / instanced / skinned / terrain 全系に速度出力が要り規模が 1 マイル分増える |
| **頂点変位を影・深度・速度へどう反映するか** (依頼の論点) | 影は全アイテム共通の `shadow_depth` で描く (`ShadowPass.cpp:39-41`、マテリアル無関係)。Deferred の velocity は `deferred_gbuffer*.hlsl` の VS が `prevWorld`/`prevViewProj` で出す (`DeferredPath.cpp:840-857`)。UE (WPO の Output Velocity) と HDRP (vertex animation の motion vector) は**頂点処理を前フレームの時刻で再評価**して速度を出す。Unity URP は作者が MotionVectors パスを書く | 聞けず (planner 裁定) `[ユーザーに聞ける]` #2 | **VSMain 使い回し (再評価方式)**。エンジンが作者の `VSMain` を包むエントリを自動生成する: 色 = 1 回評価、**速度 = 前フレームの行列・時刻で 1 回＋今フレームで 1 回の 2 回評価**して差を velocity に書く、**影 = ライトの ViewProj で評価し PS なし**。深度は色の描画そのもので書く (プリパスは足さない)。作者の追加エントリ (`VSShadow` / `PSVelocity` 等) は初版で作らない。却下案: 任意の追加エントリ (Unity URP 式) = 変位の式を 2〜3 か所に書くことになり、書き忘れ・ずれが「TAA のゴースト / 影ずれ」として静かに出る。動機 (動画) で最も痛い壊れ方なので採らない |
| 再評価方式が生 HLSL で成立するか | HLSL はエントリ関数を普通の関数として呼べる。cbuffer は差し替えられないので、位置に効く値 (`gViewProj` / `gWorld` / `gTime`) を **include 側の `static` グローバル**にし、生成エントリが評価の前に代入する | — | **成立の可否は sub-01 で最初に潰す** (荷重のかかる決定)。代償として作者規約が 3 つ付く (§4.1 規約): `VSOut VSMain(VSIn)` / `float4 PSMain(VSOut) : SV_Target` / `VSOut` に `float4 pos : SV_Position`。Water の既存 2 本は既にこの形 |
| `water_surface.hlsl` は本当に「未使用」か | 依頼文の前提と違う。Water の `assets/shaders/water_surface.hlsl` はエンジン組込み `water_surface` を**プロジェクト優先ルートで上書き**しており、`WaterPass.cpp:19` の `shaders.Load("water_surface")` で実際に使われている (`ShaderManager.h:36-41`、`ReportShadowedBuiltins` が WARN を出す)。未使用なのは `ukiyoe_flat.hlsl` / `breaking_wave.hlsl` だけ (マテリアルは全部 `forward_lit`) | — | 前提を訂正して記録。**今の水面の絵は組込み WaterPass が描いている = 速度を書かず影も落とさない** (`WaterPass.cpp` に velocity RT のバインドなし)。→ 下の行へ |
| WaterWave の水面を差し替える手段が要るか (司会補足の論点) | 水面は `WaterWaveComponent` (`Components.h:1650`) を `RenderSystem.cpp:941-1001` が別レーンで拾い WaterPass が専用メッシュ `WaterPlane()` で描く。MeshRenderer ではないので `.mat.json` 経路に乗らない。浮力 (CPU Gerstner) は WaterWave の値を使う | 聞けず (planner 裁定) `[ユーザーに聞ける]` #3 | **やる (最後のサブ sub-05)**。WaterWave に描画専用の `surfaceMaterial` (AssetRef、末尾追加、ハッシュ非関与) を足し、設定時は水面をサーフェス経路 (速度・影込み) で描く。波パラメータは予約 CB `MyEngineWater` で渡す = 浮力と同じ値が単一の出所。未設定 = 従来の WaterPass と完全同一。却下案: MeshRenderer で平面を別に置き WaterWave の描画を切る = 波パラメータをマテリアルに二重記入することになり浮力とずれる |
| シェーダの時計を何にするか | 水面は `viewFrameIndex * (1/60) * timeScale` (`RenderSystem.cpp:974`)。これは viewKey 別の描画通番で、決定的撮影 (frame == tick) で再現する。tick 時間 (`EngineLoop.h:434 tickIndex`) は Edit モードで進まない | 聞けず (planner 裁定) `[ユーザーに聞ける]` #4 | **`gTime` = `viewFrameIndex / 60` [秒]** (水面と同じ時計、timeScale なし)。速度用の前時刻は `(viewFrameIndex-1)/60`。描画専用でシミュレーション状態へは入れない (AGENTS 4.2 の「決定性が必要な状態更新へ持ち込まない」を満たす)。代償: 撮影モード外では描画 FPS に比例して速く見える (既存の水面と同じ性質) |
| Material に値を足せるか | `Material` は cooked キャッシュへ memcpy され `CookedCacheSelfTest` が `sizeof(Material)` 全体を比べる (`GpuResources.h:226-232`) | — | **`Material` POD は変えない**。サーフェス用の情報 (シェーダ名・Properties 値・パック済み CB・テクスチャ) は `MaterialLibrary` 側の横テーブルに持つ |
| `.mat.json` の `shader` を変えれば今でも効くか | `ParseMaterialJson` は名前をハッシュするだけで Load しない (`GpuResources.cpp:1048`)。Forward は `shaders.Get` が null なら **黙ってスキップ** (`ForwardPath.cpp:441-445`)。Deferred 不透明は `material.shader` を**無視**して全部 GBuffer (`DeferredPath.cpp:877-970`)。透明段だけは `mat->shader` を使う (`DeferredPath.cpp:1398`) | — | Material の**遅延 Load** と「見つからない / 失敗 = マゼンタ」を足す (M78 で後回しにした分) |
| 作者は register を書かないで済むか | M78 の実例は作者が `register(b1)` 等を書いている (`MyTint.post.hlsl`、Water `TestCop.cs.hlsl`)。D3DReflect による名前→スロットは `ComputeAbiRunner.cpp:79-134` に前例。入力レイアウトは VS リフレクションから `APPEND_ALIGNED` で作る (`ShaderManager.cpp:83-128`) ので、作者の `VSIn` が `NORMAL` を省くとオフセットがずれる | — | **全バインドを D3DReflect の名前で解決** (予約 CB・予約テクスチャ・作者の `MyEnginePerMaterial`・作者の `Texture2D`)。include の予約資源には明示 register を付けてよいが作者には要求しない。入力レイアウトは **MeshVertex の固定オフセットを semantic 名で引く** (作者の VSIn は部分集合・順不同でよい) |
| PerMaterial CB のレイアウトを M78 と同じ「パース順」にするか | M78 は Properties の並び順でパックし作者の cbuffer と順序一致を要求 (M78 spec §4.2)。作者が並びを間違えると値が静かにずれる | 聞けず (planner 裁定) `[ユーザーに聞ける]` #5 | **リフレクションのオフセットで詰める** (名前一致)。Properties にあって cbuffer に無い名前は WARN、型の大きさ不一致は WARN＋その項目を書かない。M78 のポスト/コンピュートの規則は変えない。却下案: M78 と同じパース順 = 手書き cbuffer と並びがずれた瞬間に壊れる。Properties から cbuffer を自動生成 = M78 の「作者が cbuffer を書く」作法から外れる |
| Inspector の Properties UI を再利用できるか | fxstack 用 UI は `InspectorWindow.cpp:2510-` にベタ書き。スキーマ取得が **`ShaderDirs() + name + ".hlsl"`** で M78f の assets 全域索引 (`ShaderManager::ResolveShaderPath`) を見ていない (`InspectorWindow.cpp:2523-2531`) = `assets/shaders` 以外に置いたシェーダは Properties が出ない | — | Properties ウィジェット描画とスキーマ取得を共通関数に切り出し、fxstack とマテリアルの両方から使う。取得は `ResolveShaderPath` 経由に直す (M78 の潜在バグも同時に解消。スコープ追加として明示) |
| 骨入り (スキン) / インスタンシングはどうするか | スキンはマテリアルのシェーダを無視して `forward_skinned` / GBuffer スキン版へ差し替える (`ForwardPath.cpp:440-441`)。インスタンシングは Forward は forward_lit 限定 (`ForwardPath.cpp:323`)、Deferred と Shadow は (material,mesh) 連続 run でシェーダ無関係 | — | 初版: **スキン＋サーフェスは従来のスキン経路で描き WARN 1 回** (後回し)。**サーフェスのアイテムはインスタンス run から外す** (Deferred GBuffer・Shadow とも) |
| 局所ライトの影 (シャドウアトラス) も変位させるか | 太陽 CSM は `ShadowPass`、スポット/ポイントは `ShadowAtlas` (`ShadowAtlas.h:61`) と別実装。Water の main シーンの光は Sun 1 本 | — | 初版は **CSM だけ変位込み**。アトラスは従来シェーダ (変位なしの剛体形) で描く = 後回しとして文書化 |
| ABI 追加が要るか | 実行時に作者が触るのはアセット (`.mat.json` / HLSL) だけ。スクリプトからの `SetMaterialFloat` 等は依頼に無い | — | **ABI 変更なし** (`MYE_API_VERSION` 21 のまま)。スクリプトからのマテリアル値操作は後回し |
| バリアントは本当に作らないか | 生成するエントリ (色 / 速度 / 影) はエンジン所有のパスの別で、作者のキーワードではない | — | 作者向けキーワード・`multi_compile` 相当は作らない (依頼どおり)。パス別の生成エントリは「バリアント」に数えない |
| dirty ツリー | 台帳の申し送り: M79 無関係の未コミット変更が残る | — | 触らない。コミットはファイル名明示 (`git add -A` 禁止) |

**押し切られた点**: なし (ユーザーとの往復なし)。台帳の既存ユーザー判断 (作者形式 = M78 と同じ生 HLSL＋Properties＋名前バインド、「表面関数だけ書く」抽象化の却下) は前提として採用済みで、本仕様の再評価エントリは**作者の VSMain / PSMain をそのまま呼ぶ包み**であり抽象化の再提案ではない。

## 3. スコープ

### やる
- `*.surface.hlsl` の assets 全域索引 (M78f の索引に第 3 の接尾辞として追加。短名は `Foo.surface`)
- 共通 include `assets/shaders/MyEngineSurface.hlsli` (予約 CB・予約テクスチャ/サンプラ・位置用 static・光/影/霧のヘルパ)
- エンジン生成エントリ (色 / 速度 / 影) によるコンパイル、D3DReflect 名前バインド、MeshVertex 固定オフセットの入力レイアウト
- Material のシェーダ遅延 Load、横テーブル (シェーダ名・Properties 値・パック CB・テクスチャ)、`.mat.json` の `properties`
- Forward パス: 不透明・透明のサーフェス描画
- Deferred パス: GBuffer から除外し、光パス後のフォワード段で HDR＋velocity＋深度へ描画 (不透明)。透明は既存の透明段で描く
- 太陽 CSM への変位込み影
- 失敗時マゼンタ (組込みエラーシェーダ) ＋ Console エラー ＋ マテリアル Inspector のエラーバナー。ホットリロード失敗は旧プログラム維持
- マテリアル Inspector: シェーダ選択 (forward_lit ＋ 索引済み `*.surface`)、Properties 自動 UI (fxstack と共通化)、スキーマ取得を索引経由に修正
- Asset Browser 作成メニューに「サーフェスシェーダ」テンプレート
- M78 の既存不具合の修正: `*.cs.hlsl` 判定の off-by-one (`ShaderManager.cpp` の `IsProjectIndexedShaderFile` と `AssetOps.cpp:466`。8 文字の `.cs.hlsl` を 9 文字で比較しており一致しない = assets 全域索引にコンピュートが載らない)。sub-04 で直す
- WaterWave の `surfaceMaterial` と予約 CB `MyEngineWater`
- SelfTest (索引・生成エントリのコンパイル・予約 CB のレイアウト照合・マテリアル Properties パック・既定シーン不変)

- (review-1 #3 で追加) `.mat.json` の `boundsPadding` (変位で AABB の外へ出る形を視錐台カリング・CSM フィットで包む余白)
- (review-1 #7 で追加) `.mat.json` の `doubleSided` (両面描画。Cull None)
- (review-1 #1 #2 で明文化) サーフェスとサーフェスでないアイテムが混在しても、後続の非サーフェス描画 (forward_lit・instanced・影の深度・失敗時フォールバック) の固定スロット前提を壊さない

### やらない (明示)
- GBuffer に書く作者規約 (Deferred の照明を作者シェーダで受ける)
- 作者向けキーワード / バリアント
- 作者の追加エントリ (`VSShadow` / `PSVelocity` / `PSDepth` 等)
- スクリプト (GameLogic / C#) からのマテリアル値操作・ABI 変更
- Forward パスへの velocity / TAA 追加
- テッセレーション / ジオメトリシェーダ / 作者指定のレンダーステート (Cull / Blend / ZWrite の宣言)。ステートは `transparent` フラグで決まる既存どおり
- Water プロジェクト側の絵作り (大波の作り直し・ポストスタック・動画書き出し・既存 3 本の移植)。台帳申し送りどおり M79 完了後の別作業

### 後回し
- Deferred の GBuffer 作者規約、SSAO / SSR / デカール / RT 受光をサーフェスに効かせること
- スキンメッシュ＋サーフェス、インスタンシング＋サーフェス
- シャドウアトラス (スポット/ポイント) の変位込み影、アルファクリップ影
- 作者宣言のレンダーステート (HLSL 側の Cull / Blend / ZWrite 宣言)。両面だけは `.mat.json` の `doubleSided` で初版から扱う (review-1 #7)
- 両面描画時の裏面判定 (`SV_IsFrontFace`) を作者へ渡すこと (規約のシグネチャ `float4 PSMain(VSOut)` では受け取れない。裏面は法線と視線の内積で作者が判定する)
- レンダパス (Forward/Deferred) のプロジェクト設定への保存 (現在はセッション切替 / `--deferred`)
- スクリプトからのマテリアル値 ABI

## 4. 仕様

### 4.1 振る舞い

#### 作者ワークフロー
1. `<project>/assets` 配下の任意フォルダに `Foo.surface.hlsl` を置く (推奨 `assets/shaders/`)。M78f の索引が短名 `Foo.surface` で解決する。同名 2 本は両方無効＋ERROR (M78f と同じ規則)
2. 先頭に Properties ブロック (M78 と同じ DSL・同じ型と属性)、次に `#include "MyEngineSurface.hlsli"`、`MyEnginePerMaterial` cbuffer (register なし)、`Texture2D` (register なし)、`VSIn` / `VSOut` / `VSMain` / `PSMain`
3. `.mat.json` の `shader` を `"Foo.surface"` にする (Inspector のシェーダ選択でも可)。Properties の値は同じ `.mat.json` の `"properties"` に入る
4. MeshRenderer がそのマテリアルを参照すると、Forward / Deferred どちらのパスでも作者の式で描かれる。Deferred＋TAA では変位込みの速度が書かれる
5. HLSL を保存するとホットリロード。失敗時は旧プログラムを維持して WARN (初回から失敗ならマゼンタ)

#### 作者規約 (初版)
| 規約 | 内容 | 破ったとき |
|---|---|---|
| シグネチャ | `VSOut VSMain(VSIn v)` / `float4 PSMain(VSOut i) : SV_Target`。`VSOut` は `float4 pos : SV_Position` を持つ。型名 `VSIn` / `VSOut` とメンバ名 `pos` は固定 | 生成エントリのコンパイルエラー → マゼンタ。エラー文の先頭にエンジンが「サーフェス規約: ...」の一行を足す |
| 頂点入力 | `VSIn` は `POSITION` / `NORMAL` / `TEXCOORD0` の任意の部分集合・任意の順 (MeshVertex の固定オフセットを semantic で引く) | 未知 semantic はコンパイル後の入力レイアウト作成失敗 → マゼンタ＋エラー |
| 位置に効く値 | クリップ座標・ワールド位置の計算には **`gViewProj` / `gWorld` / `gTime`** (include の static) を使う。これらは生成エントリがパスごとに代入する (色 = 今フレーム、速度 = 前と今、影 = ライト VP) | 予約 cbuffer の生フィールドを直接読むと、速度 / 影が変位を反映しない (コンパイルは通る。docs とテンプレートのコメントで明示) |
| register | 書かなくてよい。予約名と衝突する宣言は不可 | コンパイルエラー → マゼンタ |
| 予約名 | 接頭辞 `MyEngine` の cbuffer 名、`gMye` / `_mye` 接頭辞の識別子はエンジン予約 | 同上 |

#### 共通 include `MyEngineSurface.hlsli` が供給するもの
- **位置用 static**: `float4x4 gViewProj; float4x4 gWorld; float gTime; float gWaterTime;` (生成エントリが代入。`gWaterTime` は `MyEngineWater` の水面時刻 = WaterWave の timeScale 込みで、速度エントリでは前/今で差し替わる。水面が無効なフレームは 0)
- **予約 CB** (名前でバインド。中身の正本は C++ の構造体、HLSL 側は include。レイアウト一致は SelfTest がリフレクションで照合):
  - `MyEnginePerFrame`: 既存 Forward/Deferred の PerFrame と同じ内容 (カメラ位置・環境光・ライト配列・CSM・霧・IBL・シャドウアトラス・フロクセル・音響)。**ただし先頭の `viewProj` は持たない** (位置用 static `gViewProj` と名前が衝突するため。カメラ VP は `MyEngineSurfaceFrame` の今 VP と、色エントリで代入される `gViewProj` で読む)。GPU バッファは既存 PerFrame と別でよく、共有するのは RenderView から詰める式 (sub-01 VERDICT round 1 で確定)
  - `MyEngineSurfaceFrame`: 今/前の ViewProj (前は非ジッタ)、今/前の時刻、ジッタ NDC、画面サイズ、前履歴の有効フラグ
  - `MyEnginePerObject`: 今/前の World、`gBaseColor` (マテリアル baseColor のリニア値)
  - `MyEngineWater`: 水面が有効なとき WaterWave の波パラメータ・色・時刻 (sub-05)。無効なら全 0＋有効フラグ 0
- **予約テクスチャ / サンプラ**: CSM シャドウマップと比較サンプラ、IBL 3 点、フロクセル、線形 Wrap / Clamp サンプラ (Forward の既存スロット構成を名前で引けるようにする)
- **ヘルパ**: 太陽の方向・色、`MyeSunShadow(posW)` (CSM の減衰 0..1)、`MyeApplyFog(color, posW)`、環境光。既存 `common.hlsli` の関数を包む

#### エンジン生成エントリ (作者には見えない)
- **PS 側の static**: VS と PS は別プログラムなので、VS エントリで代入した static は PS に届かない (PS では未代入 = 0)。**全 PS エントリ (色・速度) は `PSMain` を呼ぶ前に 4 つの static へ「今フレーム」の値を代入する**。PS から見える `gViewProj` / `gWorld` / `gTime` / `gWaterTime` は常に今フレーム (速度エントリでも前の値は PS に出ない) (sub-02 VERDICT round 1 で明文化)
- 色: `gViewProj` = 今フレーム (ジッタ込み) の VP、`gWorld` = 今の World、`gTime` = 今の時刻を代入して `VSMain` → 作者 `PSMain`
- 速度 (Deferred 不透明のみ): 前 (非ジッタ前 VP・`prevWorld`・前時刻) で `VSMain` を 1 回、今で 1 回評価し、今の結果を `PSMain` へ、両クリップ座標から `ComputeVelocityUv` (common.hlsli) で velocity を SV_Target1 へ。前履歴が無いフレームは velocity 0 (既存 GBuffer と同じ規則)
- 影 (CSM): `gViewProj` = カスケードのライト VP で `VSMain` を評価、PS なし (深度のみ)。深度バイアス等のステートは既存 ShadowPass と同じ。**影エントリでは `MyEnginePerFrame` は 0 埋め** (ShadowPass が環境の確定より前に走るため)。`MyEngineSurfaceFrame` の時刻・`MyEnginePerObject`・`MyEnginePerMaterial`・作者テクスチャ・`MyEngineWater` は張られる。→ 作者規約: VSMain の変位に `MyEnginePerFrame` の値 (`gCameraPos` 等) を使うと影だけ形が違う (sub-03 VERDICT round 1 で確定。docs とテンプレートのコメントに書く)

#### パスへの組み込み
| パス | 不透明サーフェス | 透明サーフェス (`transparent: true`) |
|---|---|---|
| Forward | 既存の不透明ループ内で色エントリ (深度書き込み) | 既存の透明ループ内で色エントリ (深度書かない・αブレンド) |
| Deferred | GBuffer ループから除外 → **SSR の後・水面の前**に「フォワード段」: RT = HDR シーン＋`gbVelocity`、既存 DSV (深度テスト＋書き込み)、速度エントリ | 既存の透明段で色エントリ (速度は書かない) |
| CSM 影 | 影エントリで描く (インスタンス run から除外) | 影を落とさない (既存どおり透明は影対象外) |
| シャドウアトラス | 従来シェーダ (変位なし) | 同上 |

- **カリングと境界 (review-1 #3)**: 視錐台カリング (`RenderSystem.cpp` のステージ 2) と CSM のキャスター AABB は、サーフェスマテリアルのアイテムについて **メッシュの AABB をワールド空間で各軸 `boundsPadding` [m] だけ広げた箱**で判定する。既定 0 = 従来と同じ (変位で外へ出る形は作者が余白を付ける。UE の Bounds Scale / Unity の Renderer bounds と同じ考え方)。WaterWave の水面アイテムは従来どおりカリングしない
- **両面 (review-1 #7)**: `doubleSided: true` のサーフェスは色・速度・影の全エントリを Cull None で描く。既定 false = Cull Back (従来)
- **固定スロットの復元 (review-1 #1 #2)**: サーフェスの描画は名前解決で CB / SRV / サンプラ / シェーダ / 入力レイアウトを張り替える。各パス (Forward の不透明・透明、Deferred のサーフェス段・透明段、ShadowPass) は、サーフェスを描いた直後に**そのパスの非サーフェス描画が前提とする固定バインド (VS/PS の CB・SRV・サンプラ、IL/VS/PS、instance バッファ SRV) を全部戻す**。混在の順序 (サーフェスが先 / 後) に依らず、forward_lit・instanced・スキン・失敗時フォールバックの絵と影が変わらないこと
- Deferred のフォワード段は、サーフェスのアイテムが 0 件なら RT / ステートを一切触らない (既定シーンのビット一致)
- Deferred のサーフェス画素は GBuffer に居ないので SSAO / SSR / デカール / RT 受光が掛からない (後回し。docs に明記)
- ピッキング・RT の BVH は従来どおり変位なしのメッシュ形 (docs に明記)

#### 失敗時
| 対象 | 挙動 |
|---|---|
| `shader` 名が索引にも組込みにも無い | マゼンタ (組込みエラーシェーダ `surface_error`、変位なし、Deferred では剛体の速度も書く) ＋ Console ERROR 1 回 |
| コンパイル失敗 / 生成エントリの規約違反 / 入力レイアウト作成失敗 | 同上。エラー文をマテリアル Inspector のバナーにも出す |
| Properties パース失敗 | 同上 (M78 と同じく「シェーダ全体を無効」) |
| ホットリロード失敗 | 旧プログラム維持＋WARN (既存 ShaderManager の規則) |
| Properties にあるが cbuffer / Texture に無い名前 | WARN、値は無視。描画は続ける |
| `.mat.json` の `properties` にあるがスキーマに無いキー | 読み込みで保持 (シェーダを戻したとき値が残る)、CB には書かない |
| テクスチャ未割当 / 未解決 GUID | M78 と同じ組込み既定 (`white` / `black` / `gray` / `bump`、不明名は white＋WARN) |
| スキン＋サーフェス | 従来のスキン経路で描く＋WARN 1 回 |

### 4.2 データ・保存形式・互換性

#### `.mat.json`
```json
{
  "engine": "MyEngine",
  "material": 1,
  "name": "Ukiyo Boat",
  "shader": "UkiyoFlat.surface",
  "baseColor": [0.62, 0.36, 0.14, 1.0],
  "transparent": false,
  "properties": {
    "_InkColor": [0.04, 0.05, 0.07, 1.0],
    "_RimPower": 2.8,
    "_PaperTex": 1234567890123
  }
}
```
- `properties` の値の符号化は fxstack (`FxStackAsset.cpp:19` の `ParseProperties(json)`) と同じ: 数値 = Float/Range、4 要素配列 = Color/Vector、Tex2D は**数値 GUID と文字列の両方を受理** (文字列 = 組込み名 `white` 等・16 進 GUID・assets 相対パス)。書き出し (Inspector) は、アセットのテクスチャは数値 GUID (Material 本体の `texture` / `normalMap` と同じ規約)、組込み既定は名前文字列。初版のテクスチャ読み込みは常に sRGB (DSL に色空間指定が無いため。マスク / ノイズ等のリニア指定は後回し) (sub-02 VERDICT round 1 で確定)
- `boundsPadding` (float [m]、欠損 = 0、負値は 0 に丸めて WARN) と `doubleSided` (bool、欠損 = false) を追加 (review-1)。サーフェスでないマテリアルでは読み込むが効かない (横テーブル側に持ち `Material` POD は変えない)
- 既存フィールド (metallic / roughness / emissive / texture / normalMap / reflectionClass) は従来どおり読み書きする (サーフェスでも RT の BVH ヒット等が使う)。Inspector 保存で `properties` を落とさない
- `properties` 欠損 = 全部既定値。`shader` 欠損 = `forward_lit` (従来)
- `Material` POD (`GpuResources.h:204`) のサイズ・並びは変えない (cooked キャッシュ互換)

#### 予約 CB のレイアウト
- C++ 構造体 1 本を正本とし、HLSL (include) は写し。**SelfTest が include を実際にコンパイルし、D3DReflect の各変数オフセットを C++ の `offsetof` と照合する** (AGENTS 4.3「C++ と HLSL の共有定数・レイアウト一致」)
- `MyEnginePerFrame` を既存 PerFrame バッファの使い回しにする場合、既存 PerFrameCB へ末尾追加したときは include も同時に直す必要がある → 同じ SelfTest が検出する

#### WaterWave (sub-05)
- `WaterWaveComponent` に `AssetID surfaceMaterial` (FieldType::AssetRef) を**末尾追加**。欠損 = null = 従来の WaterPass。TypeId 不変
- 描画専用。WorldHash / リプレイに入らないこと (既存シーンの replay 一致で確認)

#### 互換
- ABI: 変更なし (`MYE_API_VERSION` 21)
- M78 のポスト / コンピュートの Properties パック規則 (パース順) は変更しない。サーフェスだけリフレクションのオフセット
- 既存 `*.hlsl` の読み込み名・上書き規則は変えない (`water_surface` の上書きは引き続き効く)

### 4.3 UI / ビジュアル
- マテリアル Inspector: `shader:` のグレー表示をコンボに置き換える (項目 = `forward_lit` ＋ 索引済み `*.surface` の短名。昇順)。サーフェス選択時は既存欄の下に「Properties」セクション (fxstack と同じウィジェット: Float=Drag / Range=Slider / Color=ColorEdit(HDR 属性) / Vector=Drag4 / 2D=テクスチャ参照 / Header / HideInInspector)
- サーフェス選択時は `boundsPadding` (DragFloat、0 以上) と `doubleSided` (チェックボックス) も出す。ツールチップで「頂点変位で形がメッシュの外へ出るなら余白を付ける (付けないとカメラの外判定で消える)」を明示 (review-1)
- シェーダが失敗状態ならマテリアル Inspector の上部に赤字バナー (シェーダ名＋エラー先頭行)
- マテリアルプレビュー (M53) はサーフェスでもそのシェーダで描く (Forward パスを通るので自然に効く想定。効かない場合は理由を実装メモに書き、プレビューはマゼンタにしない = 既存の forward_lit 表示のまま、でよい)
- Asset Browser の作成メニューに「サーフェスシェーダ」。テンプレートは Properties 2 件・include・PerMaterial・VSMain (`gWorld`/`gViewProj` を使う例)・PSMain (太陽光＋影＋霧のヘルパ使用例) を含み、そのままコンパイルが通ること
- マゼンタ = (1,0,1) 不透明。スクショで判別できること
- ローカライズ: 新規 UI 文字列は `LocalizationTable.inl` と `Tr()`、日英両方

### 4.4 非機能
- **決定論**: サーフェス描画はワールドハッシュ非関与。`gTime` は描画通番由来で sim へ入らない。`tools\replay_verify.bat` が従来どおり一致
- **既定の絵**: サーフェスマテリアルを持たないシーンは Forward / Deferred とも従来と 1 ビットも変わらない (フォワード段・影エントリ・横テーブル参照が 0 件時に何も張らない)。既存 SelfTest / golden 系が無変更で通る
- **層**: 生 D3D 型は Renderer 内。Editor は ShaderManager / MaterialLibrary の公開 API 越しにスキーマとエラー文を得る
- **性能**: 速度エントリは VS を 2 回評価する (頂点数比例)。初版は計測しない・最適化もしない (WARP では有意な値が取れないため、実 GPU での計測は後回し。sub-03 VERDICT round 1 で変更)
- **Debug / Release**: 生成エントリ・コンパイルフラグは構成で同一 (既存 ShaderManager 規則)
- **バイトコードキャッシュ**: 生成エントリを含むソース全体をキーにする (作者ファイルか include が変われば再コンパイル)

## 5. 受け入れ条件

1. `*.surface.hlsl` を assets の任意フォルダに置くと短名 `X.surface` で索引され、同名 2 本は両方無効＋ERROR
   — 検証: `Editor.exe --selftest` (ShaderManagerProjectIndexSelfTest に surface ケース追加)
2. 生成エントリ (色・速度・影) が、規約どおりの作者シェーダ (register なし・VSIn が部分集合) でコンパイルでき、予約 CB / 予約テクスチャ / `MyEnginePerMaterial` / 作者 `Texture2D` のスロットが D3DReflect の名前で解決される。規約違反のフィクスチャは「サーフェス規約」付きエラーで失敗する
   — 検証: `--selftest` (フィクスチャ HLSL をコンパイルしてスロット表とエラー文を検査)
3. 予約 CB の HLSL レイアウトが C++ 構造体とオフセット単位で一致する
   — 検証: `--selftest` (リフレクション照合)、`tools\check_rules.ps1`
4. `.mat.json` の `shader: "X.surface"` のメッシュが Forward パスで作者の式で描かれる (不透明・透明)。`properties` の Float/Range/Color/Vector/2D が CB / SRV に反映される
   — 検証: `--selftest` (properties の読み込み→リフレクションオフセットでのパック)、一時シーン＋`Runtime.exe --screenshot` で平塗りとパラメータ違い 2 枚
5. 失敗 (名前なし / コンパイル失敗 / 規約違反 / Properties パース失敗) でそのメッシュがマゼンタになり Console にエラー、クラッシュしない。ホットリロード失敗で旧絵維持
   — 検証: 一時シーンのスクショ＋ログ抜粋、`--selftest` (失敗時にエラーシェーダへ落ちる判定)
6. Deferred パスで不透明サーフェスが光パス後のフォワード段で描かれ、深度と velocity を書く。`gTime` 駆動の頂点変位があるメッシュは、カメラ静止でも `--velocity-debug` に変位の動きが出る。`--taa` 有効時、変位メッシュの縁にゴースト (前フレームの形の残像) が出ない
   — 検証: `Runtime.exe --deferred --velocity-debug --screenshot` と `--deferred --taa --screenshot` (決定的撮影フレーム指定) の画像、比較用に変位なし版
7. 太陽 CSM の影が変位後の形で落ちる
   — 検証: 変位平面＋受け側平面の一時シーンで変位あり/なしスクショ 2 枚
8. マテリアル Inspector でシェーダを選べ、Properties が自動 UI で出て、保存で `.mat.json` の `properties` に書かれ描画に反映される。`assets/shaders` 以外に置いたシェーダでも Properties が出る (fxstack 側も同様に索引経由になる)。失敗時は赤字バナー
   — 検証: 手動操作＋Editor スクショ (`--screenshot` 一時プローブ)、`--selftest` (MaterialEditToJson が properties を保持)
9. 作成メニューの「サーフェスシェーダ」テンプレートが生成され、そのままコンパイル成功する
   — 検証: `--selftest` (AssetOpsSelfTest に作成＋コンパイル)
10. WaterWave の `surfaceMaterial` 設定時、水面がサーフェス経路で描かれ (`MyEngineWater` で波パラメータが渡る)、Deferred で速度・CSM 影に変位が反映される。未設定時は従来の WaterPass と同一。浮力の結果は設定の有無で変わらない
    — 検証: 一時シーンのスクショ (設定あり/なし)、`--selftest`、`tools\replay_verify.bat`
11. サーフェスを使わない既存シーン・既定シーンで絵・決定論・ABI が不変: `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat` がすべて成功。`MYE_API_VERSION == 21`、`sizeof(Material)` 不変
    — 検証: 各コマンドの結果
12. 新規 UI 文字列が日英両方で `LocalizationTable.inl` に入っている
    — 検証: `tools\check_rules.ps1` (ローカライズ規則)、diff
13. (review-1 #1 #2) サーフェスと forward_lit / instanced / 失敗時フォールバックが混在しても、描画順 (サーフェスが先・後) に依らず非サーフェス側の色・影・instanced ジオメトリが変わらない。VS で Texture2D を読むサーフェスの後でも instanced が消えない。D3D デバッグレイヤにスロット不一致エラーが出ない
    — 検証: `--selftest` (Forward / Deferred / ShadowPass で混在順序を入れ替えた read-back 比較)、reviewer の rv1 シーン (shadowA/shadowB/vtexA 相当) の再撮影
14. (review-1 #3) `boundsPadding` を付けたサーフェスは、変位で元の AABB の外へ出てもカメラに写り影も落とす。0 のときは従来どおり (カリングされうる)
    — 検証: `--selftest` (カリング判定の単体)、rv1 の liftA 相当の再撮影 (余白あり/なし)
15. (review-1 #7) `doubleSided: true` のサーフェスは裏面も描かれ、影も両面で落ちる。false は従来どおり
    — 検証: `--selftest` または一時シーンのスクショ (薄板の裏側から)

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | サーフェス規約の成立確認: 索引・共通 include・生成エントリ・名前バインド表 | なし | 1, 2, 3, 11 | `M79a: サーフェスシェーダの規約と生成エントリ` |
| sub-02 | Forward 描画とマテリアル (遅延 Load・横テーブル・properties・マゼンタ) | sub-01 | 4, 5, 11 | `M79b: Forward のサーフェス描画とマテリアル Properties` |
| sub-03 | Deferred フォワード段・速度・CSM 影 | sub-02 | 6, 7, 11 | `M79c: Deferred のサーフェス段と変位込みの速度・影` |
| sub-04 | マテリアル Inspector (シェーダ選択・Properties 共通化・バナー) と作成メニュー | sub-02 | 8, 9, 12 | `M79d: マテリアル Inspector のシェーダ選択と Properties` |
| sub-05 | WaterWave の surfaceMaterial と MyEngineWater | sub-03 | 10, 11 | `M79e: WaterWave の水面をサーフェスシェーダで描く` |
| sub-06 | サーフェスの境界余白と両面 (boundsPadding / doubleSided) | sub-03 (差し戻し分), sub-04 (差し戻し分) | 14, 15, 12 | `M79f: サーフェスの境界余白と両面描画` |

- review-1 の差し戻し: sub-02 (#2 #4)・sub-03 (#1)・sub-04 (#5)・sub-05 (#6)。#1 と #2 は同じ根 (名前解決で張ったスロットを固定スロット前提の後続へ戻していない) なので、受け入れ条件 13 は sub-02 と sub-03 の両方で満たす。修正コミットは `M79b-fix:` 等の既存慣例

- sub-03 と sub-04 は互いに依存しない (並列可)。sub-05 は sub-03 の速度・影経路を使う
- sub-01 は「static 代入による再評価」「register なし自動割当と名前解決」「MeshVertex 固定オフセット」の 3 つの未知を最初に潰す。**成立しない場合は sub-02 以降に進まず planner へ差し戻す** (方式を「任意の追加エントリ」へ切り替える判断になるため)

## 7. 未決事項・リスク

**ユーザーに確認したい裁定 (司会が聞く)**
1. `[ユーザーに聞ける]` TAA のために、サーフェスを Forward パスだけでなく Deferred パスでも (フォワード段として) 描く。— 逆 (依頼どおり Forward パスのみ) なら sub-03 の速度部分が消え、動画で TAA が使えない / 逆 (Forward パスに速度＋TAA を足す) なら新サブ 1〜2 本 (forward_lit 系全シェーダへの速度出力) が増える
2. `[ユーザーに聞ける]` 頂点変位の反映は「VSMain 再評価 (エンジンが包むエントリを自動生成)」、作者の追加エントリは作らない。— 逆 (Unity 式の任意エントリ `VSShadow` / `PSVelocity`) なら sub-01 の生成エントリがエントリ検出に置き換わり、作者規約 3 つ (型名・`pos`・static 使用) が消える代わりに、変位の式を書き忘れると TAA ゴースト / 影ずれが静かに出る
3. `[ユーザーに聞ける]` WaterWave の水面をサーフェスで差し替える口 (sub-05) を M79 に含める。— 逆 (含めない) なら sub-05 が消え、水面は今の `water_surface.hlsl` 上書きのまま = 速度を書かないので TAA で水面にゴースト、影も無し
4. `[ユーザーに聞ける]` `gTime` は描画通番 / 60 (水面と同じ時計)。— 逆 (tick 時間) なら描画 FPS に依存しなくなるが Edit モードでアニメが止まり、水面 (WaterWave) と時計が別になる。変更箇所は sub-01 の時刻供給 1 か所
5. `[ユーザーに聞ける]` PerMaterial はリフレクションのオフセットで詰める (M78 のパース順と違う)。— 逆 (M78 と同じパース順) なら作者の cbuffer の並びを Properties と揃える義務が残る。変更箇所は sub-02 のパック 1 か所

6. `[ユーザーに聞ける]` (review-1 #3) 変位で AABB の外へ出る形は `.mat.json` の `boundsPadding` で作者が包む (既定 0)。— 逆 (サーフェスはカリングしない = スキンと同じ保守策) なら余白を付け忘れて消える事故は無くなるが、CSM フィットは変位前の箱のままで大波の影が端で切れうる / サーフェス数に比例して描画コストが増える。sub-06 の中身が「カリング除外＋CSM フィット対象外」に変わるだけで本数は同じ
7. `[ユーザーに聞ける]` (review-1 #7) 両面描画 `doubleSided` を M79 に含める (Water の breaking_wave.hlsl:130-134 が裏面を描く前提のため)。— 逆 (後回し) なら sub-06 は boundsPadding だけになり、大波の裏面は描かれない (メッシュが両面を持っていれば影響なし。未確認)

**実装で判明する見込みのリスク (coder が「不安・質問」で拾う)**
- fxc の自動 register 割当が include の明示 register と衝突しないか (名前解決なので衝突しても動くはずだが、未使用で消えた予約資源と作者資源が同じスロットに乗る場合の扱い) — sub-01
- static グローバル代入を含む生成エントリで fxc が警告 / 最適化上の問題を出さないか — sub-01
- 速度エントリの MRT (HDR + R16G16F) とブレンド状態 (IndependentBlendEnable) の整合 — sub-03
- Deferred のフォワード段をどこに挟むか (SSR の合成が既にシーンへ乗った後であること) の実コード確認 — sub-03
- 描画通番時計は撮影モード外で FPS 比例に速く見える (既存水面と同じ)。動画は決定的撮影で撮る前提
- レンダパスはセッション切替で保存されない。TAA 付きの確認・撮影は `--deferred` を毎回付ける (後回し項目)
- WaterWave の新フィールドが WorldHash に入らないことの確認方法 — sub-05 で `kFieldNoHash` 新設により解決
- (別件・M79 の回帰ではない) `shot_verify.bat` が 23 枚 FAIL する — golden が M79 より前から陳腐化 (golden の空が黒い)。reviewer が 3b55f4a と HEAD の既定シーン (Forward/Deferred) のビット一致を確認済み。golden の撮り直しは別件として申し送る (review-1 #8)
- (後続マイル候補・未調査) オブジェクトを極端な座標 (1000,1000,1000) に置くと CSM のシーン AABB 由来とみられる描画異常 (sub-05 検証中に coder が観測。M79 の変更とは無関係の推測)。`ComputeCascadeVPs` の頑健性調査

## 8. 変更履歴

- 2026-09-24: 初版確定 (PLAN)。AskUserQuestion 不可のため §2 を planner 裁定で埋め、§7 に `[ユーザーに聞ける]` 5 件。依頼の前提「water_surface.hlsl は未使用」を訂正 (組込みを上書きして使用中)。「フォワードのみ」を「シェーディング方式」と解釈し Deferred でも描く裁定。
- 2026-09-24: ユーザーが §7 の 5 件を全件裁定どおりで確定 (司会経由)。
- 2026-09-24: sub-01 VERDICT OK (coder SELF_EVAL round 1)。(1) `MyEnginePerFrame` は既存 PerFrame から先頭 `viewProj` を除いた内容・別バッファと確定 (§4.1)。(2) サーフェスのホットリロードとバイトコードキャッシュ配線は sub-02 へ。(3) `MyeApplyFog` のフロクセル合成は sub-02 の should。(4) coder が見つけた `.cs.hlsl` 索引の off-by-one (M78 の既存不具合、実コードで確認) を §3 やるに追加し sub-04 で直す。
- 2026-09-24: sub-02 VERDICT REWORK (round 1)。§4.1 に「PS エントリも static へ今フレーム値を代入」を明文化 (spec の穴。sub-01 の生成エントリが PS 側を未代入だった)。§4.2 Tex2D 符号化を「数値 GUID と文字列の両受理、書き出しは GUID 数値 / 組込み名」、読み込みは sRGB 固定と確定。
- 2026-09-24: sub-02 VERDICT OK (round 2)。仕様変更なし。
- 2026-09-24: sub-03 VERDICT REWORK (round 1)。§4.1 影エントリの `MyEnginePerFrame` 0 埋めを受理し作者規約として明記。§4.4 GPU 時間計測を後回しへ (WARP で測れない)。sub-03 に自動 SelfTest を must で追加 (AGENTS.md §7「レンダラー変更は回帰テストを追加」— sub-03.md に書き漏らした planner の穴)。
- 2026-09-24: sub-03 VERDICT OK (round 2)。仕様変更なし。
- 2026-09-24: sub-04 VERDICT REWORK (round 1)。仕様変更なし (シェーダ切替でも properties を保持する契約は §4.1 のとおり。coder の再解釈を却下)。
- 2026-09-24: sub-04 VERDICT OK (round 2)。仕様変更なし。
- 2026-09-24: sub-05 VERDICT REWORK (round 1)。Deferred 透明段のサーフェス未対応 (coder 発見) は spec §4.1 表の実装漏れ (sub-03 の VERDICT で planner が見落とし) と判定し、M79 内 (sub-05) で直す。WaterWave の `surfaceMaterial` は保存するがハッシュしない新フラグ `kFieldNoHash` で登録することを受理 (§4.2 の「ハッシュ非関与」の実現手段)。水面 RenderItem は CSM フィット AABB に含めない (従来 WaterPass と同じ)。
- 2026-09-24: sub-05 VERDICT OK (round 2)。Deferred 透明段のサーフェス描画を回収 (§4.1 表どおり)。仕様変更なし。§7 に CSM の極端座標の観測を後続候補として記録。
- 2026-09-24: reviewer round 1 (review-1.md) への応答。#3 を仕様の穴として認め、`boundsPadding` (カリング・CSM フィットの余白、既定 0) を §3/§4.1/§4.2/§4.3 に追加、受け入れ条件 14。#7 を認め最小の両面指定 `doubleSided` を追加 (受け入れ条件 15、裏面判定の SV_IsFrontFace は後回し)。#1 #2 の根にある「名前解決で張ったスロットを戻す」契約を §4.1 に明文化し受け入れ条件 13 (混在順序) を追加 — spec が混在を Forward 色だけでしか条件化していなかった穴。#8 は M79 の回帰ではなく別件 (§7 に記録)。新サブ sub-06、差し戻し sub-02/03/04/05。
- 2026-09-24: sub-02 VERDICT OK (round 3、review-1 #2 #4)。仕様変更なし。
- 2026-09-24: sub-03 VERDICT OK (round 3、review-1 #1)。受け入れ条件 13 は sub-02 と合わせて充足。仕様変更なし。
- 2026-09-24: sub-05 VERDICT OK (round 3、review-1 #6)。テンプレート (AssetOps.cpp) の黒潰れは sub-06 へ移管 (sub-04 並行作業とのファイル衝突回避)。
- 2026-09-24: sub-04 VERDICT OK (round 3、review-1 #5)。仕様変更なし。
- 2026-09-24: sub-06 VERDICT OK (round 1)。§4.2 補足: Inspector は `boundsPadding` / `doubleSided` を forward_lit のマテリアルでも常に書き出す (既存スカラと同じ規約。サーフェスでないマテリアルでは読み込むが効かない)。
