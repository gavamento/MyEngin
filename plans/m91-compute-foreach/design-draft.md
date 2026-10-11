# M91 Compute.ForEach — C++/C# 共通の計算 API — 事前調査 (design-draft)

2026-10-11 作成。/harness の planner へ渡す事前調査。ユーザーとの対話で大前提と 3 つの論点は確定済み。
**この文書は仕様書ではない**。spec.md は planner が書く。

## Context

ユーザー依頼: 「C#/C++ 共通の計算用 API。プログラマーが CPU と GPU を意識せずに処理を書ける API」。
求める機能: CPU/GPU 自動選択、CPU/GPU 強制指定、並列処理、バッファ管理、自動データ転送、非同期実行、
実行結果の取得、GPU 非対応時の代替実行、CPU/GPU 負荷制限、自動 SIMD 最適化。

呼び出しの像 (ユーザー提示):

```csharp
Compute.ForEach(particles, "ParticleUpdate", Execution.Auto);
```

番号は着手時の末尾から取る (2026-10-11 時点: ABI v28 = 164 スロット、TypeId 80、ADR-029、snapshot v48、kCookVersion 6)。
memory では M75h が ABI v29 を予定しているので衝突を確認すること。

## 1. ユーザーとの合意事項 (2026-10-11)

### 1.1 大前提 (必須条件として扱う)

| 項目 | 決定 |
| --- | --- |
| 決定論 | **維持する**。CPU と GPU で結果が変わりうるものは CPU で計算する |
| 重要なゲーム状態 | **CPU で計算する** |
| 自動判断 (`Execution.Auto`) | どちらで走らせても整合性 / 決定論が壊れないことを**確認したうえで**選ぶ。確認できないものは CPU |

### 1.2 論点の決定

| 論点 | 決定 |
| --- | --- |
| C++ と C# | **同じ呼び方**にする (API の形・引数・戻り値を揃える) |
| カーネルの記述 | **HLSL サブセットの制限を受け入れる**。`*.kernel.hlsl` を 1 回書き、GPU (cs_5_0) と CPU (C++) の両方から同じファイルを使う |
| カーネルの置き場 | **`assets/shaders/kernels/`** |
| CPU 版と GPU 版の版ずれ | **カーネルのソースハッシュを DLL に埋め、不一致時は実行を制限する** |
| 呼び出しと設定の分担 | 呼び出しはコード (API)。実行ポリシーと観測はインスペクター。保存先は `project_settings.json` |

補足: インスペクターは **狭める方向だけ** (Auto → CPU 固定は可、State バッファを GPU へ向けるのは不可)。

## 2. すでにある土台 (作り直さないこと)

### 2.1 Compute ABI v21 (M78e) — GPU 側の入口
- `src/Shared/EngineAPI.h:741` 以降の 7 スロット: `CreateComputeBuffer` / `ReleaseComputeBuffer` / `SetComputeBuffer` /
  `SetComputeFloat` / `SetComputeFloat4` / `SetComputeTextureFromAsset` / `DispatchCompute`。
- 実装は `src/Engine/Renderer/Compute/ComputeAbiRunner.{h,cpp}` (`kMaxAbiBuffers = 64`、不透明ハンドル = 世代 32bit + スロット 32bit、
  M88 の `ReleaseGpu` / `RecreateGpu` 対応済み)。
- テーブル充填は `src/Engine/Engine/Script/EngineApiTable.cpp:1313` 付近、C++ 糖衣は `src/Shared/ScriptAPI.h:1291`、
  C# は `src/Scripting/Interop.cs` (位置ミラー) と `MyeScript.cs`。
- **無いもの**: CPU → GPU のアップロード、GPU → CPU の読み戻し (M78 spec §「後回し」に明記)、CPU 実行、非同期ハンドル。
- `EngineAPI.h` の注記: Compute 結果を ECS / WorldHash に書き戻す用法は禁止 (spec §4.1、ADR-022 R-7)。

### 2.2 JobSystem (ADR-028) — CPU 側の並列基盤
- `src/Engine/Core/Jobs/JobSystem.h`: `ParallelRanges(total, grain, fn(begin,end))` / `ParallelFor`。
- 決定性の規約: 連続レンジ分割、縮約は仕事単位の配列に置いて index 順に直列加算、ネスト禁止、**呼び出し元は常に 1 つ**。
- `--no-jobs` で全直列。`kCfgJobs` (`Session/SessionTypes.h:37`) が configBits に入り、ネットは不一致を Reject。
- `tools/replay_verify.bat` の `:job_jobsab` が jobs あり/なしのワールドハッシュ一致で証明する。これが A/B 検証の前例。
- **ABI には露出していない**。GameLogic / C# から使う手段は無い。

### 2.3 シェーダ基盤
- `ShaderManager::LoadCompute("name.cs")` → `name.cs.hlsl`、エントリ `CSMain`、`D3DCompile` (FXC、cs_5_0)。
  フラグは Debug/Release 同一 (`kCompileFlags`、ShaderManager.cpp:742)。
- シェーダルートは優先度順の複数持ち (`<project>/assets/shaders` → `<engine>/assets/shaders`)。
  `RebuildProjectShaderIndex` が `*.cs.hlsl` を短名で索引する。`kernels/` サブディレクトリを索引対象に足す必要がある。
- ホットリロード: `FileWatcher` → `ReloadHub::ReloadShader` → `RequestRecompileForFile` (非同期) → `PollAsyncCompiles`。
- バイトコードキャッシュ `ShaderCache` (中身ハッシュで鮮度判定)。
- `.hlsl` には `.meta` (guid / type: shader) が付く。`*.kernel.hlsl` にも同じ扱いが要る。
- D3D 型は Renderer の外に出さない規則 (AGENTS.md §4.1)。実態の境界は Engine プロジェクトと Shared / GameLogic / C# / Editor の間。
- Slang / ISPC / HLSL→C++ 変換器は**無い**。HLSL を CPU で動かす既存手段は WARP だけ (CI が使用)。

### 2.4 既存の readback
汎用層は無く、各所が STAGING + Map(READ) を個別に書いている。流儀は `OcclusionCuller::PollStats` (3 本のリング、2 フレーム遅れ、
対話中は `DO_NOT_WAIT`、決定的撮影と selftest では待つ)。

### 2.5 C# レーン
- CoreCLR (.NET 8) + Roslyn。`src/Scripting/`、ホストは `src/Engine/Engine/Script/ManagedHost.{h,cpp}`。
- C# レーンは `TickRunner.cpp:339` の条件で **record / verify / networked / resim 中は走らない**。C# のコンポーネントは `kComponentNoHash`。
- C# の `Engine` クラスは internal。公開は `MyeScript` (protected) と `MyeEntity`。
- 配列を受け取る API は Overlap 系 / FindPartsByTag / FindEntitiesWithTag / NavFindPath のみ (呼び出し側がバッファを用意し、戻り値は総数)。
- `Schema.gen.cs` の型付きアクセサは `src/Engine/Engine/Schema/SchemaCodegen.{h,cpp}` が生成する。構造体 codegen の前例。

### 2.6 同じカーネルを 2 回書いている前例
GPU 粒子 `assets/shaders/particle_sim.cs.hlsl` の `CSMain` と、CPU 粒子 `CpuParticleBackend.cpp` の `StepPool`。
ADR-008 の SIMD 規約 (演算列をスカラー参照と一致させる、SIMD 幅に分割を揃える) と `CpuModalBackend` (align=8 でスレッド数非依存) が
CPU 側の決定性の前例。

### 2.7 設定の置き場の前例
`assets/project_settings.json` = `{"particleBackend":"cpu","particleCompareMode":false,...}`。M90h のオクルージョン ON/OFF も同じファイル。
`contentHash` がこのファイルを覆うので、参加者間の不一致を検出できる。

## 3. 設計の骨子 (対話で提示し、ユーザーが受け入れた形)

### 3.1 カーネル = `*.kernel.hlsl` (HLSL サブセット、単一ソース)

```hlsl
// assets/shaders/kernels/ParticleUpdate.kernel.hlsl
struct Particle { float3 pos; float3 vel; float life; };
KERNEL_PARAMS { float dt; float3 gravity; };
KERNEL(ParticleUpdate, RW(Particle) particles)
{
    Particle p = particles[id];
    p.vel = p.vel + params.gravity * params.dt;
    p.pos = p.pos + p.vel * params.dt;
    p.life = p.life - params.dt;
    particles[id] = p;
}
```

- GPU: `KERNEL` が `[numthreads(64,1,1)] void CSMain(uint3 tid : SV_DispatchThreadID)` に展開され、既存の `LoadCompute` でコンパイル。
- CPU: 互換ヘッダ (`float2/3/4` の struct、`RW(T)` / `RO(T)` の span 風型、`saturate` / `lerp` / `rcp` / `min` / `max` などの C++ 版) を
  前置して C++ としてコンパイルし、`JobSystem::ParallelRanges` で `id` を回す。`sin` / `cos` は CRT ではなく既存の `MyeSinRad` 系。
- C#: 構造体を codegen で生成 (`Schema.gen.cs` と同じ流儀)。構造化バッファは 4 バイト境界で詰まるので C++ struct / C# `Sequential` と一致する。
  `KERNEL_PARAMS` だけ cbuffer の 16 バイト規則を吸収する。
- **書けないもの** (受け入れ済み): 並べ替えスウィズル (`v.zyx`)、`groupshared` / barrier / atomic、テクスチャサンプリング (初版)。
  これらが要る処理は既存の `*.cs.hlsl` (フル HLSL、GPU 専用) で書く。
- カーネルが守る約束 (ADR-028 と同じ): 自分の要素だけに書く、他要素の今回の書き込みを読まない、実行順に依存しない、共有 RNG / ログを触らない。

### 3.2 2 段構え

| 段 | ファイル | 書ける範囲 | `Execution.Auto` の候補 |
| --- | --- | --- | --- |
| ForEach カーネル | `*.kernel.hlsl` | HLSL サブセット | GPU / native CPU (Jobs) |
| フル compute (既存 v21) | `*.cs.hlsl` | HLSL 全部 | GPU / WARP (デバイス無しなら不可) |

### 3.3 バッファの種別で実行先を縛る (大前提の実装)

| 種別 | 用途 | 実行先 | ハッシュ / スナップショット |
| --- | --- | --- | --- |
| **State** (既定) | ゲーム状態 | **常に CPU**。`Execution.Gpu` の強制は拒否 (0 戻り + ログ) | **対象** (planner 論点 4) |
| **Fx** (明示オプトイン) | 演出・一時データ | GPU 優先、無ければ CPU (同じカーネル) | 非対象 (NoHash) |

- `Execution.Auto` の「確認」は、(a) 関与するバッファが全部 Fx であること、(b) 負荷枠に収まること、(c) GPU 版と CPU 版のハッシュが一致していること (3.5) の 3 つ。
  1 つでも落ちれば CPU。
- 既定を State (CPU) にするのは、正しさ → 決定性の優先順位に合わせて「安全側に倒す」ため。

### 3.4 API の形 (C++ と C# で同一)

```cpp
auto buf = Compute::CreateBuffer<Particle>(ctx, count, BufferFlags::ReadWrite);            // 既定は State
buf.Write(particles);                                                                        // CPU ミラーへ (dirty)
auto job = Compute::ForEach(ctx, buf, "ParticleUpdate", Execution::Auto, params);
job.Wait();                                                                                   // または次 tick で job.IsDone()
buf.Read(particles);
```

```csharp
var buf = Compute.CreateBuffer<Particle>(count, BufferFlags.ReadWrite);
buf.Write(particles);
var job = Compute.ForEach(buf, "ParticleUpdate", Execution.Auto, params);
job.Wait();
buf.Read(particles);
```

ABI は v21 の 7 本に末尾追加する想定 (Write / Read / RunKernel (実行先・パラメータ付き) / IsJobDone / WaitJob / 負荷枠または統計の取得 で 6 本程度)。
既存スロットの並び・シグネチャは不変。bump の検証は memory `abi-bump-verification.md` + `check_rules` 規則 11。

### 3.5 版ずれの検出と実行制限 (合意 3)
- GameLogic.dll のビルド時に `assets/shaders/kernels/*.kernel.hlsl` のソースハッシュを埋める (生成ファイル経由)。
- 実行時にディスク上のファイルのハッシュと照合し、不一致なら: State バッファ向け ForEach は **失敗** (古い CPU コードを黙って走らせない)、
  Fx バッファ向けは **GPU 固定 + WARN** (GPU 側はホットリロードで新しい)。Editor にエラー表示。
- GameLogic.dll のホットリロードでハッシュが再同期する。

### 3.6 インスペクターと設定
- スクリプトを選ぶと「このスクリプトが呼ぶカーネル一覧、最終実行先 (CPU / GPU / 代替)、要素数、転送量、所要時間」を表示 (観測が主目的)。
- カーネルごとの上書きは Auto → CPU 固定のみ。保存先は `project_settings.json` (contentHash で不一致検出)。
- 負荷枠も `project_settings.json`: GPU を使う最小要素数、CPU のワーカー上限、tick あたりの GPU 転送バイト上限、tick あたりの Dispatch 数上限。
  時間計測ではなく数で縛る (決定的)。
- カーネル一覧を静的に知るために、スクリプト側に `FIELDS` と同じ流儀の宣言 (`COMPUTE_KERNELS("ParticleUpdate", ...)`) を置く案。

## 4. 設計の論点 (planner がユーザーと詰める)

1. **CPU カーネルの住処と呼び出し経路**
   - カーネルはプロジェクト内容 (`assets/`) なので CPU 版は GameLogic.dll に入るのが自然 (ホットリロード対象)。`gen_project_files.ps1` の
     `Update-Vcxproj 'GameLogic'` に生成ファイル (`Kernels.gen.cpp`: 各 `.kernel.hlsl` を互換ヘッダ付きで include + 登録表) を足す。
   - `MyeScriptModule` (`src/Shared/ScriptTypes.h`) にカーネル表 (名前、関数ポインタ、ソースハッシュ) を足す = ABI 変更。
     C# からの ForEach は Engine → ロード済み GameLogic.dll のカーネル表 → 実行、の経路になる。
   - GameLogic.dll が無い (C# だけ) プロジェクトでは CPU 版が存在しない。State バッファは失敗、Fx は GPU / WARP。この制限を受け入れるか。
2. **データの往復と非同期**
   - State (CPU) は同期即時。Fx (GPU) は DX11 の readback がストールするので、`Read` は「Dispatch の 2 フレーム後に取れる」遅延モデルか、
     `Wait` で明示ストールか。既存の `OcclusionCuller::PollStats` のリング流儀に合わせる。
   - ジョブ完了の取り込み時点を tick 境界に固定する (非同期完了順が sim に漏れないように)。
3. **`particles` の正体**: スクリプトが持つ配列 (`Write` / `Read` で往復) を初版とし、ECS コンポーネント列の直接渡しは範囲外にするか。
4. **State バッファの寿命・スナップショット・ハッシュ** ★最重要
   - 「重要なゲーム状態」を State バッファに置くなら、その中身は SimSnapshot / WorldHash / Replay / 分岐 (M72) / ロールバック (M52) の
     対象でなければならない。ADR-003 (スクリプト状態はエンジン側 ECS の POD) と整合させる方法:
     エンジン所有の blob としてエンティティに紐づける、または ForEach の結果を ECS へ書き戻す層を API 側で持つ。
   - C# レーンは NoHash なので、C# が作る State バッファは「CPU 固定・NoHash」(C# レーン全体と同じ扱い) でよいか。
   - シーン遷移・Play 停止での回収は `ReleasePlaySessionEngineState` に倣う。
5. **A/B 検証の設計**: `--compute-force-cpu` を configBits に足し、`replay_verify.bat` に jobs A/B と同じ形の compute A/B を追加する。
   Auto と CPU 固定でワールドハッシュが一致しなければ「GPU 結果が状態に漏れている」と検出できる。
6. **WARP 代替**: デバイス無しの環境で Fx カーネルを WARP に落とすか、単に失敗 (0 戻り) にするか。Server.exe は device-less 設計 (ADR-022)。
7. **自動 SIMD の約束の範囲**: コンパイラの自動ベクトル化頼みになる。エンジンが保証するのは「SIMD 幅に揃えた分割でスレッド数非依存」まで。
   ADR-008 流に手書き SIMD を要求するかは用途次第。`/fp:precise` 維持。
8. **既存 v21 との関係**: 7 本は残し (フル compute 用)、ForEach は別経路にするか、`CreateComputeBuffer` を共通化するか。
9. **最初の用途** (未決): 群衆 / ボイド / 格子など。planner が 1 つ決めてデモシーンにする。
10. **Unity / UE の前例確認** (memory `reference-existing-engines.md`): Unity の `ComputeShader` + Burst `IJobParallelFor`、
    UE の Niagara Sim Stages / `FComputeShaderUtils`。ILGPU / ComputeSharp (C# → GPU) も API 形状の参考。

## 5. 検証 (各サブで harness が実施)
- `bin\x64\Debug\Editor.exe --selftest` と Release 版。互換ヘッダの組み込み関数、`KERNEL` 展開、ハッシュ照合、State への GPU 強制拒否、
  Fx の CPU 代替、負荷枠超過の挙動を `*SelfTest.cpp` に足す。
- `tools\check_rules.ps1` (規則 11: ABI ミラー)。C# は temp プローブで実走 (memory `abi-bump-verification.md`)。
- `tools\replay_verify.bat`: compute A/B (論点 5) を足し、Debug / Release / WARP で一致。
- 同じカーネルの CPU 版と GPU 版の結果差を selftest で計測し、Fx 用途の許容差を数値で残す。
- `--screenshot` で Fx カーネルの見た目、Editor のインスペクターで統計表示を確認。
