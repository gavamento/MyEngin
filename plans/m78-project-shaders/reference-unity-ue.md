# M78 参照調査 — Unity / Unreal の「プロジェクト側が独自シェーダを足す」設計

調査日 2026-09-22。原則として Unity Manual (docs.unity3d.com) と Epic Developer Community
(dev.epicgames.com) の一次情報。一次ページが取れなかった箇所は **[二次]** と明記する。
メモリ `reference-existing-engines.md`「方式を決める前に既存エンジンを当たる」に対応する文書。

## 0. 全体像

| 軸 | Unity | Unreal |
|---|---|---|
| 作者が書くもの | テキストファイル (`.shader` / `.compute` / `.hlsl`) | ノードグラフ (`.uasset`)。生 HLSL は Custom ノード / プラグインの `.usf` に限定 |
| 誰がパラメータ UI を作るか | エンジンが `Properties` ブロックを**パースして**インスペクタを生成 | パラメータ**ノード**に名前を付けると、そのノードがインスペクタ項目になる |
| コンパイル単位 | シェーダファイル × キーワード組合せ (バリアント) | マテリアル × 頂点ファクトリ × パス × 静的スイッチ (パーミュテーション) |
| コンパイル時期 | インポート時に最小処理 → 必要時にキャッシュ照合してコンパイル → ビルド時に全部 | エディタでマテリアル保存時に非同期コンパイル、結果は DDC へ |
| 失敗時 | マゼンタの error shader | PSO 未完了時にエンジン既定マテリアルへフォールバック or 描画スキップ |

★両者に共通する最重要の設計判断: **シェーダ作者はレジスタ番号も定数バッファのレイアウトも書かない**。
名前 (Unity) またはノード ID (UE) だけを宣言し、バインディングはエンジンが生成する。

---

## 1. サーフェスシェーダ

### 1.1 Unity / ShaderLab

**配置**: `.shader` は「Assets フォルダに置かれた拡張子 `.shader` のテキストファイル」。
特別なフォルダ規約なし。`Assets > Create > Shader` で雛形生成。
出典: https://docs.unity3d.com/Manual/class-Shader.html

**構造** (公式シグネチャ):
```
Shader "<name>"
{
    <optional: Material properties>
    <One or more SubShader definitions>
    <optional: custom editor>
    <optional: fallback>
}
```
`Fallback` = 主シェーダが実行できないときの代替。`CustomEditor` = エディタでの表示のされ方
(レンダーパイプラインごとに別エディタ指定可)。
出典: https://docs.unity3d.com/Manual/SL-Shader.html

Pass は `Pass { <name> <tags> <commands> <shader code> }`。commands は
`Cull` / `ZWrite` / `ZTest` / `Blend` / `ColorMask` / `Stencil` 等。
出典: https://docs.unity3d.com/Manual/SL-Pass.html

SubShader Tags: `RenderPipeline` (`UniversalPipeline` / `HDRenderPipeline`)、
`Queue` (Background / Geometry / AlphaTest / Transparent / Overlay)、`RenderType`、
`ForceNoShadowCasting`、`IgnoreProjector`、`PreviewType`。
出典: https://docs.unity3d.com/Manual/SL-SubShaderTags.html

**Properties ブロックの型** (公式表記そのまま):
```
_ExampleName ("Integer display name", Integer) = 1
_ExampleName ("Float display name", Float) = 0.5
_ExampleName ("Float with range", Range(0.0, 1.0)) = 0.5
_ExampleName ("Texture2D display name", 2D) = "red" {}
_ExampleName ("Texture2DArray display name", 2DArray) = "" {}
_ExampleName ("Texture3D", 3D) = "" {}
_ExampleName ("Cubemap", Cube) = "" {}
_ExampleName ("CubemapArray", CubeArray) = "" {}
_ExampleName ("Example color", Color) = (.25, .5, .5, 1)
_ExampleName ("Example vector", Vector) = (.25, .5, .5, 1)
```
- テクスチャ既定値は文字列リテラル `"white" / "black" / "gray" / "bump" / "red"` で、
  未指定時にエンジンが組込みテクスチャを挿す ← **MyEngine の `textures.White()` と同じ思想**
- Color は HLSL 上 float4、Inspector はカラーピッカー。Vector は 4 つの float 欄
- Range の両端は**含む**スライダ

**属性 (= UI の指示)**: `[Gamma]` `[HDR]` `[HideInInspector]` `[MainTexture]` `[MainColor]`
`[NoScaleOffset]` `[Normal]` `[PerRendererData]`。
出典: https://docs.unity3d.com/Manual/SL-Properties.html

`MaterialPropertyDrawer` 系は UI とシェーダキーワードを同時に制御:
```
[Toggle(ENABLE_EXAMPLE_FEATURE)] _ExampleFeatureEnabled ("Enable example feature", Float) = 0
[PowerSlider(3.0)] _Shininess ("Shininess", Range (0.01, 1)) = 0.08
[IntRange] _Alpha ("Alpha", Range (0, 255)) = 100
[Header(A group of things)] _Prop1 ("Prop1", Float) = 0
```
`[Toggle]` はチェックボックス + キーワード有効化 (既定名は `大文字プロパティ名_ON`)。
`[KeywordEnum]` は最大 9 個のポップアップ。`[Enum]` `[Space]` `[Header]` も同系。
出典: https://docs.unity3d.com/ScriptReference/MaterialPropertyDrawer.html

**UI を丸ごと乗っ取る口**: `CustomEditor "ClassName"` → `ShaderGUI` 派生の
`OnGUI(MaterialEditor, MaterialProperty[])`。`properties` は **Properties ブロックから
自動生成されたもの**で、`base.OnGUI()` で既定 UI をそのまま描ける (= パースは常に走る)。
出典: https://docs.unity3d.com/ScriptReference/ShaderGUI.html

**HLSL 側の受け取り = 名前一致**: 「同じ名前で互換型の変数を宣言する」。
Color/Vector → `float4`/`half4`、Range/Float → `float`/`half`、2D → `sampler2D`、
Cube → `samplerCUBE`、3D → `sampler3D`。**レジスタ番号は作者が書かない**。
C# の `Material.SetFloat("_MyFloat", v)` も同じ文字列名 (実運用は `Shader.PropertyToID` で int 化)。
出典: https://docs.unity3d.com/Manual/SL-PropertiesInPrograms.html

**定数バッファ (SRP Batcher)** — URP/HDRP では作者が自分で cbuffer を切る義務がある:
```
CBUFFER_START(UnityPerMaterial)
float4 _BaseMap_ST;
half4  _BaseColor;
CBUFFER_END
```
- 全マテリアルプロパティを **`UnityPerMaterial` という単一 CBUFFER** に
- 組込みエンジンプロパティ (`unity_ObjectToWorld` 等) は **`UnityPerDraw`** に分離
- 複数 Pass があっても**全 Pass が同じ CBUFFER レイアウト**でないと非互換
- テクスチャ/サンプラは CBUFFER に入らない
- SRP Batcher は定数バッファを GPU 側に persist させ、描画間の再アップロードを省く

出典: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/shaders-in-universalrp-srp-batcher.html ,
https://docs.unity3d.com/Manual/SRPBatcher.html

**予約名**: `UNITY_MATRIX_MVP/MV/V/P`、`unity_ObjectToWorld`、`unity_WorldToObject`、
`_WorldSpaceCameraPos`、`_ScreenParams`、`_ProjectionParams`、`_ZBufferParams`、
`_Time` (= `(t/20, t, t*2, t*3)`)、`_SinTime`/`_CosTime`、`unity_DeltaTime`、`unity_FogColor`。
出典: https://docs.unity3d.com/Manual/SL-UnityShaderVariables.html

**コンパイル時期**:
- インポート時は最小限の処理 (サーフェスシェーダ生成など) だけ
- エディタ実行時は必要バリアントだけ `Library/ShaderCache` と照合、無ければコンパイルして保存。
  「このフォルダは削除しても安全で、再コンパイルが走るだけ」
- ビルド時に必要な全バリアントを全グラフィックス API 向けにコンパイル
- OpenGL/Metal/Vulkan は FXC 出力を **HLSLcc** でクロスコンパイル

出典: https://docs.unity3d.com/Manual/shader-compilation.html

**バリアント (キーワード)**:
```
#pragma shader_feature REFLECTION_TYPE1 REFLECTION_TYPE2 REFLECTION_TYPE3
```
1 pragma が「セット」を作るが「実行時には Unity はセットの概念を持たず、どのキーワードも
独立に on/off できる」ので実質セット間の直積。「合計 128 を超えるキーワードでプロジェクトが遅くなる」。
`shader_feature` は**どのマテリアルからも使われていないバリアントはビルドに含まれない**、
`multi_compile` は使用有無に関わらず含まれる。
出典: https://docs.unity3d.com/Manual/SL-MultipleProgramVariants-declare.html ,
[二次] https://docs.unity3d.com/Manual/shader-variant-stripping.html

**Shader Variant Collection**: 事前ロードする Pass タイプ + キーワード組合せのリスト。
prewarm 用だが「ビルドから strip されないことを保証する」役割も持つ。
出典: https://docs.unity3d.com/Manual/shader-variant-collections.html

**失敗時のフォールバック**:
- **マゼンタ = error shader**。「マテリアル未割当」「コンパイル不能」「非サポート」時。
  エディタでもビルドでも同じ
- **シアン = loading shader**。必要バリアントをコンパイル中 (非同期コンパイル有効時 /
  開発ビルドの Shader Live Link 時)
- `BatchRendererGroup.SetErrorMaterial` / `SetLoadingMaterial` で差し替え可

出典: https://docs.unity3d.com/Manual/shader-error.html

### 1.2 URP/HDRP の Forward / Deferred 両対応の強制のしかた ★M78 の核心

**URP は「Pass の LightMode タグ」でパス種別を宣言させる**:

| LightMode | 役割 |
|---|---|
| `UniversalForward` | オブジェクトジオメトリを描画し、すべてのライト寄与を評価する |
| `UniversalGBuffer` | ライト寄与を一切評価せずにジオメトリを描画する = GBuffer 書き込み |
| `UniversalForwardOnly` | Deferred パスでも強制的に前方ライティング。「Clear Coat の法線のようにデータが GBuffer に入らない」場合 |
| `DepthNormalsOnly` | `UniversalForwardOnly` と併用し Depth+Normal プリパスで描く |
| `ShadowCaster` / `DepthOnly` / `DepthNormals` / `Meta` / `Universal2D` / `MotionVectors` | 各補助パス |
| `SRPDefaultUnlit` | LightMode 未記載 Pass の既定値。アウトライン等の追加パス用 |

出典: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/urp-shaders/urp-shaderlab-pass-tags.html

→ **URP は両対応を強制しない**。「Deferred で描かれたければ `UniversalGBuffer` パスを書け、
書かないなら Forward-only 扱い」という宣言主義。GBuffer に入らないマテリアルは自動で前方パスに落ちる。

**GBuffer に書くときの規約 (URP の GBuffer レイアウト)**:
- GBuffer0 RGBA8(sRGB): RGB=albedo 24bit、A=material flags ビットフィールド
  (bit0 ReceiveShadowsOff / bit1 SpecularHighlightsOff / bit2 SubtractiveMixedLighting /
  bit3 SpecularSetup / bit4-7 予約)
- GBuffer1: RGB=specular、A=baked occlusion
- GBuffer2: RGB=ワールド法線 24bit エンコード、A=smoothness
- GBuffer3: `B10G11R11_UFloatPack32` に emissive + ベイク済み GI
- GBuffer4 以降は条件付き (ShadowMask / Rendering Layer Mask / Depth as Color)

**スロット割り当ても意味もエンジン固定**で、ユーザーシェーダは「決められた意味の値を
決められたチャンネルに詰める」だけ。自由な G バッファ拡張は提供されない (拡張したければ Forward-only)。
出典: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/rendering/g-buffer-layout.html

**HDRP はもっと強い**: 「HDRP は AxF シェーダを除く**すべてのシェーダで Shader Graph を使う**」。
手書き ShaderLab は事実上サポート外で、Shader Graph の HDRP Target が全パスを自動生成する。
Lit Shader Mode は Forward / Deferred / Both の 3 択 (Both はビルド時間が増える)。
Fabric / Hair / AxF / StackLit / Unlit / 透明な Lit は**強制的に Forward**。
出典: https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@15.0/manual/Forward-And-Deferred-Rendering.html

**Built-in RP → URP 移植で守らされる規約** (= URP のカスタムシェーダ規約):
`CGPROGRAM` → `HLSLPROGRAM`、`Core.hlsl` の include、SubShader Tags に
`"RenderPipeline" = "UniversalPipeline"` (無いとマテリアルはマゼンタ)、
全プロパティを `CBUFFER_START(UnityPerMaterial)` に、`v2f`→`Varyings` / 入力 `Attributes`、
`UnityObjectToClipPos()`→`TransformObjectToHClip()`、`fixed4`→`half4`、
`sampler2D`/`tex2D` → `TEXTURE2D()`+`SAMPLER()`+`SAMPLE_TEXTURE2D()`、tiling/offset は `TRANSFORM_TEX()`。
出典: https://docs.unity3d.com/6000.2/Documentation/Manual/urp/urp-shaders/birp-urp-custom-shader-upgrade-guide.html

### 1.3 Unreal / マテリアルエディタ

マテリアルは `.uasset` (ノードグラフ)。生 HLSL は **Custom ノード**と
C++ プラグインの `.usf`/`.ush` (§3.2) の 2 つだけ。

**Material Domain**: Surface / Deferred Decal / Light Function / Volume / Post Process /
User Interface。Shading Model は 11 種 (Unlit / Default Lit / Subsurface / Preintegrated Skin /
Clear Coat / Subsurface Profile / Two Sided Foliage / Hair / Cloth / Eye / Single Layer Water)。
出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-properties

→ **Unity の LightMode に相当するものは UE には無い**。作者は「ドメイン + シェーディングモデル +
ブレンドモード」という**意味論**だけを申告し、Deferred/Forward/Depth/Shadow/Velocity 各パス用の
シェーダは**エンジンがグラフから自動生成**する。そもそもパスが作者に見えていない。

**Custom ノード**: Code 欄に「return 文を含む完全な関数本体」または単純な式。Inputs に付けた名前が
「マテリアルエディタ上の表示名であり、HLSL コード内で入力値を参照する名前でもある」。
`Additional Defines` / `Include File Paths` あり。警告: 「custom ノードの使用は定数畳み込みを妨げ、
組込みノードで同等のことをやる場合よりも大幅に多い命令数になりうる」。
出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/custom-material-expressions-in-unreal-engine

**パラメータとマテリアルインスタンス**:
- 「マテリアルインスタンシングは、高価な再コンパイルを起こさずに見た目を変えるために使う」
- 種別: Scalar / Vector / Texture / Static Switch
- **名前を付けることが公開の条件**: 「すべてのパラメータに一意で説明的な名前を付けることが重要」
- Material Instance Constant (実行前確定) と Material Instance Dynamic (実行時生成・変更)
- 静的スイッチだけは「組合せごとに新しいマテリアルがコンパイルされる」

出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-materials-in-unreal-engine

**Material Parameter Collection** (プロジェクト横断の共有パラメータ):
1 コレクションあたりスカラー 1024・ベクター 1024 まで、1 マテリアルが参照できるのは最大 2 コレクション。
**パラメータ数を変えると参照している全マテリアルが再コンパイル**される (リネームは不要)。
出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/using-material-parameter-collections-in-unreal-engine

**コンパイル単位とキャッシュ**:
- 「マテリアルが必要とするシェーダ一式は `FMaterialShaderMap` に入る」。
  「このスパース行列的なキャッシュ方式はシェーダ数をすぐ膨らませ、メモリとコンパイル時間を食う」
- 3 階層: Global Shader (「フルスクリーンクアッドのような固定ジオメトリに作用し、
  マテリアルと接続する必要が無い」) / Material Shader / Mesh Material Shader
  (「マテリアル属性**と**メッシュ型の両方に依存するため、マテリアル×頂点ファクトリの組合せごと」)
- 「コンパイル済みシェーダは DDC に格納され、そのキーにはシェーダソースを含む**全入力のハッシュ**が入る」
- ホットリロード: `r.ShaderDevelopmentMode=1` + **Ctrl+Shift+.** (= `recompileshaders changed`)

出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/shader-development-in-unreal-engine

**実行時フォールバック (PSO)**: `r.PSOPrecache.ProxyCreationWhenPSOReady` で
0 = 「PSO が準備できるまで描画をスキップ」/ 1 = 「準備できるまでエンジン既定マテリアルにフォールバック」。
出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine

---

## 2. ポストエフェクト

### 2.1 Unity URP — Renderer Feature / ScriptableRenderPass / Volume

**挿入順は 3 層に分かれている**:
1. **どの Renderer に足すか** — URP Renderer アセットの Renderer Features リスト (アセット上の並び)
2. **フレーム内のどこで走るか** — C# の `renderPassEvent`
3. **効果の強さ/パラメータ** — Volume (シーン上のボリューム、ブレンドあり)

```csharp
public class MyRendererFeature : ScriptableRendererFeature
{
    private RedTintRenderPass redTintRenderPass;

    public override void Create()   // ロード時/有効無効/プロパティ変更時
    {
        redTintRenderPass = new RedTintRenderPass();
        redTintRenderPass.renderPassEvent = RenderPassEvent.AfterRenderingSkybox;
    }

    public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
    {
        renderer.EnqueuePass(redTintRenderPass);   // 毎フレーム・カメラごと。ここでリソース生成しない
    }
}
```
出典: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/renderer-features/scriptable-renderer-features/inject-a-pass-using-a-scriptable-renderer-feature.html

**Volume 連携**: `Assets > Create > Rendering > URP Post-processing Effect
(Renderer Feature with Volume)` でテンプレート生成。パス側は
`VolumeManager.instance.stack?.GetComponent<NewPostProcessEffectVolumeComponent>()` で
ブレンド済みの設定値を読む。Volume Inspector への露出は「VolumeComponent のフィールドを
エンジンがリフレクションで拾う」方式。
出典: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/post-processing/custom-post-processing-with-volume.html

**コード無しの口**: Full Screen Pass Renderer Feature。プロパティは `Pass Material` /
`Pass` / `Injection Point` ("Before Rendering Transparents" / "Before Rendering Post Processing" /
"After Rendering Post Processing"(既定)) / `Requirements` (None / Everything / Depth / Normal /
Color / Motion — 選んだものだけ追加パスを生成) / `Fetch Color Buffer` / `Bind Depth-Stencil`。
出典: https://docs.unity3d.com/6000.0/Documentation/Manual/urp/renderer-features/renderer-feature-full-screen-pass.html

### 2.2 Unreal — Post Process Material ★M78 で採るならこちら

Material Domain を **Post Process** にし、「マテリアルは新しい色の出力に
**Emissive Color** だけを使うべき」。

**Blendable Location** (挿入位置):
| 値 | 内容 |
|---|---|
| Before Tonemapping | Post Process Input 0 を使うとき、ライティングは HDR のシーンカラーで供給される。TAA や GBuffer 参照の問題を回避できる |
| After Tonemapping | トーンマッピングとカラーグレーディング完了後。色が LDR で帯域が小さく性能上有利 |
| Before Translucency | Before Tonemapping より早く、透明がシーンカラーに合成される前 |
| Replacing the Tonemapper | エンジンのトーンマッパーを完全に置き換える (WIP 扱い) |

**Blendable Priority**: 同じ Location を持つマテリアルが複数あるときの実行順。
公式の区別: 「Post Process Volume 側の Priority は同一 blendable マテリアルの複数インスタンス間の
**ブレンド順**、マテリアル側の Blendable Priority は**異なる** blendable マテリアル間の**描画順**」。

**入力**: `SceneTexture` ノード (UV 入力、Color 4 成分出力、Size、InvSize)。
**注意**: 「控えめに、本当に必要なときだけ」。コストは HDR 8 バイト/画素、LDR 4 バイト/画素。
出典: https://dev.epicgames.com/documentation/en-us/unreal-engine/post-process-materials-in-unreal-engine ,
https://dev.epicgames.com/documentation/en-us/unreal-engine/blendables-in-unreal-engine

---

## 3. コンピュートシェーダ

### 3.1 Unity — `.compute`

「通常の描画パイプラインの外で GPU 上を走るシェーダプログラム」。拡張子 `.compute`。
`SystemInfo.supportsComputeShaders` で実行時に対応可否を問い合わせる。最小例:
```hlsl
#pragma kernel FillWithRed

RWTexture2D<float4> res;

[numthreads(1,1,1)]
void FillWithRed (uint3 dtid : SV_DispatchThreadID)
{
    res[dtid.xy] = float4(1,0,0,1);
}
```
出典: https://docs.unity3d.com/Manual/class-ComputeShader.html

**ディスパッチ**: `Dispatch(int kernelIndex, int threadGroupsX, int threadGroupsY, int threadGroupsZ)`。
「総起動数はグループ数 × スレッドグループサイズ」。グループサイズは `numthreads` 属性で決まり、
C# は `GetKernelThreadGroupSizes()` で問い合わせる。カーネルは `FindKernel()` で名前→インデックス解決。
間接ディスパッチは `DispatchIndirect()`。
出典: https://docs.unity3d.com/ScriptReference/ComputeShader.Dispatch.html

**ライフサイクル**: `ComputeBuffer(count, stride, type)`。**ネイティブのグラフィックス API
リソースなので手動解放が必須** (`Release()` / `Dispose()`、IDisposable)。
`ComputeBufferType` は Default (HLSL の `StructuredBuffer<T>` / `RWStructuredBuffer<T>`) /
Structured / Raw / Append / Counter / Constant。
出典: https://docs.unity3d.com/ScriptReference/ComputeBuffer.html

→ ここでも**バインドは文字列名** (`SetBuffer(kernel, "_Particles", buf)`)。
`register(u0)` を作者が書く必要はない。

### 3.2 Unreal — プラグインからグローバルシェーダを足す

1. 空プラグイン (例 `Foo`) を作り `Plugins/Foo/Shaders/Private/` を作る
2. `.uplugin` に `"LoadingPhase" : "PostConfigInit"`。外すと
   「Shader type was loaded after engine init」の assert で死ぬ
3. `Shaders/Private/MyShader.usf` に `#include "/Engine/Public/Platform.ush"` とエントリ関数
4. C++ で `FGlobalShader` 継承 + `DECLARE_EXPORTED_SHADER_TYPE(...)` + `ShouldCache(EShaderPlatform)`
5. 仮想パス付きマクロ登録:
```cpp
IMPLEMENT_SHADER_TYPE(, FLensDistortionUVGenerationVS, TEXT("/Plugin/Foo/Private/MyShader.usf"), TEXT("MainVS"), SF_Vertex)
IMPLEMENT_SHADER_TYPE(, FLensDistortionUVGenerationPS, TEXT("/Plugin/Foo/Private/MyShader.usf"), TEXT("MainPS"), SF_Pixel)
```
6. `Foo.Build.cs` に `"RenderCore"`, `"RHI"` を追加

仮想パス `/Plugin/Foo/...` は `AddShaderSourceDirectoryMapping(virtualDir, realDir)` で成立。
`.usf` = Unreal Shader Format、`.ush` = Unreal Shader Header。
出典: https://dev.epicgames.com/documentation/unreal-engine/creating-a-new-global-shader-as-a-plugin-in-unreal-engine

**Niagara Simulation Stages** [概念は二次]: GPU エミッタで Emitter Update と Particle Update の
間に追加実行パスを挿す。`Iteration Source` で Particles / Data Interface を選び 1 フレームに複数回反復。
ロジックは Custom HLSL ノードや Scratch Pad モジュールで書く。

---

## 4. 設計上の 4 つの問いへの回答

### Q1. パラメータ宣言記法と、エンジンが UI を作る仕組み
- **Unity**: シェーダファイル自身に DSL (`Properties { }`) で書かせ、エンジンが**テキストをパースして**
  `MaterialProperty[]` を作り、型でウィジェットを決め、`[...]` 属性で修飾。
  **宣言 = UI 定義 = シリアライズスキーマ**が 1 か所に同居している。
- **Unreal**: グラフ中のパラメータノードがそのまま宣言。**名前を付けた瞬間に公開**。
  型はノード種別。パースではなくアセット内のノード列挙。

共通点: **作者は「型と名前と既定値」しか書かない。C++ 側のリフレクションコードを一切書かせない。**

### Q2. 可変長のマテリアル定数バッファ
- **Unity(SRP)**: 固定レイアウトではなく**シェーダごとに作者が宣言**。ただし置き場所は
  `UnityPerMaterial` 1 か所に固定。エンジン供給値は `UnityPerDraw` に分離。
  同一シェーダの全 Pass が同じレイアウトでないと SRP Batcher 非互換。
- **Unreal**: 作者はバッファを一切書かない。コンパイル時にマテリアル固有のユニフォームバッファへ
  **生成**され、マテリアルインスタンスは「値だけ」差し替える (だから再コンパイル不要)。
  静的スイッチだけレイアウトを変えるので新パーミュテーション。
  **UE は「レイアウト変更 = 再コンパイル」を正面から認めている**。
- 補足: 「MID を作ると描画がまとまらない」問題に UE は **Custom Primitive Data**
  (プリミティブごとの float ペイロード) という別経路を用意している。

### Q3. テクスチャスロットの割り当て規則
- **Unity**: 作者はレジスタ番号を書かない。**名前一致**でバインド。エンジン予約は
  「名前空間による分離」(`unity_*` / `_WorldSpaceCameraPos` / `_Time` 等)。
  URP は `TEXTURE2D(_BaseMap)` + `SAMPLER(sampler_BaseMap)` で、サンプラ名も
  `sampler` + テクスチャ名という**命名規約**で結びつける。実レジスタ割り当ては HLSL コンパイラ任せ。
- **Unreal**: エンジン側テクスチャ (GBuffer / ライトマップ / シャドウ) はエンジンが握り、
  ユーザーのテクスチャは残りに詰める。上限は 16 サンプラ。逃げ道が **Shared Sampler**
  (`sampler_source` でグローバルサンプラを選ぶ) で、「グローバルサンプラはサンプラスロットを
  消費しない」。代わりに clamp 固定。[一部二次]

★両者とも「**ユーザーにレジスタ番号を握らせない**」ことでエンジン予約との衝突を構造的に消している。

### Q4. コンパイル失敗時のフォールバックとホットリロード

| | Unity | Unreal |
|---|---|---|
| コンパイル失敗 | **マゼンタの error shader**。エディタでもビルドでも同じ | マテリアルをコンパイルエラー表示、描画は既定マテリアルにフォールバック |
| コンパイル中 | **シアンの loading shader** | PSO 未完了時に「描画スキップ」か「既定マテリアル」を選択 |
| 差し替え | `BatchRendererGroup.SetErrorMaterial` / `SetLoadingMaterial` | — |
| ホットリロード | アセット変更検知 → 該当バリアントだけ再コンパイル (`Library/ShaderCache` 照合) | `r.ShaderDevelopmentMode=1` + Ctrl+Shift+.。結果は DDC に入力ハッシュキーで格納 |

★3 点が両エンジンで一致: 「派手な色で**必ず目に見える失敗**にする」「失敗しても落とさない」
「キャッシュキーはソースを含む全入力のハッシュ」。

---

## 5. MyEngine への所見 (調査者の提案 — planner が採否を判断すること)

1. **一番効くのは「Properties 相当の宣言を .hlsl 側に置き、エンジンがパースして Inspector を作る」構造**。
   現状の `ShaderManager` は名前 → `ShaderProgram` の解決までしかやっておらず、パラメータ宣言が
   `.mat.json` 側にしか無い。Unity 方式 (シェーダが真値、マテリアルは値だけ) にすると
   「シェーダを足したら Inspector が生える」が成立する。
2. **パース対象は HLSL 本体でなく先頭のコメント DSL か隣の `.meta`** (既に `guid/type/version` を持つ)
   に置くのが安全。D3DCompile の前処理を汚さず、既存の `.hlsl.meta` 基盤を拡張できる。
3. **UI 属性 (`[Range]` `[Toggle]` `[HDR]` `[NoScaleOffset]`) は費用対効果が非常に高い**。
   ImGui で Slider/Checkbox/ColorEdit4 に振り分けるだけでインスペクタが自動生成される。
4. **定数バッファは Unity の `UnityPerMaterial` 方式が DX11 に素直**。マテリアル用 cb スロットを
   1 本予約して丸ごと載せる。可変長は「シェーダごとにレイアウト生成、`.mat.json` は名前→値の辞書」で、
   `ID3D11ShaderReflection` からオフセットを引けば C++ 側は 1 本で済む。
   UE 方式 (エンジンが生成) は D3DCompile 前のコード生成が要るので重い。
5. **テクスチャは名前バインド + 予約プレフィックスで衝突回避**。
   `ID3D11ShaderReflection::GetResourceBindingDesc` で `t*` の名前とスロットを引き、
   エンジン予約名を除いた残りをユーザースロットとして `.mat.json` に露出する。
   作者に `register(t5)` を書かせない規律は、後でエンジンがスロットを増やしても壊れない点で決定的。
6. **MyEngine には既に強い資産が 2 つある**: (a) include 依存グラフ付きホットリロード +
   「失敗時は旧プログラム維持」、(b) バイトコードキャッシュ。Unity の `Library/ShaderCache` と
   UE の DDC と同じ思想でそのまま使える。足りないのは**失敗の可視化**だけ。
   現状はログのみ (しかも描画スキップで物が消える) なので、
   「マテリアルが無効ならマゼンタ固定色シェーダ」を 1 本入れると事故が即見える。
7. **プロジェクト側シェーダルートの優先度上書きは既に Unity/UE より素直**。
   UE の `AddShaderSourceDirectoryMapping` + 仮想パスに相当するものを、より単純な探索順で実現できている。
   ここは変えないほうがよい。
8. **効かないもの: バリアント/キーワードシステム**。`multi_compile` 相当を入れると組合せ爆発と
   ビルド時 strip が必要になり、**Debug/Release/WARP のビット一致という最大の制約と真っ向からぶつかる**
   (どのバリアントが選ばれるかが実行環境で変わると再現性が割れる)。当面は `#define` 固定 + 別ファイルで足りる。
9. **効かないもの: Deferred/Forward の宣言的パス選択 (URP の LightMode)**。
   MyEngine は `deferred_gbuffer.hlsl` / `forward_lit.hlsl` が固定の対で存在するので、
   ユーザーが任意 Pass を足せる設計より「ユーザーシェーダは GBuffer 書き込み規約に従う」という
   **URP の GBuffer レイアウト固定方式**を明文化するほうが安い。
10. **ポストエフェクトは URP の三層より UE の二軸 (Blendable Location + Priority) が MyEngine 規模に合う**。
    既存 `PostProcess.cpp` に「挿入点 enum + priority 整数」を持つユーザーパス配列を足すだけで
    順序が決定論的に決まり、リプレイ検証とも整合する。コンピュートは Unity 方式
    (`#pragma kernel` 相当 + 名前バインド + 明示 Release) が既存の `froxel_*.cs.hlsl` の作法と揃う。

---

## 取得できなかった / 二次情報で補った箇所
- UE「Simulation Stages」本文 (公式が目次のみ。概念は二次情報と Niagara API ドキュメントで補完)
- UE「Custom Primitive Data」本文 (目次のみ。存在と位置づけのみ記載)
- UE「Understanding Shader Permutations」(本文取得不可)
- Unity `shader_feature` の strip 挙動とキーワード上限の正確な数値 (一次ページが 404)
- UE のサンプラ上限 16 の一次確認
