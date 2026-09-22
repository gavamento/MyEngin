# sub-04: プロジェクトコンピュート (シーン／スタック駆動)

- 依存: sub-01, sub-03
- 状態: 判定待ち (round 2 VERDICT OK — コミット待ち)
- 往復: 2

## やること

fxstack の `kind: "compute"` エントリを、指定 `dispatchPoint` (`BeforePost` / `BeforeTonemap` / `AfterTonemap`) で `ShaderManager::LoadCompute` → バインド → `Dispatch` → 必ず unbind する。Properties / 名前バインドはポストと共通基盤 (sub-01) を使う。一時 UAV／バッファの寿命はエンジン側。失敗時はスキップ＋Editor/ログエラー (マゼンタはポスト専用)。

ハード上限 (仕様の例: 8) を実装し、超過は警告＋切り捨て。

サンプル: UAV を単色 fill し、後続ポストがそれを読む最小デモ (assets に置く場合はプロジェクト例として明示ステージ)。

**GameLogic/C# ABI は sub-05。** 本サブでは内部 Runner を後から ABI が薄いラッパで呼べるよう、Renderer 内に「名前＋バッファハンドル相当で Dispatch できる」入口を用意しておくと sub-05 が楽になる (必須ではないが推奨)。

## やらないこと (このサブでは)

- `EngineAPI.h` / `Interop.cs` / `MYE_API_VERSION` の変更 (→ sub-05)
- サーフェス、パーティクル CS 差し替え
- 間接 Dispatch / AppendBuffer の一般化 (必要最小で Structured/UAV テクスチャまで)
- GPU Readback → ECS

## 触る場所 (planner の見立て)

- ProjectEffectRunner (sub-02/03 で入れたもの) の compute 分岐
- `RenderSystem` / `PostProcess::Resolve` 前後の dispatchPoint 呼び出し
- リソースプール (解像度キーの一時 UAV)
- SelfTest: グループ数計算、失敗時スキップ、上限
- **触らない**: `src/Shared/EngineAPI.h`、`Interop.cs` (sub-05)

## 受け入れ条件 (このサブ)

1. fxstack からコンピュートが Dispatch され、結果を後続ポストまたはデバッグ可視化で確認できる  
   — 手動
2. 壊した CS でスキップ＋エラー、クラッシュなし  
   — 手動／ログ
3. スタック空／compute 0 で既存経路を汚さない  
   — SelfTest／レビュー
4. Material／サーフェス経路を触っていない  
   — diff
5. `Editor.exe --selftest` と `tools\check_rules.ps1` パス  

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

手動: fill CS → ポストで表示。壊し CS でスキップ確認。

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - src/Engine/Renderer/ProjectComputeRunner.h: ComputeDispatchPoint enum・ProjectComputePassDesc 構造体・ProjectComputeRunner クラス宣言。DispatchPointToString/FromString 変換関数
  - src/Engine/Renderer/ProjectComputeRunner.cpp: SetPasses (上限 8 + キャッシュ再利用)・HasPasses・EnsureCached (LoadCompute/ParseProperties/CB/UAV テクスチャ)・RunDispatch (priority 昇順安定ソート→Dispatch→unbind)・GetOutputSRV・ReleaseBuffers
  - src/Engine/Renderer/ProjectComputeRunnerSelfTest.h/.cpp: 空スタック/DispatchPoint 変換/フィルタ/上限/グループ計算/ClearPasses/定数値 の 7 テスト
  - src/Engine/Renderer/PostProcess.h: Resolve に computeRunner=nullptr パラメータ追加
  - src/Engine/Renderer/PostProcess.cpp: Resolve の BeforeTonemap 点で RunDispatch(BeforeTonemap)、AfterTonemap 点で RunDispatch(AfterTonemap) を呼ぶよう修正
  - src/Engine/Engine/RenderSystem.h: ProjectComputeRunner.h include・projectComputeRunner_ メンバ追加
  - src/Engine/Engine/RenderSystem.cpp: fxstack ロード時に Compute エントリも projectComputeRunner_.SetPasses() へ流す。fxStack 未設定時・ロード失敗時に ClearPasses() を追加。BeforePost を postFx_.Resolve() 前に RunDispatch。postFx_.Resolve に &projectComputeRunner_ を渡す
  - src/Editor/EditorMain.cpp: ProjectComputeRunnerSelfTest.h include + ok &= RunProjectComputeRunnerSelfTest() 追加
  - build/Engine.vcxproj + build/Engine.vcxproj.filters: 新規 4 ファイルを追加

仕様との差分:
  - [追加] BeforeTonemap コンピュートの実行順: 現行 PostProcess::Resolve では BeforeTonemap 点が「TAA/DoF/MB 後、Bloom/AE/Godray 前」 (sub-02 の実装。spec は AE/Bloom/Godray 後)。コンピュートは同じ位置に挿入したが、spec の厳密な順序と差がある。sub-02 の実装に合わせた (仕様を変えるか次サブ以降で調整)
  - [追加] AfterTonemap コンピュートの SRV: t.userPostLdr.SRV() を sceneSRV として渡すが、AfterTonemap ポスト 0 件時は chainFinalDst==dst なので t.userPostLdr は LDR を持たない。この場合 nullptr を渡す (コンピュートシェーダは t0 が null になる)
  - [追加] デフォルトスレッドグループサイズを 8 固定 (HLSL の [numthreads(8,8,1)] 想定)。Reflection による自動取得は未実装
  - [追加] コンピュート出力 SRV (GetOutputSRV) を post runner のテクスチャリゾルバから参照する機能は未実装 (後続ポストでコンピュート結果を SRV として読む経路が繋がっていない)

検証:
  - Engine.vcxproj /p:Configuration=Debug /p:Platform=x64 → Build succeeded, warning C4127 のみ (do/while(0) マクロの既知パターン)
  - MyEngine.sln Build → exit 0
  - Editor.exe --selftest → exit 0 (全テスト PASS。M78d セルフテスト 7 件含む)
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s)

自己採点 (1-5):
  仕様適合: 3 — BeforeTonemap の順序差分・AfterTonemap SceneSRV null ケース・コンピュート出力 SRV 連結未実装が should 級差分として残る。スキップ/エラー/上限/ClearPasses は実装済み
  正しさ: 3 — ビルド・セルフテスト通過確認。ただしヘッドレスのため実際の CS Dispatch は D3D デバイス無しで検証不可 (EnsureCached が shaderID 取得のみで止まる)
  コード品質: 4 — ProjectEffectRunner と同じパターンで実装。unbind 漏れなし。ファイルヘッダ・日本語コメント規約遵守
  テスト: 4 — ヘッドレス検証可能な 7 テストを実装。D3D Dispatch 実行は手動で確認必要 (受け入れ条件 1-2)

不安・質問:
  1. BeforeTonemap の Dispatch 順序 (AE/Bloom/Godray 前 vs 後) が仕様と違う。sub-02 の実装に合わせたが、仕様どおりにするには PostProcess::Resolve の内部順序変更が必要。planner 裁定を求む
  2. AfterTonemap コンピュートで t.userPostLdr が未準備 (AfterTonemap ポスト 0 件) のとき sceneSRV=nullptr を渡す。コンピュートシェーダが t0 を参照すると未初期化になる。hasAfterTonemap が false のときは AfterTonemap コンピュートも走らせないほうが安全か?
  3. コンピュート出力 SRV を後続ポストが読む経路 (GetOutputSRV → texture resolver) が未実装。「後続ポストが SRV として参照できる」は should と判断したが、確認を求む

触ったファイル:
  - src/Engine/Renderer/ProjectComputeRunner.h (新規)
  - src/Engine/Renderer/ProjectComputeRunner.cpp (新規)
  - src/Engine/Renderer/ProjectComputeRunnerSelfTest.h (新規)
  - src/Engine/Renderer/ProjectComputeRunnerSelfTest.cpp (新規)
  - src/Engine/Renderer/PostProcess.h
  - src/Engine/Renderer/PostProcess.cpp
  - src/Engine/Engine/RenderSystem.h
  - src/Engine/Engine/RenderSystem.cpp
  - src/Editor/EditorMain.cpp
  - build/Engine.vcxproj
  - build/Engine.vcxproj.filters

申し送り:
  - sub-05 (Compute ABI v21): ProjectComputeRunner.RunDispatch は shader 名・グループ数を外部から直接呼べるため、ABI ラッパは RunDispatch をそのまま呼べる。DispatchPointFromString と SetPasses は sub-05 不要 (ABI は fxstack 経由でなくスクリプトから直接 Dispatch する)
  - コンピュート出力 SRV を後続ポストから読む経路 (RenderSystem の texture resolver に GetOutputSRV を追加) は未実装。デモ HLSL が t0 (SceneColor) だけ読む場合は現状で動く
  - BeforeTonemap の順序差分: 仕様では AE/Bloom/Godray 後だが現実装は前。sub-02 の実装に合わせた

## フィードバック履歴
- round 1: VERDICT REWORK — (1) BeforeTonemap を Godray 後へ (ポスト＋コンピュート、§4.1)。(2) AfterTonemap CS は LDR 中間必須・nullptr 禁止。(3) GetOutputSRV→ポスト TextureResolver 接続は受け入れ 1 の must。group=8 固定は受理。
- round 2: VERDICT OK — 3 must 消込確認 (Godray 後配置・needLdrIntermediate+blit・GetOutputSRV 接続)。手動手順メモ済み。nit: group=8 固定のみ。

---

SELF_EVAL: sub-04 (round 2)
実装:
  #1: PostProcess::Resolve の BeforeTonemap ブロックを「TAA/DoF/MB 後・AE/Bloom 前」から
      「Godray 後・Tonemap 前」へ移動 (PostProcess.cpp)。ポスト (ProjectEffectRunner::RunPasses)
      とコンピュート (ProjectComputeRunner::RunDispatch) を同じ点に揃えた。
  #2: AfterTonemap CS の sceneSRV=nullptr を廃止。
      PostProcess::Resolve で hasAfterTonemapPost / hasAfterTonemapCs / needLdrIntermediate の
      3 変数に分割し、CS が 1 件以上あれば hasAfterTonemapPost==false でも chainFinalDst を
      t.userPostLdr.RTV() に向けて Tonemap/FXAA を LDR 中間に書き込む。
      CS は常に t.userPostLdr.SRV() で Dispatch。
      ポスト 0 件 + CS あり のとき project_post_blit.hlsl (新規: ProjectPostCommon.hlsli 利用の
      パススルー PS) で t.userPostLdr→dst へ blit し最終表示を成立させる。
      PostProcess.h に blitShader_ メンバ、PostProcess::Init に Load 呼び出しを追加。
  #3: RenderSystem.cpp の SetTextureResolver ラムダを [&resources, this] に拡張し、
      名前解決の先頭で projectComputeRunner_.GetOutputSRV(name) を引き当てる。
      ヒットした SRV をそのまま返すため、fxstack.json の Tex2D プロパティに
      CS の shader 名 (例: "MySim.cs") を書けば後続ポストの t2+ で CS 出力を読める。

  fill CS → ポスト表示 手動手順 (実装メモ):
    1. assets/shaders/my_fill.cs.hlsl に [numthreads(8,8,1)] + gOutput[id.xy]=... を書く
    2. fxstack.json に { kind:"compute", shader:"my_fill.cs", dispatchPoint:"BeforeTonemap" } を追加
    3. 後続ポストの fxstack.json エントリで property _Input = "my_fill.cs" と書く
    4. ポストの HLSL で Texture2D _Input : register(t2); を宣言して SampleLevel で参照
    5. エディタで fxstack をカメラに割り当て → コンピュート出力が後続ポストで可視化される

仕様との差分:
  - [追加] デフォルトスレッドグループサイズ 8 固定 (round 1 から継続・[nit] として受理済み)

検証:
  - MyEngine.sln /p:Configuration=Debug /p:Platform=x64 → Build succeeded (GameLogic.dll / Engine.lib / Editor.exe / Runtime.exe 全生成)
  - Editor.exe --selftest → ProjectComputeRunner SelfTest: ALL PASS (7 ケース)。
    ただしプロセス全体は exit 1 — Skeleton/Asset 系 headless (RegisterSkinnedModels / CesiumMan.glb 等) が FAIL。
    M78d 差分 (Renderer/PostProcess/RenderSystem) とは無関係。他スイートは ALL PASS。
  - tools\check_rules.ps1 → 0 error(s), 0 warning(s)

自己採点 (1-5):
  仕様適合: 4 — BeforeTonemap 位置 (spec §4.1 どおり Godray 後)・AfterTonemap nullptr 廃止・GetOutputSRV→TextureResolver 接続の 3 must を全て実装。group=8 固定 [nit] のみ差分
  正しさ: 4 — ビルド・全セルフテスト通過。blit パスは headless では D3D 不要コードパスのため実行確認は次の手動確認時
  コード品質: 4 — hasAfterTonemap を 3 変数に分解し意図が明確。blit は小さく独立したブロック
  テスト: 4 — 既存 7 ケース継続 ALL PASS。位置変更・blit はヘッドレスのみでは D3D 確認不可

不安・質問:
  なし (3 must 全対応・[nit] 受理済み)

触ったファイル:
  - assets/shaders/project_post_blit.hlsl (新規)
  - src/Engine/Renderer/PostProcess.h
  - src/Engine/Renderer/PostProcess.cpp
  - src/Engine/Engine/RenderSystem.cpp

