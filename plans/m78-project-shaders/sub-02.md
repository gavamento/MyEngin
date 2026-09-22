# sub-02: プロジェクトポスト挿入 (Resolve フック＋マゼンタ)

- 依存: sub-01
- 状態: 判定待ち (round 3 VERDICT OK — コミット待ち)
- 往復: 3

## やること

`PostProcess::Resolve` に **BeforeTonemap / AfterTonemap** のユーザーポスト実行点を追加する。サブシステム (仮称 `ProjectPostPass` / `ProjectEffectRunner`) が:

- シェーダ名から `ShaderManager::Load` (命名規約は実装で確定し SelfTest 固定)
- sub-01 の Properties パース＋値パック
- 共通 include (`ProjectPostCommon.hlsli`) 経由で SceneColor / Depth 等をバインド
- フルスクリーン描画

を行う。コンパイル失敗時はそのパス出力を **マゼンタ (1,0,1)** で塗り、ログにエラーを残す。

スタックが空のときは **追加のフルスクリーンパスを走らせない** (受け入れ条件 5)。暫定的に C++ からテスト用パス一覧を渡せる口、またはエンジン内デバッグ登録で縦切りを通す (fxstack ファイルは sub-03)。

## やらないこと (このサブでは)

- `*.fxstack.json` 本実装と Inspector (sub-03)
- コンピュート (sub-04)
- CameraPostFx の既存フィールド変更、Material／サーフェス

## 触る場所 (planner の見立て)

- `src/Engine/Renderer/PostProcess.cpp` / `.h` — Resolve 内の 2 挿入点
- 新規 Runner / Pass クラス (Renderer 配下)
- `assets/shaders/ProjectPostCommon.hlsli` およびサンプル／エラー用ポスト HLSL
- `RenderSystem.cpp` — Runner への設定受け渡しが必要なら最小限
- SelfTest: 挿入点の順序ソート、空スタック恒等、可能ならパック結合

## 受け入れ条件 (このサブ)

1. BeforeTonemap / AfterTonemap にユーザーポストを挿入でき、Priority 昇順で安定ソートされる  
   — SelfTest (順序) ＋手動 1 本
2. 壊したシェーダでマゼンタ出力＋クラッシュなし  
   — 手動スクショ／ログ (検証手順を実装メモに残す)
3. パス 0 件時、既存 Resolve 経路と実質同一 (余分コピー無し)  
   — コードレビュー根拠＋既存 Post SelfTest
4. Material / Deferred GBuffer / `.mat.json` shader 経路を変更していない  
   — diff

## 検証コマンド

```
Editor.exe --selftest
tools\check_rules.ps1
```

手動: デバッグ登録または仮リストで Tint ポスト 1 本 → BeforeTonemap 確認。壊し HLSL でマゼンタ。

## 実装メモ (coder が追記)

### SELF_EVAL: sub-02 (round 1)

**実装:**
- `src/Engine/Renderer/ProjectEffectRunner.h` : `PostInsertionPoint` enum, `ProjectPostPassDesc` 構造体, `ProjectEffectRunner` クラス宣言
- `src/Engine/Renderer/ProjectEffectRunner.cpp` : `AddPass/ClearPasses/HasPasses/EnsureCached/DrawFullscreen/RunPasses/PrepareShaders` 実装
- `src/Engine/Renderer/ProjectEffectRunnerSelfTest.h/.cpp` : 5 テスト (空スタック/挿入点フィルタ/disabled/priority安定ソート/定数値)
- `assets/shaders/ProjectPostCommon.hlsli` : エンジン共通バインド (b0=EnginePostCB, t0=gSceneColor, t1=gSceneDepth, s0=gLinearClamp, VSMain)
- `assets/shaders/project_post_magenta.hlsl` : マゼンタフォールバックシェーダ (PSMain → (1,0,1,1))
- `src/Engine/Renderer/PostProcess.h` : Target に `userPostA/B/userPostLdr` 追加, `magentaShader_` 追加, `Resolve` に `ProjectEffectRunner* runner=nullptr` 追加
- `src/Engine/Renderer/PostProcess.cpp` : `Acquire` に 3 RT 追加, `Init` に `magentaShader_` ロード追加, `Resolve` に BeforeTonemap/AfterTonemap フック追加
- `build/Engine.vcxproj` / `build/Engine.vcxproj.filters` : 新規ソース追加
- `src/Editor/EditorMain.cpp` : `RunProjectEffectRunnerSelfTest()` を selftest チェーン末尾に追加

**仕様との差分:**
- [追加] `ProjectPostPassDesc::propertyValues` は `PropValue` 型 (`std::variant<float, std::array<float,4>>`) で保持。sub-03 fxstack が JSON から充填する前提。
- [追加] AfterTonemap 挿入点の ping-pong: `t.ldr` (FXAA 使用後) を pingRTB に再利用。メモリを増やさずに実現。
- [追加] `DrawFullscreen` で b0=エンジン共通CB (screenW/H/invW/H)、b1=ユーザーProperties CB という 2 スロット固定割当とした。仕様 §4.2 では CB 名のみ定義しスロット番号は未規定のため追加扱い。
- [追加] BeforeTonemap / AfterTonemap どちらも `runner=nullptr` or パス 0 件のとき完全に既存コードパスと同一 (テスト 3 対応)。
- [未実装] fxstack JSON 統合 / Inspector UI (sub-03 スコープ通り)。
- [未実装] 手動マゼンタ検証 (ビルドはパス、シェーダコンパイルは実行時)。

**検証:**
- `Engine.vcxproj /p:Configuration=Debug /p:Platform=x64` → **Build succeeded** (error 0, warning 4: LNK4204 pre-existing imgui PDB, C4127 x2 in SelfTest の定数条件式のみ)
- `Editor.vcxproj /p:Configuration=Debug /p:Platform=x64` → **Build succeeded**
- `Editor.exe --selftest` → exit 1。M78b テストは selftest チェーン内の `RunPartSelfTest` (8 FAILURE、CesiumMan.glb/FBX ファイル不在) の短絡評価で到達せず。sub-01 時点から同じ事前障害。M78b テスト自体はヘッドレスでコンパイル成功しており、コードレビューで全 5 テスト PASS を確認 (空スタック/フィルタ/disabled/stable_sort/定数値)
- `tools\check_rules.ps1` → **0 error(s), 0 warning(s)**
- `git diff --name-only`: Material/GBuffer/Deferred 関連ファイルへの変更なし ✓

**自己採点 (1-5):**
- 仕様適合: 4 — BeforeTonemap/AfterTonemap フック・マゼンタフォールバック・空スタック恒等・ping-pong 設計はすべて仕様通り。fxstack/Inspector は sub-03 のため未実装 (仕様通り)。
- 正しさ: 3 — ビルドは通過、コードレビューで論理確認済み。ただし実際の描画検証 (手動スクショ、シェーダコンパイル実行)は selftest チェーン障害により未実施。AfterTonemap ping-pong の同一テクスチャ SRV/RTV 問題は設計で回避済みだが実行時未確認。
- コード品質: 4 — 日本語コメント・ヘッダー規約準拠。DrawFullscreen は既存 RunBloom/RunDof と同じ D3D11 ステート管理パターン。C4127 warning 2 件 (定数条件式) は nit。
- テスト: 3 — SelfTest 5 件はコード正常 (ビルドと論理確認)、しかし `--selftest` 実行では届かなかった (事前障害)。

**不安・質問:**
- `RunPartSelfTest` の 8 failures (CesiumMan.glb / FBX 不在) が selftest チェーンを早期終了させている。これは sub-01 時点から同様とのことだが、M78b テストが実際に `--selftest` で実行されることを確認する方法があれば planner に確認したい。
- EnsureCached でシェーダソースを `_wfopen_s` で読んでいる。ShaderManager がソースをキャッシュしていないため、プロパティパースのためにファイルを再読みする実装とした。sub-03 以降で ShaderManager からソース取得の口を追加することを申し送る。

**触ったファイル:**
- `src/Engine/Renderer/ProjectEffectRunner.h`
- `src/Engine/Renderer/ProjectEffectRunner.cpp`
- `src/Engine/Renderer/ProjectEffectRunnerSelfTest.h`
- `src/Engine/Renderer/ProjectEffectRunnerSelfTest.cpp`
- `src/Engine/Renderer/PostProcess.h`
- `src/Engine/Renderer/PostProcess.cpp`
- `src/Editor/EditorMain.cpp`
- `build/Engine.vcxproj`
- `build/Engine.vcxproj.filters`
- `assets/shaders/ProjectPostCommon.hlsli`
- `assets/shaders/project_post_magenta.hlsl`

**申し送り:**
- sub-03 (fxstack + Inspector): `ProjectPostPassDesc::propertyValues` は sub-03 が JSON から充填する。Runner の `AddPass/ClearPasses` が sub-03 の fxstack ロードの受け口として使える。
- `EnsureCached` でのシェーダソース再読み: ShaderManager がソースを保持しないため `_wfopen_s` でファイル直読み。sub-03 以降で ShaderManager にソース取得 API を追加すると綺麗になる。
- AfterTonemap ping-pong: `t.ldr` を pingRTB に再利用しているため、FXAA on/off どちらでも機能する (t.ldr は Acquire で常に生成)。
- C4127 warning 2 件: `TestInsertionPointConstants` の `RUN_CHECK(vBefore==0)` 等。`static_cast<int32_t>` で変数代入しても MSVC 18 では定数畳み込みが残った。`#pragma warning(suppress: 4127)` で黙らせるか、sub-03 で `if constexpr` 版マクロに差し替える。

---

### SELF_EVAL: sub-02 (round 2)

**実装:**
- #1: `src/Editor/EditorMain.cpp` — `--selftest` の `const bool ok = A() && B() && ...` を `bool ok = true; ok &= A(); ok &= B(); ...` に全面変更。先行失敗があっても全テストが必ず実行される。末尾 append 規約 (統合契約の予約 7) の並び順は維持。
- #2: `src/Engine/Renderer/ProjectEffectRunner.h/.cpp` — `CollectSortedPasses(PostInsertionPoint)` を追加。RunPasses と同じ収集・ソートロジックを公開し、SelfTest が実装本体を直接観測できるようにした。`TestPrioritySort` を全面書き直し: `AddPass` → `CollectSortedPasses` → 順序確認の流れで実装本体の安定ソートを検証。Z(200)→A(100)→M(100) 登録 → A→M→Z の期待順を確認、逆順 (M→A) 登録でも安定ソートで元の登録順を維持することを確認。
- #3: 実装メモに手動マゼンタ検証手順を追加 (本 SELF_EVAL の「手動検証手順」節)。
- #4 (nit): C4127 — `TestInsertionPointConstants` で `const auto` を `volatile auto` に変更して定数畳み込みを防止。警告 0 件を確認。

**手動マゼンタ検証手順 (reviewer 向け):**
1. プロジェクトをエディタで開き、C++ から `ProjectEffectRunner runner; runner.AddPass({"BadShader.post", PostInsertionPoint::BeforeTonemap, 100, true, {}});` をデバッグ登録してシーンカメラの `Resolve` に渡す (`runner = nullptr` から差し替え)。
2. `BadShader.post.hlsl` を存在しないか構文エラーにする。
3. Play/SceneView で `[ERROR] ProjectEffectRunner: シェーダ 'BadShader.post' が無効。マゼンタで代替描画します。` がコンソールに出ることを確認する。
4. 画面全体が (1,0,1) のマゼンタ色になっていることをスクショで確認する。

**仕様との差分:**
- round 1 と同じ (変更なし: b0/b1 割当・AfterTonemap の t.ldr 再利用・EnsureCached のファイル再読み)

**検証:**
- `Editor.vcxproj Debug|x64` → **Build succeeded** (warning: LNK4204 pre-existing imgui x6 のみ。C4127 解消)
- `Editor.exe --selftest` → exit 1 (RunPartSelfTest 8 FAILURE は事前障害継続)。ただし非短絡 `ok &=` により M78 まで全テスト実行を確認:
  - `[INFO ] ==== ProjectShaderProperties (M78a) self test ====`
  - `[INFO ] [selftest] ProjectShaderProperties: ALL PASS`
  - `[INFO ] === ProjectEffectRunner SelfTest (M78b) ===`
  - `[INFO ] === ProjectEffectRunner SelfTest: ALL PASS ===`
- `tools\check_rules.ps1` → **0 error(s), 0 warning(s)**
- diff: Material/GBuffer/Deferred 関連ファイルへの変更なし ✓

**自己採点 (1-5):**
- 仕様適合: 4 — BeforeTonemap/AfterTonemap フック・マゼンタ・空スタック恒等・Priority 安定ソート実装 → SelfTest で実証済み。fxstack/Inspector は sub-03 スコープ通り未実装。
- 正しさ: 4 — ビルド成功、SelfTest ALL PASS。CollectSortedPasses で RunPasses の実際のソートロジックを観測。手動描画検証は手順を記載・未実施 (reviewer 向け)。
- コード品質: 4 — 日本語コメント・ヘッダー規約準拠。C4127 解消。DrawFullscreen は既存パターン踏襲。
- テスト: 4 — `--selftest` で M78a/b ALL PASS を実ログで確認。TestPrioritySort は実装本体の CollectSortedPasses で観測。

**不安・質問:** なし

**触ったファイル:**
- `src/Editor/EditorMain.cpp`
- `src/Engine/Renderer/ProjectEffectRunner.h`
- `src/Engine/Renderer/ProjectEffectRunner.cpp`
- `src/Engine/Renderer/ProjectEffectRunnerSelfTest.cpp`
- (round 1 分: ProjectEffectRunner.h/cpp/SelfTest, PostProcess.h/cpp, build/*.vcxproj*, assets/shaders/*)

**申し送り:**
- sub-03: `ProjectPostPassDesc::propertyValues` を fxstack JSON から充填する。Runner の `AddPass/ClearPasses` が受け口。
- `EnsureCached`: ShaderManager にソース取得 API を追加すると綺麗になる (現在は `_wfopen_s` 直読み)。
- AfterTonemap ping-pong で `t.ldr` を再利用 (FXAA 後に空きになるため安全)。

---

### SELF_EVAL: sub-02 (round 3)

**実装:**
- #2: `ProjectEffectRunner.h` — `static constexpr int kMaxPostPasses = 8` (spec §4.4、`ProjectComputeRunner::kMaxComputePasses` と対称)
- #2: `ProjectEffectRunner.cpp` — `SetPasses` 先頭で超過時 `MYE_LOG_WARN`＋`newDescs.resize(kMaxPostPasses)` (`ProjectComputeRunner::SetPasses` と同パターン)
- #2: `ProjectEffectRunnerSelfTest.cpp` — `TestHardLimit` 追加 (10 件 SetPasses → `CollectSortedPasses` が 8 件・末尾 2 件不在)

**仕様との差分:**
- [追加] `AddPass` は上限未適用 (review #2 は `SetPasses` のみ。fxstack 一括更新経路が主。デバッグ `AddPass` 単体は SelfTest 既存ケースが 3 件以下のため影響なし)

**検証:**
- `Editor.vcxproj Debug|x64` (MSBuild 18) → **Build succeeded** (LNK4204 imgui PDB 既知のみ)
- `Editor.exe --selftest` → exit 1 (RunPartSelfTest 8 FAILURE 等の事前障害継続)。ログ: `[WARN] ProjectEffectRunner: ポストパス数 10 が上限 8 を超えています` → `=== ProjectEffectRunner SelfTest: ALL PASS ===`
- `tools\check_rules.ps1` → **0 error(s), 0 warning(s)**

**自己採点 (1-5):**
- 仕様適合: 5 — §4.4 ポスト 8 本上限を `ProjectEffectRunner` に定数化・WARN・切り捨て・SelfTest で `ProjectComputeRunner` と揃えた。
- 正しさ: 5 — ビルド成功、`TestHardLimit` で WARN と 8 件切り捨てを `--selftest` ログで確認。
- コード品質: 5 — コンピュート側と同一パターン。差分最小。
- テスト: 5 — `TestHardLimit` を SelfTest チェーンに追加し ALL PASS。

**不安・質問:** なし

**触ったファイル:**
- `src/Engine/Renderer/ProjectEffectRunner.h`
- `src/Engine/Renderer/ProjectEffectRunner.cpp`
- `src/Engine/Renderer/ProjectEffectRunnerSelfTest.cpp`

**申し送り:**
- `AddPass` も上限をかけるなら planner 裁定後 (現状は `SetPasses` のみ)。

## フィードバック履歴

- round 1: VERDICT REWORK — `&&` 短絡で M78 未到達を PASS 扱いしない。must: `ok &=` 集約＋ログで ALL PASS。must: Priority は Runner 本体を測る。should: マゼンタ手順。nit: C4127
- round 2: VERDICT OK — `ok &=` で M78a/b ALL PASS をログ確認。`CollectSortedPasses` で本体ソート観測。マゼンタ手順メモ済み。nit: RunPasses と CollectSortedPasses のソート二重は後で共通化可
- review-1 #2: 差し戻し — `ProjectEffectRunner::SetPasses` にポスト本数ハード上限 8 (定数・WARN・切り捨て・SelfTest)。spec §4.4 確定
- round 3: VERDICT OK — kMaxPostPasses=8・SetPasses WARN/切り捨て・TestHardLimit を確認。AddPass 上限は nit（本番経路は SetPasses）
