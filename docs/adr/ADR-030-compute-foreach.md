# ADR-030: Compute.ForEach — 1 つのカーネルソースを GPU・C++ の CPU 版・C# の CPU 版で走らせる。State は常に CPU、Fx だけが GPU を候補にする

- 状態: **確定** (2026-10-12、M91a〜M91j)
- 出所: M91 Compute.ForEach。計画は `plans\m91-compute-foreach\spec.md`、判断の経緯は同じ場所の `harness.md` / `sub-NN.md`。
- 実体:
  - カーネル: `assets\shaders\kernels\*.kernel.hlsl` と `kernel_common.hlsli`、互換層 `Shared\KernelCompat.h` / `src\Scripting\KernelCompat.cs`、`Shared\KernelRangeFn.h`。
  - 検査・変換: `Engine\Compute\KernelParser.cpp`、`KernelLint.{h,cpp}`、`KernelCSharp.{h,cpp}`、`KernelCppCodegen.{h,cpp}`、`KernelHash.{h,cpp}`。
  - 実行: `Engine\Compute\ComputeService.{h,cpp}`、`ComputeDecision.{h,cpp}`、`ComputeBufferStore.{h,cpp}`、`KernelRunner.h`、`Renderer\Compute\FxGpuBackend.{h,cpp}`、`Engine\Script\EngineApiTable.cpp`、`ScriptHost.cpp`。
  - sim への組み込み: `Engine\Replay\SimSnapshot.cpp` (CMP 節)、`WorldHasher.cpp` (`HashComputeState`)、`Engine\Loop\TickRunner.cpp`、`HeadlessSim.cpp`。
  - API: `Shared\EngineAPI.h` (ABI v29)、`Shared\ScriptCompute.h` (C++ 糖衣)、`Shared\ScriptAPI.h` (`COMPUTE_KERNELS`)、`src\Scripting\Compute.cs` (C#)。
  - 生成物: `src\GameLogic\Generated\Kernels.gen.{h,cpp}` (版管理)、`assets\scripts\Generated\Kernels.gen.cs` (.gitignore)、外部プロジェクトは `<project>\cache\Generated\Kernels.gen.{h,cpp}`。
  - UI・設定: `Editor\Windows\Scene\ComputeInspectorSection.{h,cpp}`、`Engine\Compute\ComputeSettings.{h,cpp}` (`project_settings.json` の `"compute"`)、`ComputeInspectorRows.{h,cpp}`。
  - デモ・検証: `--compute-demo`、`tools\replay_verify.bat` の `compute` / `computeab`、`ComputeDemoSwarm.cpp` / `ComputeDemoSparks.cpp` / `assets\scripts\ComputeCsSwarm.cs`。
- 番号: ABI **v28 → v29** (164 → **174** スロット)、SimSnapshot **v48 → v49** (CMP 節) → レビュー 1 の指摘で **v50** (CMP 節を State の表だけにした。D11 / D29)、configBits に `kCfgComputeForceCpu = 1u << 5`。TypeId・`kCookVersion` は使わない。
  v21 の Compute ABI (7 本、GPU 専用) は変更しない。

## 背景

C++ (GameLogic) と C# のゲームプログラマーが、要素ごとに独立した計算を 1 つのソースで書き、1 つの呼び方で走らせ、実行先をエンジンに任せたい。
ただし契約として、Debug / Release / CI (WARP) のシミュレーションがビット一致しなければならない (AGENTS.md 4.2、engine_spec §11)。
GPU と CPU は同じ結果にならない (FXC と MSVC で演算列・FMA・超越関数が違う。後述の実測)。この衝突が設計の中心になる。

M91 より前の状態:

- GPU の計算口 (ABI v21、`ComputeAbiRunner`) はあるが、アップロードも読み戻しも CPU 実行も無く、ゲーム状態には使えない。
- CPU 並列 (JobSystem、ADR-028) は ABI に出ておらず、GameLogic / C# から使えない。
- 同じ計算を GPU と CPU で 2 回書いている (`particle_sim.cs.hlsl` と `CpuParticleBackend::StepPool`)。

## 決定の要約

1. バッファに **State / Fx** の種別を持たせる。**State は常に CPU・同期**で、スナップショット・ワールドハッシュ・Replay の対象。GPU を候補にするのは Fx だけ。
2. カーネルは `*.kernel.hlsl` 1 本が正本。GPU (cs_5_0)・C++ の CPU 版・C# の CPU 版へ、HLSL サブセットの検査器を通してから展開する。
3. C++ の呼び出しは常に C++ 版、C# の呼び出しは常に C# 版 (GameLogic.dll の有無で C# の結果を変えない)。両者は**バイト一致**する。
4. 版ずれ (CPU 版が古い) は FNV-1a 64bit のソースハッシュで検出し、State は実行を拒否する。
5. 宣言 (`COMPUTE_KERNELS` / `[ComputeKernels]`) していないカーネルの呼び出しは、実行せずに失敗にする。観測と上書きの UI はスクリプトのインスペクター。

## 決定 (D1〜D28)

番号は spec.md の疑った点 (D1〜D28) と同じ。却下した案は理由と一緒に書く。

### D1: 「意識せずに」と決定論の両立 — バッファ種別 State / Fx

- 採用: State は常に CPU。Fx は GPU を優先し、デバイスが無ければ同じカーネルを CPU で走らせる。呼び方とソースが同じ、という意味での「意識せずに」。
- 根拠 (実測、決定 14 に詳細): 実機 GPU (RTX 3060) では Saxpy (`a*x+y`) が CPU と最大 64 ULP 違った。GPU が融合積和 (mad) を使うため。WARP では 0 ULP だった。
  つまり CI が通っても実機では GPU と CPU の結果が割れる。ゲーム状態を GPU に渡すと、マシンごとに別の世界になる。
- 却下: 全部 Auto にして実行先で自動選択する案 (決定論が壊れる)。ハッシュだけで GPU 結果の一致を確認する案 (CI の WARP でしか一致しない)。

### D2: v21 の Compute ABI は変えず、別スロットと別ストアにする

v21 は GPU 専用で cbuffer を名前で合わせる。共通化すると State を GPU バッファに渡せる穴ができる。ForEach は ABI 末尾に追加した 10 本 (決定 11) と別のハンドル空間を使う (v21 のハンドルを渡すと 0)。

### D3: params は要素数 1 の `StructuredBuffer<Params>`

cbuffer は 16 バイト境界の規則で C++ / C# の struct と配置がずれる。構造化バッファは 4 バイト詰めなので、レイアウトの規則が 1 つで済む。
実測: リフレクションの stride と C++ の `sizeof` が一致 (params 20 バイト = `float + float3 + uint`)。

### D4: `RW` の添字は `KernelId` だけ

ADR-028 の「自分の要素だけを書く」を規約でなく型で守る。`particles[id + 1]` は C++ で `error C2280` (`operator[]<uint>` は `= delete`)、C# で `CS1503`、検査器で KL016。
`RO` は範囲外を読むと 0 初期化の T を返す (D3D11 と同じ。CPU で UB にしない)。

### D5 (ユーザー判断 U1): C# にも CPU 版を作る

- planner の当初案は「C# は GameLogic.dll の C++ 版を使うか、無ければ制限を受け入れる」だった。理由は変換器とその決定論の検証でサブが 1 本増えるため。
- ユーザーが逆を選んだ: **エンジンが HLSL サブセットを C# に変換**し (`Kernels.gen.cs`、`SchemaCodegen` と同じ流儀)、C# スクリプトと一緒に Roslyn でコンパイルする。C# だけのプロジェクトでもビルド工程も MSVC も要らない。
- 代償: 変換器 (`KernelCSharp.cpp`) と、RyuJIT の決定論の検証 (D24)。

### D6 (U2): WARP は使わない

デバイスが無いときの Fx は native CPU 版で代わりに走らせる。CPU 版も無ければ失敗。Server.exe は device-less 設計 (ADR-022)。

### D7 (U3): State はエンジン所有のストア

`ComputeStateStore` を SimSnapshot の新節 (CMP、v49 → v50。BT の後・World の前。中身は State の表だけ) と `HashSources` の源にする。スクリプトは 64bit ハンドルを FIELD に持つだけ (ADR-003)。
ハンドルは決定的に採番 (最小の空きスロット)。ハッシュはスロット昇順で、世代・持ち主・形・中身を畳む。State が空なら節ごと畳まないので、既存シーンのハッシュ列は動かない。

### D8: 寿命は作成したスクリプトのエンティティが持つ

エンティティの破棄 (tick 末の `ReleaseDeadOwners`) で自動解放。明示 Release も可。Play 停止とシーン読込で全解放 (採番も初期化)。

### D9: C# が作るバッファは NoHash の別ストア

C# レーンは record / verify / networked / resim 中に走らず NoHash (`kComponentNoHash`)。C# の State は「C# の CPU 版で同期実行、GPU 禁止」で意味は C++ と同じだが、Replay の被覆には入らない。C# レーンを Replay の対象にするのは範囲外。

### D10 (U4): State の ForEach は同期

tick をまたぐジョブがスナップショットに入るのを避ける。非同期は Fx の GPU 経路だけ。

### D11: Fx の GPU 結果を sim に漏らさない

規約: Fx のバッファ・ジョブハンドル・`Status()` / `Target()`・`Read` の結果を、sim の分岐 (バッファの作成・解放を含む) と FIELD に使わない。
State と Fx は**別々のスロット表と採番**を持つ。State のハンドルは State の作成と解放だけで決まり、Fx の有無・作り直し・復元に影響されない。ハンドルの世代欄は [lane 印 2 bit | 種別の印 1 bit | 作成回数 29 bit]。Fx の作成そのものもデバイスに依存しない (CPU ミラーだけ確保し、GPU バッファは最初の GPU 実行で遅れて作る)。
入口の防御: **書き込み先 (RW) に State があり、かつ引数に Fx が混ざる呼び出しは MixedKinds で失敗**する。検出: compute A/B (決定 12)。

- スナップショットとハッシュは State の表だけ。Fx は入れず、復元の前後で Fx の表に触らない。Fx は表示用なので、手元の Fx のハンドルは復元後も有効 (撮影後に作った Fx も捨てない)。
- 経緯 (レビュー 1 の指摘 1): 最初は State と Fx が 1 つのスロット空間を共有し、スナップショットに Fx の「居た印と世代」だけを入れていた。すると (a) 同じ Fx を持たないストアへの復元 (ネットのクライアント参加 `ClientSimRunner::ApplySnapshot`、ロールバック中の Fx の作り直し) と、(b) 復元後にスクリプトが Fx を作り直す経路 (ComputeDemoSparks) で、State の採番が連続実行とずれた (probe: continuous=8000000100000002、restored=8000000200000001)。
- 却下: (1) 空いた Fx のスロットを予約として残す案、(2) Fx の count / stride もスナップショットに入れて作り直す案。どちらも (a) は直るが (b) は直らない。
- 常設の試験: ComputeStateSelfTest / ComputeServiceSelfTest の (i)〜(v)。

### D12 (U5): Fx の出口は読み戻し経由だけ

Read → `DebugDrawLine` / NoHash の描画コンポーネント。バッファを直接描く口は作らない。

### D13: Fx の GPU `Read` は遅延、`Wait` は明示ストール

`Read` は最新の完了済み結果を返す (3 本のステージング + EVENT クエリ。`OcclusionCuller::PollStats` と同じ形)。`Wait` は待つ。
Fx を CPU で走らせるとき GPU 側に未反映の結果があれば待つ (明示ストールとして `waitCount` に積む)。Auto が CPU と GPU を行き来すると毎回起きうるが、観測できるので許容する。

### D14: resim 中の Fx は何もしない

resim 中の Fx の ForEach は Skipped を返す (粒子と音と同じ)。State は resim でも走る。

### D15: `kCfgComputeForceCpu = 1u << 5`

`--compute-force-cpu` で Fx も CPU に固定する。configBits に入れる (`kCfgJobs` が前例の「ビット同一のはず」のオプション)。

### D16: 「自動 SIMD」の保証範囲

保証するのは (a) 8 要素境界でのレンジ分割、(b) 要素ごとの演算列がスカラーと同一、(c) スレッド数・jobs の有無・Debug/Release でビット一致、まで。速度は保証せず、計測して記録する (決定 14)。RyuJIT は自動ベクトル化をしないので、C# 版は (c) だけを約束する。

### D17: ソースハッシュの正規化

BOM を 1 個だけ除き、`CR` の直後が `LF` の `CR` を除いた残りのバイト列の FNV-1a 64bit (offset `14695981039346656037`、prime `1099511628211`)。参照値 `"a"` = `af63dc4c8601ec8c`、空 = `cbf29ce484222325`。
C++ (`KernelHash`)、PowerShell の codegen (`gen_project_files.ps1`)、C# 変換で同じ値になることをテストベクタで確認した。
`core.autocrlf=true` の新規チェックアウトでカーネルが CRLF になってもハッシュは割れない (改行の統一は `docs\line-endings.md`)。

### D18 (U6 の帰結): スクリプトが使うカーネルを宣言する

- 当初案は「宣言を作らない」だった。U6 (スクリプトのインスペクターに実行状況を出す) で「まだ実行していないカーネル」も並べる必要が出たので作る。
- C++: `COMPUTE_KERNELS(Swarm, "A", "B");` (`MyeScriptDesc` の末尾に名前表 = ABI 変更)。C#: クラスの `[ComputeKernels("A", "B")]`。
- 宣言していないカーネルの呼び出しは**実行せずに失敗**する (ユーザー判断 U10。当初案は「実行して WARN」だった)。他の判定より先に行う。

### D19 (U6): 観測と上書きの UI は「スクリプトのインスペクター」

- 当初案は `.kernel.hlsl` アセットのインスペクターだった。ユーザーが逆を選んだ。
- エンティティのスクリプト欄に「Compute」の折り畳みを出す。行 = 宣言 ∪ 実行時に観測したカーネル (名前の昇順)。項目: 状態、最終の実行先、要素数、upload / readback のバイト、CPU の ms (GPU は回数)、版の一致、上書き。
  狭いインスペクターで表の列が切れたので、実装はカーネルごとの縦並び。観測の対応表は (スクリプト型, カーネル名) をキーにした表示専用のデータで、スナップショットにもハッシュにも入れない。

### D20 (U7): HLSL サブセットは厳しめに絞る

決定 7 のとおり。C++ の匿名 union なしにスウィズルは作れない (匿名 union は UB)。C++ と C# で同じ演算列になる書き方だけを許す。

### D21: 上限

1 バッファの要素数 4,194,240 (D3D11 のスレッドグループ 65535 × 64)、stride は 4 の倍数で 2048 以下、1 本 128 MiB 以下、スロット 1024、State の総バイト `stateTotalBytes` (既定 8 MiB。決定 13)。超過すると 0 を返す。

### D22: 番号は着手時の末尾から取る

ABI は sub-05 (v29)、snapshot は sub-04 (v49)、sub-16 で v50、ADR は sub-09 (030)。memory の M75h は v30 へ繰り下がる。

### D23: サブセット検査器 — 全バックエンドの前段

違反があればそのカーネルはどの実行先でも失敗する。理由: `1.0` は HLSL では float、C++ / C# では double (C++ は `float - 1.0` を double で計算する)。C# は暗黙の縮小変換を許さない。整数の 0 除算は C# では例外、C++ では UB。float→int の C 形式キャストは範囲外と NaN で C++ (UB)・C# (.NET 8 x64 は 0x80000000)・HLSL が割れる。

規則 (正本は `KernelLint.h` の KL001〜KL028。負例は全規則に最低 1 本、計 124 本。`file(12): error KL005 [swizzle]: ...` の形で出る):

| ID | 内容 | ID | 内容 |
|---|---|---|---|
| KL001 | サブセット外の構文 | KL015 | 予約語・組込み名の再利用 |
| KL002 | `#include "kernel_common.hlsli"` 以外の前処理 | KL016 | RW の添字が `id` 以外 |
| KL003 | bool / double / half / 行列 / テクスチャ | KL017 | スカラー混在が許される 6 関数以外 |
| KL004 | 配列 | KL018 | 遮蔽・再宣言 (C# の CS0136 に合わせる) |
| KL005 | スウィズル・存在しないメンバ | KL019 | 未宣言の名前 (再帰を含む) |
| KL006 | `f` の無い小数リテラル | KL020 | KERNEL / KERNEL_PARAMS の形・ファイル名との不一致 |
| KL007 | 範囲外・8 進の整数リテラル | KL021 | groupshared / barrier / Interlocked / switch |
| KL008 | 型の不一致・暗黙の縮小変換・C 形式の float→int | KL022 | シフト量が 0..31 のリテラルでない・ベクトルのシフト |
| KL009 | 整数の `/` `%` は右辺が 0 でない整数リテラルのときだけ | KL023 | 戻り値を返さない経路 |
| KL010 | ベクトルの比較・ベクトルの三項演算子 | KL024 | RO / params / const への書き込み |
| KL011 | ホワイトリスト外の組込み関数・引数の型 | KL025 | 条件式が bool でない |
| KL012 | 属性 (`[unroll]` など) | KL026 | グローバル変数 |
| KL013 | out / inout / in 引数 | KL027 | struct の形 (入れ子・空・bool メンバ) |
| KL014 | 式の中の代入・`++` `--` | KL028 | 初期化子のない宣言 (AGENTS.md 4.3) |

仕様書の表より厳しくした主な点: 1 引数のコンストラクタ (`float3(1.0f)`) の禁止 (FXC が X3014 で拒否)、`uint` の単項マイナスの禁止 (C# で long になる)、条件式は bool のみ、`min` / `max` のスカラー混在は (ベクトル, スカラー) の順だけ、入れ子 200・1 式 500 演算子で打ち切り。

許す関数 (ホワイトリスト): `min max clamp abs saturate lerp step smoothstep floor ceil frac sqrt rsqrt rcp dot cross length normalize distance sign sin cos`、互換関数 `FloatToInt` `FloatToUint` `HashU32` `HashToUnitFloat`。

- `FloatToInt(x)` / `FloatToUint(x)`: NaN → 0、範囲外は飽和 (int は 2147483520.0f、uint は 4294967040.0f)、0 方向へ切り捨て。**互換層 3 か所 (`kernel_common.hlsli` / `KernelCompat.h` / `KernelCompat.cs`) を同じ式に保つ**こと。
- `rsqrt` / `rcp` は GPU では近似、CPU (C++ / C#) では厳密な `1/sqrt(x)` / `1/x` (Fx の GPU と CPU が割れうる理由の 1 つ)。
- 符号付き整数の `+ - *` のオーバーフローは書き手の責任 (C++ では UB、C# は巻き戻る)。ハッシュなど溢れうる計算は `uint` で書く。`kernel_common.hlsli` の冒頭にも同じ注意がある。

### D24: C# の CPU 版は C++ とバイト一致する (受け入れ 15)

- C# の互換型 (`float2/3/4` など) は自前の struct で、成分の演算を左から右のスカラーで書く。`System.Numerics`(`Vector3.Dot` は SIMD の水平加算で加算順が違いうる) は使わない。`sin` / `cos` は C++ の `MyeSinRad` 系の式を移植。FMA を使わない (`/fp:precise`、SSE2)。
- 演算順の正本: `dot` は 2 次元 `x*x'+y*y'`、3 次元 `(x*x'+y*y')+z*z'`、4 次元 `((x*x'+y*y')+z*z')+w*w'`。`length(v) = sqrt(dot(v,v))`、`normalize(v) = v * rsqrt(dot(v,v))`、`lerp(a,b,t) = a + (b-a)*t`、`min(a,b) = b < a ? b : a`、`smoothstep` は `t = saturate((x-lo)/(hi-lo)); t*t*(3-2*t)`。
- **結果**: ホワイトリストの全関数・`int2`・`FloatToInt`・`FloatToUint`・`while/break/continue` を通る `SelfTestAllFuncs` を含む全カーネルで、6 通りの JIT 設定すべてで C++ の CPU 版と**バイト一致**した。ホワイトリストから外した関数は無い。
  C++ の固定ハッシュ: SelfTestSaxpy (10007 要素) `1e2749c7308fa99c`、SelfTestMix (4099 要素) `938f8aef9ad7c55c` (Debug = Release、jobs の有無、チャンク数違い 6 構成で同一)。
- 6 通りの JIT 設定 (Release の `Editor.exe --selftest` を環境変数を変えて 6 回):
  1. 既定
  2. `DOTNET_TieredCompilation=0`
  3. `DOTNET_TieredPGO=0`
  4. `DOTNET_TC_QuickJitForLoops=0` + `DOTNET_OSR_HitLimit=1`
  5. `DOTNET_EnableAVX2=0` + `DOTNET_EnableAVX=0`
  6. `DOTNET_ReadyToRun=0` + `DOTNET_TieredCompilation=0`

  既定の tiered JIT では、90 ms 空けて 10 回同じ ForEach を重ね、tier-0 → tier-1 の切り替わりをまたいでも一致することを常設の selftest で見ている。
- **再実行の手順** (CI には常設しない。CI の時間が約 6 倍になるため。`KernelCompat.cs` や `KernelCSharp.cpp` を変えたら手動で回す): PowerShell で
  `$env:DOTNET_TieredCompilation='0'; .\bin\x64\Release\Editor.exe --selftest; Remove-Item Env:DOTNET_TieredCompilation` のように上の 6 組を順に設定し、`Compute C#` の項目が全部 PASS・exit 0 であることを見る (リポジトリ直下で実行する。環境変数は終わったら消す)。
- **変異テスト** (検出力の確認): `src\Scripting\KernelCompat.cs` の `dot(float3)` の加算順を `a.x*b.x + (a.y*b.y + a.z*b.z)` に変えて `tools\build_managed.bat` → selftest すると、Mix と AllFuncs が不一致で FAIL する。確かめたら戻す。
- C# の Range 関数は collectible ALC 内の `[UnmanagedCallersOnly]` を `MethodHandle.GetFunctionPointer()` で取り、JobSystem のワーカーから呼べる (Debug / Release とも確認。R7 の直列退避は不要だった)。新しい表は古い ALC を捨てる前に登録する。
  生成 C# の `Range` の署名は ABI の `MyeKernelRangeFn` と同じ (`params, bufs, bufCount, begin, end`)。
- C# の `RW` を int で添字するとコンパイルエラー CS1503 (D4)。

### D25 (U9): C# の呼び出しは常に C# 版

GameLogic.dll の有無・ホットリロードの状態で C# の結果が変わらないようにする。C++ の呼び出しは常に C++ 版。

### D26: C# の並列実行は JobSystem から

`[UnmanagedCallersOnly]` のレンジ関数 (begin, end) を JobSystem のワーカーから呼ぶ。負荷枠 `cpuMaxChunks` を C++ と共通にするため `Parallel.For` は使わない。生成コードは例外を投げない形 (範囲外の RO は 0、整数除算は D23 で制限)。

### D27 (U8): 上書きはカーネル単位

`project_settings.json` の `kernelOverrides`。どのスクリプトのインスペクターで変えても、そのカーネルを使う全スクリプトに効く (UI にそう書く)。インスペクターの上書きはセッション上書きで、保存は Project Settings 窓の「プロジェクト既定として保存」だけ。

### D28 (U10 / U11): 宣言の出所と時点

判定に使う宣言は「いま読み込まれているモジュールの宣言表」。

- C++: DLL の読込 / ホットリロードで `MyeScriptModule` を取り込む**同じ箇所・同じ時点 (旧 DLL の解放の前)** で、カーネル表と宣言表を丸ごと差し替える (`ScriptHost::LoadModule` → `PublishCompute`)。コードと宣言表は必ず同じ世代。
- C#: Roslyn のコンパイル後、スクリプトをインスタンス化する前に属性を読み、レンジ関数と一緒に登録する。
- 呼び出し元の識別: C++ はエンジンが実行中のスクリプト (`runningScript`) から決め、引数の `callerScript` は無視する (偽造防止)。C# は managed 側が持つスクリプト型 (`Type.FullName`)。
- スクリプトのコールバック (Start / Update / LateUpdate / Trigger / Collision / Break) の外 (BT タスクなど、宣言を持てない呼び出し元) からの ForEach は失敗 (U11)。
- Start の中から呼んでも、ホットリロード直後の最初の tick でも同じ判定 (selftest 済み)。名前は大文字小文字を区別して完全一致。
- 宣言したがカーネルのファイルが無い場合は、読込時に WARN を 1 回出し、インスペクターに「宣言あり・カーネル無し」と出す (呼べば失敗)。
- 宣言表は言語ごとに持つ。C++ レーンから C# の登録スロットを呼ぶと何もせず 0。

## 決定 4: 決定表 (実行先の判断)

実装は `ComputeDecision.cpp` の純粋関数 `DecideExecution` (selftest で約 55 行を確認)。時間の計測は判断に使わない。表の正本は engine_spec §10.12。
上から順ではなく、**宣言の判定を他のすべてより先**に行う。引数の誤り (同じバッファを RW を含めて重複して渡す、params の大きさ違い、stride 違い、2 本目以降の RW が短い) は失敗し、バッファに触らない。

## 決定 7: HLSL サブセット (U7「厳しめに絞る」)

型は `float` `float2/3/4` `int` `int2/3/4` `uint` `uint2/3/4` とこれらだけを持つ struct。成分は `.x .y .z .w` のみ。float リテラルは `f` 付き。float → int / uint は `FloatToInt` / `FloatToUint` だけ。
制御は `if for while break continue return` と値渡しの自由関数。使えない: 配列、行列、スウィズル、`bool` 変数、`groupshared`、barrier、`Interlocked*`、再帰、属性、`out/inout`、式の中の代入、`#define`。
`KERNEL(Name, Params, RW(T,a) RO(U,b))` の RW / RO の間にはコンマを書かない。FXC は可変長マクロ (`__VA_ARGS__`) を持てず `error X1500` になるため (実測)。第 1 引数は RW 必須でその要素数が `id` の範囲になる。ForEach に渡すバッファの本数: C# の糖衣は 1〜4 本のオーバーロードを手書きしている (可変長ジェネリックが無いため)。C++ の `Compute::ForEach` は可変長で、固定の上限をコードに持たない (5 本以上のカーネルは未検証。C# の糖衣では呼べない)。本数はカーネルの引数の数と一致しなければならない。

## 決定 11: ABI v29 = 174 スロット

末尾に 10 本: `ComputeCreateBuffer` / `ComputeReleaseBuffer` / `ComputeWriteBuffer` / `ComputeReadBuffer(.., wait)` / `ComputeForEach` / `ComputeJobStatus` / `ComputeJobTarget` / `ComputeJobWait` / `ComputeRegisterManagedKernels` / `ComputeSetManagedDeclarations`。
POD は `MyeKernelBuffer` / `MyeKernelRangeFn` / `MyeKernelArg` / `MyeKernelDesc` / `MyeComputeDecl`。`MyeScriptDesc` の末尾に `kernelNameCount` / `kernelNames`、`MyeScriptModule` の末尾に `kernelCount` / `kernels`。
`ComputeForEach` の `callerScript` は C# 用 (C++ レーンでは無視)。`GameLogic.dll` が v28 以前なら v29 のエンジンが拒否する。外部プロジェクトは再ビルドが要る。
`Interop.cs` の位置ミラーと `tools\check_rules.ps1` の規則 11 (`29 = 174`) を合わせた。

## 決定 12: compute A/B と `--compute-force-cpu`

`kCfgComputeForceCpu = 1u << 5`。`tools\replay_verify.bat` の `computeab` は、Auto で録った `.rep` を `--compute-force-cpu` で Debug / Release 検証し、ワールドハッシュが tick ごとに一致することを見る。`compute` ジョブは Debug stress + Release + Server.exe (GPU なし) で State を使うデモを照合する。
**漏れの検出力の実測**: `ComputeDemoSparks` の Update に「Fx の Read 結果 (`sparks[17].pos.y * 0.0001`) を Floor の `LocalTransform` に足す」一時改変を入れると、実機 GPU でも `--warp` でも `[ERROR] [replay] HASH MISMATCH at tick 0` / `[FAIL] compute A/B Debug verify` になる。改変は戻した。
実機 GPU では記録側と検証側で Fx の値が違うため、tick 0 から割れる。

## 決定 13: State の総バイトの既定 8 MiB と、ハッシュの方式

- State の中身は 8 バイト語を `memcpy` で読み、32 バイトごとに 4 本のレーンへ交互に流して依存の鎖を断つ (`HashStateBytes`)。端数の 8 バイト語は先頭のレーンから順に、残りのバイトは従来のバイト単位の畳み込み。最後にレーンとサイズを `HashCombine` で 1 本にする。
- 当初のバイト単位 FNV は約 1 GB/s で、16 MiB で 1 tick (16.667 ms) の 104%。ユーザー判断 U19: ハッシュを速くしたうえで、Release で 1 tick の 10% 以内に収まる 2 のべき乗の MiB を既定にする。
- 実測 (ワールドハッシュ 1 回。`ComputeStateSelfTest` の `TestHashCost`。時間は合否に使わない):

| 構成 | 1 MiB | 8 MiB (既定) | 16 MiB |
|---|---|---|---|
| 旧 (バイト単位) Release | 1.125 ms (6.7%) | — | 17.379 ms (104.3%) |
| 新 Release (最終コード) | 0.066 ms (0.4%) | 0.944 ms (5.7%) | 1.768 ms (10.6%) |
| 新 Debug (最終コード) | 0.550 ms (3.3%) | 4.811 ms (28.9%) | 9.134 ms (54.8%) |

  新 Release を別々の selftest 実行で 3 回測った幅は、16 MiB = 1.469 / 1.856 / 1.768 ms (8.8〜11.1%)、8 MiB = 0.639 / 0.864 / 0.944 ms (3.8〜5.7%)。16 MiB は 10% の線をまたいで揺れる (メモリ帯域律速で約 9 GB/s) ので、安全側の 8 MiB を既定にした。32 MiB は確実に超える。
  Debug は 8 MiB でも 1 tick の 29% あるが、Debug は計測用ではなく既定値は Release で決めた。受け入れた。
- ハッシュのさらなる高速化 (SIMD・並列・ダーティ追跡) は M91 ではやらない。メモリ帯域律速に近く効果が限定的。
- 復元 (スナップショット) は `stateTotalBytes` を検査しない (設定が録画時と違っても復元を拒まないため)。1 本あたりの上限と stride は検査する。

## 決定 14: 実測値

### GPU と CPU の差 (受け入れ 1)

| 対象 | WARP (CI) | 実機 GPU (RTX 3060、debug layer on) |
|---|---|---|
| Saxpy (10007 要素) | 最大 0 ULP / 不一致 0 | **最大 64 ULP** / 最大絶対誤差 1.52588e-05 / 不一致 2660。`a*x+y` を GPU が融合 (mad) するため |
| Mix (4099 要素) | 最大絶対誤差 6.2e-6 (要素 1247)、tag 不一致 0 | 最大絶対誤差 6.19888e-06 (最大 8890 ULP は値が 0 近傍の要素)、tag (整数ハッシュ) 不一致 0 |
| `FloatToInt` / `FloatToUint` | FXC のコンパイルのみ | NaN・±Inf・範囲外・-0.5・境界値など 18 値で GPU == CPU (不一致 0) |

- 許容差: Saxpy は 1 ULP、Mix は絶対 1e-4 (WARP で PASS)。
- 実機の Fx GPU 経路の selftest (遅延 Read・Wait・枠・極小バッファ・デバイス消失の模擬、40 項目超) は PASS し、**FAIL したのは WARP 前提の許容差 8 項目だけ** (「GPU と CPU の差が 1〜2 ULP 以内」を要求する項目)。実機で selftest を運用するなら許容差の扱いを決める必要がある (今は WARP 固定)。
  つまり実機では Fx の結果が CPU とビットで一致しない。CI では一致する。これが「State は常に CPU」の根拠。
- 実機の測定は一時改変 (WARP 初期化を実機に替える、FloatToInt の GPU プローブを足す) で行い、元に戻した。

### 速度 (1M 要素、Release、jobs 10 本、ログのみ)

| カーネル | C++ の CPU 版 | C# の CPU 版 |
|---|---|---|
| Saxpy | 0.21〜0.34 ms | 0.26〜0.60 ms |
| Mix (sin / cos あり) | 12.8〜22.4 ms | 15.1〜28.6 ms |

範囲は環境変数 (JIT 設定) の組み合わせによるばらつき。Debug の C++ は Saxpy 11.7 ms / Mix 124 ms。C# は常に Release でコンパイルされるので Saxpy 1.9 ms / Mix 56 ms。
**1M 要素の jobs なし (`--no-jobs`) との比較は、M91 では取っていない** (未計測)。jobs の有無で結果がビット一致することは確認済み (受け入れ 2、6 構成)。

### sim への影響

`tools\replay_verify.bat`: `[parallel] all 21 jobs passed` (Debug / Release 14 シーン + jobs A/B + compute A/B + タイムトラベル + 規則)。State を使うのは `compute` ジョブだけで、既存シーンのハッシュ列は変わらない。

## 決定 15: Unity / UE の前例と、本エンジンの違い

- Unity の `AsyncGPUReadback`: 数フレームの遅延と引き換えにストールしない。→ D13 の根拠。Unity Burst の `IJobParallelFor` (要素独立・バッチ分割) は記憶による。→ D4 / D16 と同じ形。
- UE Niagara: エミッタごとに Sim Target (CPUSim / GPUComputeSim) を切り替えられる。同じモジュールを VM と HLSL の両方へコンパイルする点は記憶による。→ D5 (1 つのソースから複数のバックエンドへ変換) と同じ形。
- 違い: Unity / UE はゲームプレイの決定論を保証しないので、CPU と GPU を自動で切り替えてよい。**本エンジンは State を CPU に固定し**、版ずれのハッシュ検査と compute A/B で漏れを検出する。C# の CPU 版が C++ とバイト一致することも契約にした。

## 決定 16: ホットリロードと `.kernel.hlsl` の編集

- GPU: 既存のシェーダのホットリロードで作り直す。
- C#: `ReloadHub::SetKernelHooks` が `.kernel.hlsl` の変更で `ComputeService::ReloadKernelSource` (ディスクの綴りで名前を取る・変更検出・削除) → 照合 → 変更ありなら `ManagedHost::CompileScripts` を呼び、Kernels.gen.cs を再生成して再コンパイルする。終わるまでは不一致として扱う。
- C++: GameLogic の再ビルドが要る。それまではハッシュ不一致で State の ForEach は失敗する (結果は不変)。再ビルドで一致に戻る。インスペクターに「CPU 版が古い — GameLogic を再ビルド」と出る。
- 手動確認の手順は `docs\test_checklists.md` の M91。

## 決定 17: 外部プロジェクトのカーネル (U12 / sub-10)

- `<project>\assets\shaders\kernels` から、エディタの [Rebuild Scripts] の経路 (`PrepareProjectScriptsBat`) が `cache\Generated\Kernels.gen.{h,cpp}` を出し、`cache\GameLogic.vcxproj` に組み込む。生成器は C++ (`KernelCppCodegen`) と PowerShell (`gen_project_files.ps1`) の 2 つで、**出力のカーネル表 (名前・ハッシュ・引数名・RW/RO・stride) は文字列一致する**ことを selftest で確認 (先頭の banner だけ除く)。
- カーネル本体の `#include` は絶対パス (相対だとプロジェクトの置き場所で表の中身が変わり、無駄な再ビルドが出るため)。`kernel_common.hlsli` は vcxproj の `AdditionalIncludeDirectories` にエンジンの `assets\shaders\kernels` を足して解決する。
- **エンジンの kernels は外部プロジェクトから見えない** (1b の結論): GPU・CPU のどちらからも。外部プロジェクトの `ShaderManager` の索引は assetsRoot 配下だけを走査する。起こりうる問題は、プロジェクト内の同名カーネル (索引が両方を無効にする) と、`kernels\` 以外に置いた場合で、`ComputeService::VerifyKernelSourceAgreement` が CPU と GPU の読むファイルを照合し、食い違うカーネルを検査違反扱い + ERROR 1 回にする (Bind・DLL 読込・ホットリロードの後に実行)。
- カーネル名とファイル名の不一致は、C++ 版は検査器の KL020 で表から外してログ、PowerShell 版は throw。
- 実走: 一時プロジェクトで、宣言付きスクリプトの State の ForEach が成功 (job=1)、カーネルを編集して再ビルドしないと `hash-mismatch` で job=0・結果不変、再ビルドで復帰、カーネル 0 本でもビルドが通る、を確認した。
- 配布物に `src\GameLogic\Generated` が無いときは比較の selftest を SKIP。

## 決定 18: 起動順と既知の制限

- C# の起動時の登録は `SetComputeService` より**後**でなければエンジンに拒否される。EngineLoop は `managedHost.SetComputeService` を `CompileScripts` の前に呼ぶ。HeadlessSim は順序が逆なので selftest では見つけにくい (M91h で EngineLoop が登録を拒否される不具合を直した)。
- **既知の制限**: `ReloadHub` が `CompileScripts` を同期で呼ぶので、`.kernel.hlsl` の保存や [Compile C# Scripts] で Roslyn の数秒ぶんフレームが止まる。
- 既知の制限: `ScriptHost::LoadModule` の construct 差し替えは、複数の ScriptHost が同時に生きている構成 (ServerNetSelfTest の 4 sim) では「最後に読んだ DLL」を指す (R10。エディタ・Runtime・Server は 1 つなので実害は selftest に限る)。
- 初回起動の自動 Rebuild の後に、unknown component で保持されたシーン上のスクリプトが復活するかは範囲外 (既存の挙動)。
- Fx の GPU 実行が失敗 (dispatch 失敗) したときは、CPU 版があれば CPU で代わりに走らせて WARN を 1 回。

## 検証用の環境変数

- `MYE_KERNEL_GOLDEN_DUMP=<フォルダ>`: `Editor.exe --selftest` を回すと、変換器の実際の出力を書き出す (golden を更新するとき)。ソースハッシュは `@HASH@` に置換して比較する。
- `MYE_KERNEL_LINT_FILES=<パス;パス>`: 任意のカーネルを検査して違反をログに出す (開発用の検査 CLI 代わり)。合否には影響しない。
- 3 つの互換層 (`kernel_common.hlsli` / `KernelCompat.h` / `KernelCompat.cs`) は同じ式に保つ。変えたら selftest の golden、JIT 6 通り (D24)、`FloatToInt` の単体確認を回す。

## その他

- `.gitattributes` の `*.fbx binary` により、ASCII FBX も改行変換されない (リポジトリ全体の改行の統一は `docs\line-endings.md`)。カーネルは LF で、CRLF のチェックアウトでもハッシュは同値 (D17)。
- 決定的撮影の `--compute-demo` は、火花 (Fx) が GPU と CPU で絵が少し違うので shot_verify の golden にしていない (WARP 依存になる)。

## 結果と残ること

- `Editor.exe --selftest` Debug / Release、`Server.exe --selftest`、`tools\check_rules.ps1` (0 error)、`tools\replay_verify.bat` が PASS した。
- 残り: 実機 GPU の Compute 欄の目視、GUI での [Rebuild Scripts] / [Compile C# Scripts]、Play 中のカーネル保存 (`docs\test_checklists.md`)。1M 要素の jobs なしとの比較は未計測。
- 後回し: State の非同期実行 (D10)、Fx の直接描画 (D12)、既存の粒子をカーネルへ移すこと、C# レーンを Replay の被覆に入れること (D9)。
