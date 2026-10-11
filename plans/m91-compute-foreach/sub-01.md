# sub-01: 単一ソースのカーネル (互換ヘッダ・KERNEL 展開・CPU 実行器・ハッシュ関数)

- 依存: なし
- 状態: 未着手
- 往復: 0
- 再編: 2026-10-12 (U1/U6 反映。このサブの中身は C++ 側に限定。検査器は sub-02)

## やること
M91 で最もリスクが高い未知 (spec 7.2 R1: 同じテキストが FXC と MSVC の両方で通り、CPU 結果が決定的か) を、ABI を触らずに潰す。
spec 4.1.1 / 4.1.2 / D3 / D4 / D16 / D17 / D21。

1. GPU 側: `assets/shaders/kernels/kernel_common.hlsli` — `KERNEL` / `KERNEL_PARAMS` / `RW` / `RO` を HLSL に展開
   (`[numthreads(64,1,1)] CSMain`、`id >= count` の早期 return、params は要素 1 の `StructuredBuffer`、バッファは位置順の register)。
   要素数はエンジンが渡す小さな定数 (params とは別の 1 要素 StructuredBuffer か cbuffer の uint 1 個。uint 1 個なら 16B 問題は無い) で渡す。
2. CPU 側: 互換ヘッダ (置き場は coder 判断。GameLogic と Engine の両方が include できる場所 = `src/Shared/` 配下の header-only を推奨) —
   `float2/3/4` `int*` `uint*` (成分 `.x..w` のみ、スウィズル無し)、演算子、4.1.2 の関数ホワイトリスト (`sin`/`cos` は既存の `MyeSinRad` 系)、
   `KernelId` (RW の添字専用型)、`RW(T)` / `RO(T)` のビュー型 (RO 範囲外は 0 初期化 T)、`KERNEL` 展開 (関数 `void Name_Cpu(KernelId id, const P& params, Views...)` と登録用メタ)。
   `kernel_common.hlsli` の include 行は CPU 側では互換ヘッダに差し替わるようにする (include パス or マクロ)。
3. CPU 実行器 (Engine 内部、`src/Engine/Core/Jobs/` か `src/Engine/Engine/Compute/` — coder 判断): `JobSystem::ParallelRanges` で
   `id` を回す。レンジは 8 要素境界 (D16)、`cpuMaxChunks` 相当の上限を引数で受ける。ジョブ内からの呼び出しは失敗を返す。
4. ハッシュ関数: BOM 除去 + CRLF→LF 正規化後の FNV-1a 64 (D17)。codegen (sub-05 の PowerShell) と C# 変換器 (sub-02) と同じ結果になるよう、
   テストベクタ (CRLF 版と LF 版が同値) を selftest に置く。PowerShell 版はこのサブでは作らないが、仕様 (擬似コード) を ADR 下書きか実装メモに残す。
5. `ShaderManager` のプロジェクト索引に `kernels/*.kernel.hlsl` を追加し、GPU 版を `LoadCompute` 経路でコンパイルできるようにする (最小限。meta / ホットリロードは sub-07)。
6. 自己テスト用カーネル (例 `assets/shaders/kernels/SelfTestSaxpy.kernel.hlsl` と、sin/cos・RO 範囲外・for・三項を含む 1 本) を Editor の selftest に CPU 版として組み込み、
   - GPU (CI は WARP) と CPU の結果差 (最大絶対誤差・ULP) を計測し、許容差と比較
   - CPU 結果ハッシュが jobs あり/なし・チャンク数違いで一致、固定値と照合 (Debug / Release で同じ固定値)
   - RO 範囲外が 0
7. 自己テスト用カーネルは spec 4.1.2 (2026-10-12 版) に従って書く (float リテラルは `f` 付き、縮小変換なし等)。互換ヘッダは C# 版 (sub-02/06) と同じ演算順になるよう、`dot` / `length` / `normalize` などを左から右のスカラー式で定義し、式を実装メモに残す (C# 移植の正本)。
8. HLSL と C++ で挙動が割れる書き方を見つけたら spec 4.1.2 への追記案として SELF_EVAL の「仕様との差分」に出す (spec 本文は書き換えない)。

## やらないこと (このサブでは)
- ABI / GameLogic / C# / snapshot / Fx の upload・readback 一般化 (selftest 内で GPU 結果を読む最小の STAGING は可)
- `.meta`・インスペクター・ホットリロード整備 (sub-07)
- サブセット検査器・C# 変換 (sub-02)

## 触る場所 (planner の見立て)
- `assets/shaders/kernels/` (新規ディレクトリ、`.meta` は既存 `.hlsl` と同じ形で付ける)
- `src/Engine/Renderer/Shader/ShaderManager.cpp` (`RebuildProjectShaderIndex`、`LoadCompute` の名前解決)
- `src/Engine/Core/Jobs/JobSystem.h` (呼び出すだけ。変更が要るなら最小限で理由を書く)
- 新規: 互換ヘッダ、CPU 実行器、ハッシュ関数、`*SelfTest.cpp`
- `tools\gen_project_files.ps1` の再実行 (ファイル追加)

## 受け入れ条件 (このサブ)
1. spec 5-1: 自己テストカーネルが FXC と MSVC の両方で通り、GPU/CPU 差が許容差内 (実測値を実装メモに記録)
2. spec 5-2: CPU 結果ハッシュが Debug / Release / `--no-jobs` / チャンク数違いで同一の固定値
3. spec 5-3: RO 範囲外 = 0 を selftest で確認。`particles[id + 1]` 形の書き込みが CPU でコンパイルエラーになることを、一時ファイルで確認した手順と結果を実装メモに残す
4. ハッシュ関数のテストベクタ (CRLF/LF/BOM 有無が同値) が通る

## 検証コマンド
- `tools\gen_project_files.ps1`
- Debug|x64 と Release|x64 のビルド
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`、`--no-jobs` 付きでも
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (coder)

#### 設計の要点 (確認した事実)
- **FXC は可変長マクロ (`__VA_ARGS__`) を持てない** (fxc 10.1 で `error X1500` を確認)。そのため HLSL 側の `KERNEL(Name, P, Decls)` は 3 引数固定にし、バッファ宣言は `RW(T, n) RO(U, m)` を**コンマ無しで並べて 1 引数に束ねる**書式にした (spec 4.1.1 の例はコンマ区切り。差分欄参照)。
  C++ 側は `RW(T,n)` が `, RWView<T> n` に展開されるので、並べたものがそのまま関数の引数リストになる。
- 1 ファイルの C++/HLSL 分岐: `assets/shaders/kernels/kernel_common.hlsli` が `#ifdef __cplusplus` で両方の展開を持つ。C++ では取り込み側が `Shared/KernelCompat.h` を先に include し、`namespace mye::kernel::cpu::<Name> { #include "...<Name>.kernel.hlsl" }` で取り込む (名前空間を mye::kernel の内側にするのは、CRT の `sin`/`sqrt` より先に互換関数を見つけさせるため。外側だと float 同士で曖昧になる)。「include 行の差し替え」は不要になった (互換ヘッダを先に include しておくだけ)。
- GPU 展開: params = `StructuredBuffer<P> __kernel_params` (t0)、要素数 = `cbuffer __KernelCount { uint __kernel_count; }` (b0)、`[numthreads(64,1,1)] CSMain` が `id >= count` で早期 return して `Name_Body(id)` を呼ぶ。レジスタは**宣言順の自動割り当て** (RO は t1 から、RW は u0 から)。エンジンは名前でなくリフレクションの `BindPoint` で束縛する (selftest がその実装例: `RunKernelGpu`)。
- 構造化バッファの詰め方: リフレクションの `NumSamples` (= stride) が C++ の `sizeof` と一致 (params 20 バイト = `float + float3 + uint`、要素 20 バイト、16 バイト) = **D3 (4 バイト詰め) を実測で確認**。
- `KernelId`: `RWView::operator[]` は `KernelId` だけ。`uint` / `int` / `id + 1u` は `= delete` のテンプレートに当たり C2280。`particles[id + 1]` を一時ファイル (`kernel_common.hlsli` + `BadKernel` を `cl /std:c++20 /W4` で単体コンパイル) で試すと `error C2280: 'T &RWView<T>::operator[]<uint>(I) const': 削除された関数を参照しようとしています` (検査器を待たず C++ コンパイラが弾く)。
- `JobSystem::InJob()` を新設 (thread_local の深さカウンタ。`DrainChunks` のレンジ呼び出しと直列経路の両方で +1)。ネスト呼び出しを実行器が弾くのに必要。JobSystem の他の挙動は変えていない。
- `ShaderManager`: プロジェクト索引に `*.kernel.hlsl` (短名 `Foo.kernel`) を追加。`ResolveInclude` は通常の探索で見つからない相対名を `<shaderRoot>\kernels\` でも探す (D3DCompile の include は呼び出し元ファイルの階層を渡さないため。`#include "kernel_common.hlsli"` を spec どおりの綴りにできる)。キャッシュの依存再解決も同じ関数を通る。

#### 検証結果
- fxc 単体: `fxc /T cs_5_0 /E CSMain` でカーネルの展開が通る (dcl_resource_structured t0, 4 / t1, 16 / dcl_uav_structured u0, 16 を確認)。
- Debug|x64 / Release|x64 ビルド: 警告 0 (W4)。`tools\gen_project_files.ps1` は `pwsh` で実行 (Windows PowerShell 5.1 だとパースエラー。リポジトリ既存の挙動)。
- `Editor.exe --selftest` Debug / Release とも exit 0、Kernel 節 PASS。CPU 版の結果ハッシュ (jobs on/off、maxChunks 1/3、1 ブロックずつ、の 6 構成すべてで同一、Debug = Release):
  - SelfTestSaxpy (10007 要素): `1e2749c7308fa99c`
  - SelfTestMix (4099 要素): `938f8aef9ad7c55c`
- GPU (WARP) と CPU の差: Saxpy = 最大 0 ULP / 不一致 0 / 10007 要素。Mix = 最大絶対誤差 6.2e-6 (要素 1247)、tag (整数ハッシュ) は不一致 0。許容差は Saxpy 1 ULP、Mix 1e-4 (絶対)。
- `tools\check_rules.ps1`: 0 error (警告 50 は既存の rule 7 のみ。新規ファイルは該当なし)。

#### C# 変換器 (sub-02/06) が移植する正本 (演算順)
スカラー式はすべて float の左から右。FMA を使わない (`/fp:precise`、SSE2)。
- `dot`: 2 次元 `x*x' + y*y'`、3 次元 `(x*x' + y*y') + z*z'`、4 次元 `((x*x' + y*y') + z*z') + w*w'`
- `cross(a,b) = (a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x)`
- `length(v) = sqrt(dot(v,v))`、`distance(a,b) = length(a-b)`、`normalize(v) = v * rsqrt(dot(v,v))`、`rsqrt(x) = 1.0f / sqrt(x)`、`rcp(x) = 1.0f / x`
- `lerp(a,b,t) = a + (b - a) * t`、`saturate(x) = min(max(x,0),1)`、`clamp(x,lo,hi) = min(max(x,lo),hi)`、`min(a,b) = b < a ? b : a`、`max(a,b) = a < b ? b : a`
- `step(e,x) = x >= e ? 1 : 0`、`smoothstep(lo,hi,x): t = saturate((x-lo)/(hi-lo)); t*t*(3 - 2*t)`、`frac(x) = x - floor(x)`、`sign` は int を返す (`x>0 ? 1 : x<0 ? -1 : 0`)
- `abs(float)` は符号ビットを落とす (`-0.0f` -> `+0.0f`)。`floor`/`ceil`/`sqrt` は IEEE 単精度の正しい丸め
- `sin(x)`: `|x| > 1.5*pi` なら `x = x - 2pi * floor(x * (1/2pi) + 0.5f)` で寄せてから `MyeSinRad(x)`。`cos(x)`: `|x| > pi` なら同じ寄せ、その後 `MyeCosRad(x)` (= `MyeSinRad(x + pi/2)`)。`kMyePi = 3.14159265358979f`、`kTwoPi = kMyePi*2.0f`、`kInvTwoPi = 1.0f / kTwoPi` (float 定数の計算順もそのまま)。`MyeSinRad` の式は `Shared/ScriptAPI.h` が正本 (★1 命令も変えない)
- `HashU32(v): state = v*747796405u + 2891336453u; word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u; return (word >> 22u) ^ word`、`HashToUnitFloat(h) = (float)(h >> 8u) * (1.0f / 16777216.0f)` (HLSL と C++ で同じ式。tag の GPU/CPU 完全一致を selftest で確認)
- ベクトル演算子: 成分ごと。スカラー混在は `vec op scalar`/`scalar op vec`。比較演算子は定義しない

#### ソースハッシュの手順 (PowerShell codegen / C# 変換器向け。`Engine/Compute/KernelHash.h` が正本)
1. 先頭の BOM `EF BB BF` を 1 個だけ除く。2. `CR` の直後が `LF` の `CR` を除く (単独の CR は残す。`CR CR LF` は 1 個目の CR が残る)。3. 残りのバイト列の FNV-1a 64bit (offset `14695981039346656037`、prime `1099511628211`、`h ^= byte; h *= prime`)。空 = offset。参照値: `"a"` = `af63dc4c8601ec8c`。PowerShell は `[IO.File]::ReadAllBytes` で読んでバイト列に対して同じ処理をすること (文字列にデコードすると BOM が自然に落ちるが、CR/LF の扱いを揃えるため)。

SELF_EVAL: sub-01 (round 1)
実装: KernelCompat.h (互換層)、kernel_common.hlsli (GPU/C++ 両展開)、KernelRunner.h (CPU 実行器)、KernelHash (ソースハッシュ)、JobSystem::InJob、ShaderManager (索引と include 探索)、KernelSelfTest、自己テスト用カーネル 2 本
仕様との差分:
  - [逸脱] KERNEL のバッファ引数はコンマ無しで並べる (`KERNEL(Name, P, RW(T,a) RO(U,b))`)。FXC は可変長マクロ不可 (実測)。spec 4.1.1 の例の修正案: コンマを外す
  - [逸脱] 取り込み側は `namespace mye::kernel::cpu::<Name> { #include ... }` と書く。include 行の差し替えは不要 (互換ヘッダを先に include)
  - [追加] sin/cos は範囲外のとき 2pi で寄せてから MyeSinRad/MyeCosRad (MyeSinRad は |x|<=3pi/2 が前提のため)
  - [追加] JobSystem::InJob() を新設 (ジョブ内からの ForEach 拒否に必要)。ShaderManager::ResolveInclude に kernels\ 配下の探索を追加
  - [追加] spec 4.1.2 追記案: (a) `[unroll]` は C++ で通らない -> 表から外す、(b) 自由関数の `out`/`inout` は C++ に同じ綴りが無い -> 外す、(c) `&&` `||` `?:` は GPU で両辺評価されうるので副作用のある式を置かない、(d) 予約語: `params` `id` `KernelId` `HashU32` `HashToUnitFloat`、(e) 組込み関数のスカラー混在は min/max/clamp/lerp/step/smoothstep のみ対応、(f) 2 本目以降の RW は第 1 引数以上の要素数が必要 (CPU は拒否)
  - [未実装] PowerShell 版ハッシュ (sub-05)。手順は実装メモに記載
検証: gen_project_files (pwsh)、Debug/Release ビルド警告 0、selftest Debug/Release/--no-jobs 付きの 4 通りすべて exit 0 で Kernel 節 PASS、check_rules 0 error、一時ファイルで particles[id + 1] が C2280、fxc 単体で展開確認
自己採点: 仕様適合 4 / 正しさ 4 / コード品質 4 / テスト 4

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。逸脱 2 件 (KERNEL がコンマなし、名前空間で包む取り込み) と追記案 (a)〜(f) は spec 4.1.1 / 4.1.2 に反映した。sin/cos の折り返しは採用。nit: 新しく作った .meta が LF で、既存の .meta (CRLF) と線種が違う — 申し送り。
