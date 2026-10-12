# M91 Compute.ForEach — C++/C# 共通の計算 API — 仕様書

- slug: m91-compute-foreach
- 状態: 確定 (2026-10-12。U1〜U10 はユーザーの回答を反映済み (司会が AskUserQuestion で確認)。新しく出た U11 は planner の裁定を仮置きしている — 7.1)
- 依頼原文: (一字も変えずに)
  - 「C#/C++共通の計算用APIを使えるように
    プログラマーがCPUとGPUを意識せずに処理を書けるAPIを目標
    いったん話をつめたい」
  - 「CPU/GPU自動選択、CPU/GPU強制指定、並列処理、バッファ管理、自動データ転送、非同期実行、実行結果の取得、GPU非対応時の代替実行、CPU/GPU負荷制限、自動SIMD最適化
    などを
    Compute.ForEach(
        particles,
        "ParticleUpdate",
        Execution.Auto
    );
    こんな感じで呼び出せるAPI」
  - 「大前提決定論を維持するためにCPUとGPUで結果が変わる物はCPU側で計算するように
    判断はどちらでやっても整合性/決定論が壊れないかを確認するように
    重要なゲーム状態はCPUで計算するように
    1.制限を受け入れる
    2.assets/shaders/kernels/に置く
    3.ハッシュと不一致時の実行制限」
  - 「C++/C#は同じように呼び出せるようにしてほしい
    HLSLはまだ話したい」
- 事前調査: `plans/m91-compute-foreach/design-draft.md` (§1 はユーザーとの合意事項。本書はそれを前提にする)

## 1. 目的 (なぜ作るか)

ゲームプログラマー (C++ GameLogic / C#) が「要素ごとに独立した計算」を **1 つのソースで書き、1 つの呼び方で走らせ**、
実行先 (CPU 並列 / GPU) をエンジンに任せられる状態にする。ただし、エンジンの最大の契約である
**Debug / Release / CI(WARP) のシミュレーションのビット一致** は崩さない。

現状の問題 (実コードで確認したもの):
- GPU の計算口 (ABI v21、`ComputeAbiRunner`) はあるが、アップロード・読み戻し・CPU 実行が無いので、ゲーム状態には使えない (`EngineAPI.h` の v21 注記で禁止されている)。
- CPU 並列 (JobSystem、ADR-028) は ABI に出ていないので、GameLogic / C# から使えない。
- 同じ計算を GPU と CPU で 2 回書いている (`particle_sim.cs.hlsl` と `CpuParticleBackend::StepPool`)。

達成したい状態:
1. カーネルを `assets/shaders/kernels/*.kernel.hlsl` に 1 回書けば、GPU (cs_5_0)、C++ の CPU 版、C# の CPU 版のどれでも走る。
2. ゲーム状態 (State) は常に CPU で計算する。決定的で、スナップショット / ハッシュ / Replay / ロールバックの対象になる (C++ レーン)。
3. 演出 (Fx) は GPU を優先し、デバイスが無ければ同じカーネルを CPU で走らせる。
4. C++ と C# が同じ形で呼べる。C# だけのプロジェクトでも CPU 版がある。
5. 版ずれ (CPU 版と GPU 版の食い違い) を検出して実行を制限する。
6. スクリプトを選ぶと、そのスクリプトが使うカーネルと、その実行状況がインスペクターで見える。

## 2. 疑った点と結論

凡例: 「合意済み」= design-draft §1。「ユーザー 10-12」= 司会が 2026-10-12 に AskUserQuestion で確認した回答。「planner 裁定」= ユーザーにまだ聞いていない planner の判断 (差し戻し可)。

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| D1 | GPU と CPU では同じ結果が出ない (FXC と MSVC で演算列・超越関数が違う)。「CPU/GPU を意識せずに」と「決定論維持」は両立するか | 既存の GPU 粒子と CPU 粒子は別実装で、Replay の対象は CPU 粒子だけ (`SimSnapshot.cpp:181 WriteParticles`) | 合意済み: 結果が変わるものと重要な状態は CPU | バッファに **State / Fx** の種別を持たせる。**State は常に CPU**、GPU を候補にするのは Fx だけ。「意識せずに」は「呼び方とソースが同じ」という意味に限り、実行先の候補はバッファの種別で絞る |
| D2 | 既存の v21 Compute ABI (7 本) を広げれば済むのでは (二重実装の疑い) | v21 は GPU 専用で、cbuffer を名前で合わせる方式。upload / readback が無い (`EngineAPI.h:741-788`) | planner 裁定 (論点 8) | **v21 は変えずに残す**。ForEach は別の末尾追加スロットと別のバッファストアを使う (v21 のハンドルを渡すと 0)。共通化すると State を GPU バッファに渡せる穴ができる |
| D3 | `KERNEL_PARAMS` を cbuffer にすると、16 バイト境界の規則で C++ / C# の struct と配置がずれる | 構造化バッファは 4 バイト詰め | ユーザー 10-12 (U7「厳しめに絞る」の一部) | params は **要素数 1 の `StructuredBuffer<Params>`** で渡す。レイアウトの規則が「4 バイト詰め」の 1 つだけになる |
| D4 | RW バッファで「自分の要素だけ書く」(ADR-028 の約束) を守らせる手段が無い | ADR-028 は規約で守らせている | ユーザー 10-12 (U7) | `id` を専用型 `KernelId` にし、`RW(T)` の添字は `KernelId` しか受け付けない (C++ / C# ともコンパイルエラーになり、検査器も弾く)。`RO(T)` は範囲外を読むと 0 初期化の T を返す (D3D11 と同じ。CPU で UB にしない) |
| D5 | GameLogic.dll を持たない C# だけのプロジェクトには CPU 版が無い (論点 1) | CPU 版は GameLogic.dll に入る (draft §4-1) | **ユーザー 10-12 (U1): 「C# 用の CPU 版も作る」** — planner 裁定 (制限を受け入れる) の逆 | **エンジンが HLSL サブセットを C# に変換**し (`assets/scripts/Generated/Kernels.gen.cs`、既存の `Schema.gen.cs` と同じ生成先と流儀)、C# スクリプトと一緒に Roslyn でコンパイルする。変換は実行時にエンジンが行うので、ビルド工程も MSVC も要らない。planner の当初案 (制限を受け入れる) と記録した理由: 変換器の分だけサブが 1 本増え、RyuJIT の決定論の検証が増える — ユーザーはこれを承知で選んだ |
| D6 | デバイスが無いとき、Fx を WARP に落とすか (論点 6) | Server.exe は device-less 設計 (ADR-022) | ユーザー 10-12 (U2): WARP は使わない | ForEach は WARP を使わない。デバイスが無いときの Fx は native CPU 版で代わりに走らせ、CPU 版も無ければ失敗にする |
| D7 | State の中身はゲーム状態なので、Snapshot / WorldHash / Replay / 分岐 / ロールバックの対象でなければならない (論点 4) | 可変長のエンジン所有状態を Snapshot 節に持つ前例: CPU 粒子池 (`SimSnapshot.cpp:181`)、XPBD (`:296`)、BT (`:529`)。ハッシュは `HashWorld(world, simRefs.HashSources())` に源を足す | ユーザー 10-12 (U3): エンジン所有のストア | `ComputeStateStore` (エンジン所有) を SimSnapshot の新節と `HashSources` の新しい源にする。スクリプトは 64bit ハンドルを FIELD に持つだけ (ADR-003 と整合)。ハンドルは決定的に採番し、採番表ごとスナップショットする |
| D8 | State バッファの寿命が「Release し忘れ」で決まる | AGENTS.md §5 | planner 裁定 | 作成したスクリプトのエンティティが所有し、エンティティの破棄 (tick 末の破棄点) で自動的に解放する。明示 Release も可。Play 停止とシーン読込では全部解放する |
| D9 | C# レーンは record / verify / networked / resim 中に走らず NoHash (`TickRunner.cpp:340`)。C# の CPU 版 (D5) ができても、C# の State をスナップショットに入れる意味はあるか | C# のコンポーネントは `kComponentNoHash` (`ComponentRegistry.h:19`) | planner 裁定 (U1 の帰結) | **C# が作るバッファは C# レーン用のストア (NoHash、スナップショットしない)**。C# の State は「C# の CPU 版で同期実行・GPU 禁止」で、C++ と同じ意味を持つが Replay の被覆には入らない (C# レーン全体と同じ扱い)。C# レーンを Replay 対象にするのは M91 の範囲外 |
| D10 | 非同期実行を State にも与えると、tick をまたいで実行中のジョブがスナップショットに入る | ADR-028「呼び出し元は常に 1 つ」 | ユーザー 10-12 (U4): 同期 | State の ForEach は呼び出しの中で完了する。非同期は Fx の GPU 経路だけ |
| D11 | Fx の GPU 結果を ECS (ハッシュ対象) に書かれると、決定論が壊れる | スクリプトには tick のコールバックしかない (`ScriptTypes.h:49-72`) | planner 裁定 | 規約で禁止 (注記) したうえで、Auto と `--compute-force-cpu` のワールドハッシュを照合する A/B を `replay_verify.bat` に足して**検出**する |
| D12 | Fx の GPU 結果を表示する手段が無い | ABI に「バッファを描く」口は無い。あるのは `DebugDrawLine` (`EngineAPI.h:378`) と NoHash の描画コンポーネント | ユーザー 10-12 (U5): 読み戻し経由だけ | Fx の出口は Read → DebugDrawLine / NoHash 描画コンポーネントにする。直接描画は後回し |
| D13 | Fx の readback を同期で取るとストールする | `OcclusionCuller::PollStats` (3 本リング、2 フレーム遅れ)。Unity の `AsyncGPUReadback` も「数フレームの遅延と引き換えにストールしない」 | planner 裁定 | Fx GPU の `Read` は最新の完了済み結果を返す。`Wait` は明示的にストールさせる。決定的撮影と selftest では待つ |
| D14 | resim 中に Fx を進めると演出が二重に進む | 粒子と音は `!ts.resim` で抑止している (`TickRunner.cpp:398,545`) | planner 裁定 | resim 中の Fx の ForEach は何もせず Skipped を返す。State は resim でも走る |
| D15 | `--compute-force-cpu` を configBits に入れるか | configBits には「ビット同一のはず」のオプションも入れる方針 (`kCfgJobs` が前例) | planner 裁定 | `kCfgComputeForceCpu = 1u << 5` |
| D16 | 「自動 SIMD 最適化」をエンジンが保証できるか (論点 7) | ADR-008 / `CpuModalBackend` | planner 裁定 | 保証するのは、(a) 8 要素境界でのレンジ分割、(b) 要素ごとの演算列がスカラーと同一、(c) スレッド数・jobs の有無・Debug/Release でビット一致、まで。速度は保証せず、計測して記録する。RyuJIT は自動ベクトル化をしないので、C# の CPU 版は (c) だけを約束する |
| D17 | カーネルのソースハッシュが改行コードで割れる | memory `script-editing-traps.md` (線種がファイルごとに違う) | planner 裁定 | BOM を除き CRLF→LF に正規化した後の FNV-1a 64bit。C++ の codegen (PowerShell)、エンジン、C# 変換で同じ値になることをテストベクタで確かめる |
| D18 | スクリプトが使うカーネルを静的に知る宣言は要るか | U6 でスクリプトのインスペクターに決まり、「まだ実行していないカーネル」も一覧に出すには静的な宣言が要る | **ユーザー 10-12 (U6) の帰結** — planner の当初案 (宣言を作らない) の逆 | **作る**。C++ は `REGISTER_SCRIPT` と一緒に書く `COMPUTE_KERNELS("A", "B")` (`MyeScriptDesc` の末尾に名前表を追加 = ABI 変更、同じ版 bump の中で行う)、C# はクラスの属性 `[ComputeKernels("A", "B")]`。宣言していないカーネルを呼んだら**実行せずに失敗**させる (ユーザー 10-12、U10。planner 案「実行して WARN」の逆)。判定に使う宣言の出所と時点は D28 |
| D19 | 観測と上書きの UI をどこに出すか | — | **ユーザー 10-12 (U6): 「スクリプトのインスペクター」** — planner 裁定 (`.kernel.hlsl` アセットのインスペクター) の逆 | エンティティのスクリプト欄に「Compute」の折り畳みを出す: 宣言 ∪ 実行時に観測したカーネルごとに、最終実行先・要素数・転送量・所要時間・版の一致・上書き。観測は実行時の対応表 (スクリプト型 × カーネル名) で持つ。`.kernel.hlsl` 側のインスペクターは作らない |
| D20 | HLSL サブセットの文法範囲 | C++ では匿名 union なしにスウィズルを作れない (匿名 union は UB) | ユーザー 10-12 (U7): 厳しめに絞る | 4.1.2 の表のとおり。U1 で C# も対象になったので、C++ と C# の両方で同じ演算列になる書き方だけを許す (D23) |
| D21 | Dispatch の上限 | D3D11 の 1 次元あたりスレッドグループ上限 65535 (記憶による)、numthreads 64 | planner 裁定 | 1 バッファあたりの要素数の上限は 4,194,240 |
| D22 | 番号の衝突 (memory では M75h が ABI v29 を予定) | `plans/m75-ugui.md:13`「番号は着手時の末尾から取る」 | planner 裁定 | 着手した時点の末尾から取る (ABI は sub-05、snapshot は sub-04、ADR は sub-09)。TypeId と kCookVersion は使わない |
| D23 | C++ / C# / HLSL で暗黙の型規則が違う。例: `1.0` は HLSL では float、C++ / C# では double。C++ は `float - 1.0` を double で計算する。C# は uint→int や float→int の暗黙変換を許さない。整数の 0 除算は C# では例外、C++ では UB | 言語仕様 (記憶による) | planner 裁定 (U7「厳しめ」の範囲内) | **サブセット検査器**を 1 つ作り、全バックエンドの前段で走らせる。違反があればそのカーネルはどの実行先でも失敗する。規則は 4.1.2 (float リテラルは `f` 付き必須、暗黙の縮小変換は禁止、整数の `/` `%` は 0 でない定数を右辺に置く場合だけ、ベクトル同士の比較とベクトルの三項演算子は禁止) |
| D24 | C# の CPU 版は RyuJIT でビット決定的か。C++ の CPU 版と同じビットになるか | x64 の RyuJIT はスカラー SSE で IEEE 単精度。自動 FMA 縮約はしない (記憶による・未確認)。`System.Numerics.Vector3.Dot` は SIMD の水平加算で、加算の順序がスカラーと違いうる | planner 裁定 (U1 で必要になった) | C# の互換型 (`float3` 等) は自前の struct で、成分の演算を左から右のスカラーで書く (`System.Numerics` は使わない)。`sin`/`cos` は C++ の `MyeSinRad` 系と同じ式を C# に移植する。**受け入れ条件: 同じ入力に対する C# の CPU 版と C++ の CPU 版の出力がバイト一致し、tiered JIT の有無 (`DOTNET_TieredCompilation=0`) でも一致する**。一致しない関数はホワイトリストから外す (C++ / HLSL 側でも禁止になる) |
| D25 | C# から呼ぶとき、GameLogic.dll に C++ の CPU 版があればそちらを使うか | — | ユーザー 10-12 (U9): 常に C# 版 | **C# の呼び出しは常に C# の CPU 版**を使う。C++ の呼び出しは常に C++ 版。GameLogic.dll の有無や DLL のホットリロード状態で C# の結果が変わらないようにするため |
| D26 | C# の CPU 版を並列に走らせる手段 | JobSystem は 1 つにまとめたい (負荷枠 `cpuMaxChunks` を共通にするため) | planner 裁定 | エンジンの JobSystem から `[UnmanagedCallersOnly]` のレンジ関数 (begin, end) を呼ぶ。生成コードは例外を投げない形にする (範囲外は 0、整数除算は D23 で制限)。`Parallel.For` は使わない |
| D27 | カーネルの上書き (Auto→CPU 固定) の粒度 | U6 でスクリプトのインスペクターに表示が決まった | ユーザー 10-12 (U8): カーネル単位 | **カーネル単位** (`project_settings.json` の `kernelOverrides`)。どのスクリプトのインスペクターで変えても、そのカーネルを使う全スクリプトに効く。UI にもそう書く |
| D28 | 宣言なしを失敗にするなら (U10)、宣言を Start 前やホットリロード直後に参照しても、一貫して判定できるか | C++ の宣言は `MyeScriptDesc` の静的データで、DLL を読み込んだ時点で揃う (Start の前)。C# の属性はアセンブリを読み込んだ時点でリフレクションで読める。BT タスク (`MyeBtTaskDesc`) にはスクリプト型が無い | ユーザー 10-12 (U10) の帰結。planner 裁定 | **判定に使う宣言は「いま読み込まれているモジュールの宣言表」**。(a) C++: DLL の読込 / ホットリロードで `MyeScriptModule` を取り込むのと同じ箇所・同じ時点で、エンジンが宣言表をコピーして差し替える。モジュールのコードと宣言表は必ず同じ世代になる。(b) C#: Roslyn のコンパイル後、スクリプトをインスタンス化する前に、属性を読み、レンジ関数と一緒に登録する (C# の再読込で外して付け直す)。(c) 呼び出し元の識別: C++ は ctx が指す実行中スクリプトの型名、C# は managed のスクリプト型。(d) 宣言表はスクリプトの Start より前に必ずあるので、Start の中から呼んでも同じ判定になる。名前は大文字小文字を区別して完全一致で比べる。(e) スクリプトのコールバックの外 (BT タスクなど、宣言を持てない呼び出し元) からの ForEach は失敗にする (U11)。(f) 宣言したがカーネルのファイルが無い場合は、読込時に WARN を 1 回出し、インスペクターに「宣言あり・カーネル無し」と表示する (呼べば失敗)。判定に使うのは静的な宣言とモジュールの世代だけなので、決定的になる (record と verify は同じ DLL で走る) |

### 2.1 参照した既存エンジンの前例

- Unity `AsyncGPUReadback`: 「数フレームの遅延と引き換えにストールしない」(https://docs.unity3d.com/2018.4/Documentation/ScriptReference/Rendering.AsyncGPUReadback.html)。D13 の根拠。
- UE Niagara: エミッタごとに Sim Target (CPUSim / GPUComputeSim) を切り替えられる (https://dev.epicgames.com/documentation/en-us/unreal-engine/how-to-create-a-gpu-sprite-effect-in-niagara-for-unreal-engine)。同じモジュールを VM と HLSL の両方へコンパイルする点は記憶による。D5 の「1 つのソースから複数のバックエンドへ変換する」と同じ形。
- Unity Burst `IJobParallelFor` (要素独立・バッチ分割) は記憶による。D4 / D16 と同じ形。
- 公式と違う点: Unity / UE はゲームプレイの決定論を保証しないので、CPU と GPU を自動で切り替えてよい。本エンジンは State を CPU に固定する (プロジェクトの契約が優先)。

## 3. スコープ

- やる:
  - `*.kernel.hlsl` (HLSL サブセット) を単一ソースとして、GPU / C++ の CPU 版 / C# の CPU 版で走らせる仕組み
  - サブセット検査器と、HLSL サブセットから C# への変換器
  - `ComputeForEach` のエンジン実装 (バッファストア、実行先の判断、負荷枠、Fx の upload / dispatch / 遅延 readback、ジョブ)
  - State バッファの SimSnapshot / WorldHash 組み込みと、所有エンティティによる寿命の管理
  - GameLogic.dll のカーネル表、`COMPUTE_KERNELS` 宣言、ABI の末尾追加、C++ 糖衣
  - C# の同形 API、構造体と CPU 版の生成、`[ComputeKernels]` 属性
  - ハッシュの照合と、不一致のときの実行制限
  - `.kernel.hlsl` のアセット扱い (meta、索引、ホットリロード)、スクリプトのインスペクターの Compute 欄、`project_settings.json`
  - デモ、compute A/B、`--compute-force-cpu`、ADR-030 と文書
  - **外部プロジェクト (HAL Collector / 三校など) の `<project>/assets/shaders/kernels` から C++ のカーネル表を生成し、エディタが作る `<project>/cache/GameLogic.vcxproj` に組み込む** (U12、ユーザー 10-12 の回答で M91 に含めた)
- やらない:
  - v21 の変更・統合 (D2)、WARP での代替 (D6)、ECS の列を直接渡すこと (論点 3)
  - `groupshared` / barrier / atomic / テクスチャ / スウィズル (D20)
  - 手書き SIMD の強制 (D16)、`.kernel.hlsl` アセットのインスペクター (D19)
  - C# レーンを Replay の被覆に入れること (D9)
- 後回し: State の非同期実行 (D10)、Fx の直接描画 (D12)、既存の粒子をカーネルへ移すこと

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 カーネル (`assets/shaders/kernels/<Name>.kernel.hlsl`)

```hlsl
#include "kernel_common.hlsli"
struct Particle { float3 pos; float3 vel; float life; };
KERNEL_PARAMS(ParticleUpdateParams) { float dt; float3 gravity; };
KERNEL(ParticleUpdate, ParticleUpdateParams, RW(Particle, particles) RO(Obstacle, obstacles))   // RW / RO の間にはコンマを書かない
{
    Particle p = particles[id];
    p.vel = p.vel + params.gravity * params.dt;
    p.life = p.life - 1.0f * params.dt;
    particles[id] = p;
}
```

- ファイル名 = カーネル名 = `KERNEL` の第 1 引数。1 ファイルに 1 カーネル。
- バッファ引数は位置の順に並べる。**`RW(...)` / `RO(...)` の間にはコンマを書かず、空白で並べる** (FXC のマクロは可変長引数を持てないため。sub-01 で `error X1500` を実測した)。第 1 引数は RW 必須で、その要素数が `id` の範囲になる。2 本目以降の RW は、第 1 引数以上の要素数が必要 (足りなければ失敗)。
- C++ の取り込み方: `#include "Shared/KernelCompat.h"` を先に置き、カーネルのファイルを `namespace mye::kernel::cpu::<Name> { #include "<Name>.kernel.hlsl" }` で包む (CRT の `::sin` などとの曖昧さを避けるため)。
- 予約語: `params` `id` `KernelId` `HashU32` `HashToUnitFloat` `FloatToInt` `FloatToUint` (カーネルの中で識別子に使えない)。
- 検査規則の一覧の正本は `src/Engine/Engine/Compute/KernelLint.h` の KL001〜 (4.1.2 の表は要約)。sub-02 で表より厳しくした規則 (1 引数のコンストラクタの禁止、整数リテラルの範囲、条件式は bool のみ、uint の単項マイナスの禁止、シフト量は 0..31 のリテラル、遮蔽の禁止、void の自由関数の禁止、min/max の混在は (ベクトル, スカラー) の順だけ、など) も正本に含む。
- 生成する C# は `namespace MyeKernels.<Name>` に入る (別のカーネルの同名 struct と衝突させないため)。
- GPU に展開すると `[numthreads(64,1,1)] CSMain` になり、範囲外の `id` は早期 return する。params は要素数 1 の StructuredBuffer で渡す。
- C++ の CPU 版は互換ヘッダを前に置いて C++ としてコンパイルする。C# の CPU 版は変換器が生成する。どちらも `id` は `KernelId` 型 (D4)。
- カーネルが守る約束 (ADR-028 と同じ): 自分の要素だけを書く、他の要素の今回の書き込みを読まない、実行順に依存しない、エンジンの API・RNG・ログを触らない。乱数が要るときは、互換層にある `uint` ハッシュに params の seed と `id` を渡して作る。

#### 4.1.2 HLSL サブセット (U7「厳しめ」)。違反は検査器がエラーにする (ファイル名・行・規則)

| 区分 | 使える | 使えない |
|---|---|---|
| 型 | `float` `float2/3/4` `int` `int2/3/4` `uint` `uint2/3/4`、およびこれらだけを持つ struct | `bool` 型の変数・メンバ、`double` `half` `min16*`、行列、配列 (ローカルもメンバも)、入れ子の struct、テクスチャ |
| 成分 | `.x` `.y` `.z` `.w` | スウィズル全般 |
| リテラル | float は `f` 付き (`1.0f`、`0.5f`)。整数の後ろに `u` を付けてよい | `f` の無い小数 (`1.0`)、指数表記を省いた曖昧な書き方は検査器が弾く |
| 変換 | int / uint → float の明示キャストと暗黙変換、int ↔ uint の明示キャスト (2 の補数のビットをそのまま)、**float → int / uint は `FloatToInt(x)` / `FloatToUint(x)` だけ** (NaN → 0、範囲外は飽和、0 方向への切り捨て。3 つのバックエンドで同じ式を互換層に持つ) | 暗黙の縮小変換 (float→int、int↔uint)、float → int / uint の C 形式のキャスト (`(int)x`。C++ では範囲外と NaN が UB で、C# / HLSL と結果が割れる) |
| 演算 | 四則、比較 (スカラー同士だけ)、論理 (`&&` `||` `!`)、三項演算子 (条件と結果がスカラー)、成分ごとの演算、スカラーとの混合、コンストラクタ `float3(a,b,c)`。ビット演算は整数だけ | ベクトル同士の比較、ベクトルの三項演算子、整数の `/` `%` (右辺が 0 でない整数リテラルの場合だけ可) |
| 関数 | `min max clamp abs saturate lerp step smoothstep floor ceil frac sqrt rsqrt rcp dot cross length normalize distance sign sin cos`。スカラーとベクトルを混ぜてよいのは `min max clamp lerp step smoothstep` だけ。`sin`/`cos` は、範囲外のときだけ `x - 2pi*floor(x/2pi + 0.5f)` で折り返してから `MyeSinRad`/`MyeCosRad` を呼ぶ (式の正本は sub-01 の実装メモ。C# も同じ式) | 上記以外。D24 でバイト一致が取れなかった関数は外す |
| 宣言 | 初期化子つきのローカル変数 (`float3 v = ...;`) | 初期化子のない宣言 (`T e;`。C++ / GPU では未定義の値。AGENTS.md §4.3「変数は宣言時に初期化」) |
| 制御 | `if` `for` `while` `break` `continue` `return`、自由関数 (引数は値渡しだけで、結果は戻り値で返す) | `out`/`inout` 引数、`groupshared`、barrier、`Interlocked*`、再帰、属性すべて (`[unroll]` を含む。C++ では通らない)。式の中での代入と `++` `--` (= `&&` `||` `?:` の項に副作用を置けない。GPU では両辺を評価しうるため) |
| 前処理 | `#include "kernel_common.hlsli"` だけ | それ以外の include、`#define` |

`rsqrt` / `rcp` は GPU では近似、CPU (C++ / C#) では厳密な `1/sqrt(x)` / `1/x`。整数の符号付きオーバーフローは書き手の責任 (C++ では UB なので、ハッシュ計算には `uint` を使う)。

#### 4.1.3 API (C++ と C# で同じ形)

```cpp
// C++ (GameLogic)
REGISTER_SCRIPT(Swarm, FIELDS(...));
COMPUTE_KERNELS(Swarm, "ParticleUpdate");                        // 宣言 (形は sub-05 で確定)
auto buf = Compute::CreateBuffer<Particle>(ctx, count, BufferKind::State);
buf.Write(ctx, particles);
auto job = Compute::ForEach(ctx, "ParticleUpdate", Execution::Auto, params, buf, obstacles);
job.Wait(ctx);
buf.Read(ctx, particles);
Compute::ForEach(ctx, particles, "ParticleUpdate", Execution::Auto, params);   // 配列を直接渡す糖衣 (一時的な State バッファを使う)
```

```csharp
[ComputeKernels("ParticleUpdate")]
class Swarm : MyeScript {
  ... var buf = Compute.CreateBuffer<Particle>(count, BufferKind.State);
      buf.Write(particles);
      var job = Compute.ForEach("ParticleUpdate", Execution.Auto, params, buf, obstacles);
      job.Wait(); buf.Read(particles);
      Compute.ForEach(particles, "ParticleUpdate", Execution.Auto, params);
}
```

違いは C++ の `ctx` だけ。名前・引数の順・戻り値・失敗したときの値は揃える。

#### 4.1.4 実行先の判断 (決定表)

「CPU 版」は、呼び出し元が C++ なら GameLogic.dll の C++ 版、C# なら変換で作った C# 版を指す (D25)。

| バッファ種別 | Execution | 条件 | 実行先 |
|---|---|---|---|
| 1 つでも State | Auto / Cpu | 検査に合格 ∧ CPU 版あり ∧ ハッシュ一致 | CPU (同期) |
| 1 つでも State | Gpu | — | 拒否 (0 + ログ) |
| 1 つでも State | 任意 | 検査で違反 / CPU 版なし / ハッシュ不一致 | 失敗 (0 + ログ + インスペクターに表示) |
| 全部 Fx | Auto | 検査に合格 ∧ デバイスあり ∧ 要素数 ≥ `gpuMinElements` ∧ tick の GPU 枠内 ∧ (ハッシュ一致 ∨ CPU 版なし) | GPU (非同期) |
| 全部 Fx | Auto | 上が偽で、CPU 版あり ∧ ハッシュ一致 | CPU (同期) |
| 全部 Fx | Auto | ハッシュ不一致 ∧ デバイスあり | GPU 固定 + WARN |
| 全部 Fx | Cpu | CPU 版あり ∧ 一致 / それ以外 | CPU / 失敗 |
| 全部 Fx | Gpu | デバイスあり / なし | GPU / CPU で代替 + WARN (1 回だけ)。CPU 版も無ければ失敗 |
| 全部 Fx | 任意 | resim 中 | Skipped |
| 任意 | 任意 | `--compute-force-cpu` | Fx も CPU |
| 任意 | 任意 | `kernelOverrides` で CPU 固定 | Auto を Cpu として扱う |
| 任意 | 任意 | 書き込み先 (RW) に State があり、かつ引数に Fx が 1 つでも混ざる | **失敗** (MixedKinds。Fx の CPU ミラーには GPU の結果が入っているので、それを State の計算に使うと GPU の結果が sim に漏れる — D11 を入口で塞ぐ)。State を RO で読んで Fx に書くのは可 |
| 全部 Fx | Auto | CPU 版なし | `gpuMinElements` と tick 枠を見ずに GPU (デバイスが無ければ失敗) |
| 任意 | 任意 | 呼び出し元のスクリプトがそのカーネルを宣言していない、または呼び出し元がスクリプトではない (D28) | **失敗** (0 + ログを 1 回 + インスペクターに表示)。この判定は他のすべての行より先に行う (U10) |

負荷枠と `gpuMinElements` が効くのは Auto だけ (明示の Gpu / Cpu は利用者の意図を優先する)。引数の誤り (同じバッファを RW を含めて重複して渡す、params の大きさ違い、stride 違い、2 本目以降の RW が短い) は失敗し、バッファに触らない。Fx の GPU 実行で dispatch が失敗したときは、CPU 版があれば CPU で代わりに走らせる (WARN 1 回)。Fx を CPU で走らせるとき、GPU 側に未反映の結果があれば待つ (明示ストールとして Wait の回数に積む。Auto が CPU と GPU を行き来すると毎回起きうるが、観測できるので許容する)。
判断に使う値は全部決定的で、時間の計測は判断に使わない。失敗したときのジョブハンドルは 0。同期で完了したジョブは定数 1、Skipped は定数 2、GPU の非同期ジョブだけが連番 (0x100 + 通し番号)。**Fx のジョブハンドルは実行先で値が変わるので、ハッシュ対象の FIELD に保存してはいけない** (ScriptAPI.h / MyeScript.cs に注記する。compute A/B が検出する)。同じ理由で、**Fx のジョブの `Status()` / `Target()`、Fx の `Read` 結果、Fx のバッファハンドルを、sim の分岐 (バッファの作成・解放を含む) に使ってはいけない**。State と Fx は 1 つのスロット空間を共有するので、Fx の作成・解放が実行先に依存すると State のハンドル値まで割れる。Fx の作成そのものはデバイスに依存しない (CPU ミラーだけを確保し、GPU バッファは最初の GPU 実行で遅れて作る) ので、この規約を守る限り採番は決定的。`Status()` は Done / Pending / Failed / Skipped、`Target()` は Cpu / Gpu / None を返す。

#### 4.1.5 バッファと転送
- `CreateBuffer(count, stride, kind)` は 64bit のハンドルを返す (世代 + スロット、最小の空きスロットから採番。世代の上位 2bit はストアの印で、他のストアや v21 のハンドルを弾く)。stride は 4 の倍数で 2048 以下 (D3D11 の構造化バッファの上限)、1 本 128MiB 以下、スロットは 1 ストアあたり 1024。count が 0、上限超過 (D21)、ストアが満杯、State の総バイト枠を超える場合は 0。stride がカーネルの struct の大きさと違えば ForEach が失敗する。
- State は CPU 配列だけを持ち、`Write` / `Read` はその場でコピーする。Fx は CPU ミラーと GPU バッファを持ち、`Write` すると dirty になって GPU で実行する前に upload する。`Read` は最新の完了済み結果を返す (D13)。
- デバイスを失ったとき (M88) は、Fx の GPU バッファを CPU ミラーから作り直す。
- バッファは作成したスクリプトのエンティティが所有する (D8)。v21 のハンドルとは別の空間。
- ストアは 2 つ: GameLogic 用 (State がスナップショット / ハッシュの対象) と C# レーン用 (NoHash) (D9)。

#### 4.1.6 版ずれ (ハッシュ) とホットリロード
- C++: GameLogic のビルド時に codegen がハッシュを `Kernels.gen.cpp` のカーネル表へ埋める。エンジンはディスク上のファイルのハッシュと照合する。DLL の読込 / ホットリロードと FileWatcher のときに評価し直す。
- C#: エンジンが変換するときに、そのときのハッシュを `Kernels.gen.cs` に埋める。`.kernel.hlsl` が変わったら、C# の再生成と再コンパイルを要求する。再コンパイルが終わるまでは不一致として扱う。
- GPU: `.kernel.hlsl` が変わると、既存のシェーダのホットリロードで作り直す。
- 不一致のときは 4.1.4 のとおり。コンソールにエラーを 1 回出し、インスペクターに「CPU 版が古い — GameLogic を再ビルド」(C++) または「C# の再コンパイル待ち」(C#) と出す。

#### 4.1.7 スクリプトのインスペクター (U6)
- エンティティのスクリプト欄 (C++ の FIELDS スクリプトと C# スクリプトの両方) に「Compute」の折り畳みを出す。行 = 宣言したカーネル ∪ 実行時に観測したカーネル (名前の昇順)。
- 列: カーネル名 / 状態 (未実行・実行済み・**宣言なしで失敗**・宣言あり・カーネル無し・検査違反) / 最終の実行先 (CPU / GPU / 代替 / 失敗 / Skipped) / 要素数 / upload と readback のバイト数 / CPU の ms (GPU は回数) / 版 (一致・不一致・CPU 版なし) / 上書き (Auto / CPU 固定、カーネル単位であることを表示 — U8)。
- 観測の対応表は (スクリプト型名, カーネル名) をキーにした表示専用のデータ。sim の状態ではないので、スナップショットにもハッシュにも入れない。

### 4.2 データ・保存形式・互換性
- **ABI** (sub-05 に着手した時点の次の版。2026-10-12 時点なら v29): 末尾に追加するだけ。ForEach 用のスロット群 (7〜9 本程度。C# のレンジ関数ポインタを登録するスロットを含む。確定は sub-05)。`MyeScriptModule` の末尾にカーネル表、`MyeScriptDesc` の末尾にカーネル宣言の名前表を足す (`ScriptTypes.h:35` の注記に従って同じ bump の中で)。`Interop.cs` の位置ミラーと check_rules の規則 11 も合わせる。
- **SimSnapshot** (sub-04 に着手した時点の次の版。今なら v49): 新節「ComputeState」を追加。
- **WorldHash**: `HashSources` に ComputeState を足し、`HashWorldDetailed` にも出す。
- **configBits**: `kCfgComputeForceCpu = 1u << 5`。
- **project_settings.json**: `"compute": { "gpuMinElements": 4096, "cpuMaxChunks": 0, "fxUploadBytesPerTick": 8388608, "fxDispatchPerTick": 64, "stateTotalBytes": 16777216, "kernelOverrides": { "<Name>": "cpu" } }`。キーが無ければ既定値、不正な値なら既定値 + WARN。
- **生成物**: C++ は `src/GameLogic/Generated/Kernels.gen.{h,cpp}` (`gen_project_files.ps1` から)。C# は `assets/scripts/Generated/Kernels.gen.cs` (エンジンが `SchemaCodegen` と同じ流儀で生成)。**C++ の生成物 (Kernels.gen.{h,cpp}) は版管理する** (sub-05 で変更。gen_project_files.ps1 を実行していない新規クローンでも GameLogic.vcxproj が参照する .cpp が存在するようにするため。ハッシュを含むので、カーネルを編集した後に再生成しなければ版ずれとして検出される)。C# の生成物の扱いは Schema.gen.cs と同じ。
- 既存の .rep は snapshot の版が上がるので録り直しが要る。v21 の利用者には影響しない。

### 4.3 UI / ビジュアル
- スクリプトのインスペクターの Compute 欄 (4.1.7)。文字列は `Tr()` で日英両方。
- デモ `--compute-demo`: State の群れ (C++、CPU) と Fx の火花 (GPU、DebugDrawLine)。C# の版も 1 つ置く (C# の CPU 版の実走)。

### 4.4 非機能
- 決定論: State (C++) の結果は、Debug / Release / jobs の有無 / ワーカー数 / CI(WARP) でビット一致。compute A/B で Auto と force-cpu のワールドハッシュが一致する。
- C# の CPU 版は C++ の CPU 版とバイト一致する (D24)。C# レーンは Replay の被覆外のままなので、検証はプローブまたは selftest で行う。
- `/fp:precise` を維持し、`_DEBUG` で結果が変わる分岐を作らない。ForEach をジョブの中から呼ぶと失敗する。
- 性能は計測して記録する (1M 要素の jobs あり/なし、C++ と C# の CPU 版の比較、Fx の CPU/GPU 誤差)。

## 5. 受け入れ条件

1. 同じ `*.kernel.hlsl` が FXC と MSVC の両方でコンパイルでき、GPU (CI では WARP) と C++ の CPU 版の差が許容差内に収まる。実測値を ADR-030 に記録する。— `Editor.exe --selftest` (Debug / Release)
2. C++ の CPU 版の結果ハッシュが、Debug / Release / `--no-jobs` / チャンク数違いで同じ固定値になる。— selftest
3. `RW` を `id` 以外で添字すると、C++ でも C# でもコンパイルエラーになり、検査器も弾く。`RO` の範囲外は 0 を返す。— selftest + 手順の記録
4. 4.1.4 の決定表の全行が selftest で確かめられる。
5. State バッファがスナップショットの保存→復元で中身・採番・所有ごと戻り、WorldHash に入る。所有エンティティを破棄すると解放される。— selftest + replay_verify の round-trip
6. Fx の GPU 経路で、`Read` が遅延した結果を返し、`Wait` で最新になる。デバイスを失って復旧した後も動く。— selftest
7. C++ スクリプトから `Compute::ForEach` と `COMPUTE_KERNELS` が ABI 経由で動き、DLL のホットリロード後もハッシュが合い直す。— selftest + check_rules
8. **(U1 で変更)** C# のスクリプトから同じ形の API でカーネルが動く。**GameLogic.dll にカーネルが無い状態でも、C# の CPU 版で State の ForEach が成功する**。C# のバッファは NoHash。— プローブまたは selftest
9. `tools\check_rules.ps1` が PASS する。
10. `tools\replay_verify.bat` が compute A/B を含めて PASS する。
11. **(U1 / U6 で変更)** `.kernel.hlsl` を編集すると、GPU 版がホットリロードされ、スクリプトのインスペクターに「不一致」が出て、C++ の State の ForEach が失敗する。GameLogic を再ビルドすると一致に戻る。C# の側は再生成・再コンパイルで一致に戻る。— 手動確認 (test_checklists) + selftest
12. `--compute-demo --screenshot` で群れと火花が写る。— スクリーンショット (Release)
13. ADR-030・engine_spec・test_checklists が実装と一致する。
14. **(新規、U1)** サブセット検査器が 4.1.2 の違反をファイル名・行・規則つきで弾き、違反したカーネルはどの実行先でも失敗する。変換器は selftest のカーネルを C# に変換し、その出力が期待するテキスト (golden) と一致する。— selftest
15. **(新規、U1 / D24)** 同じ入力に対して、C# の CPU 版と C++ の CPU 版の出力がバイト一致する。tiered JIT の有無でも一致する。ホワイトリストのすべての関数を通るカーネルで確かめる。— プローブまたは selftest。結果を ADR-030 に記録する
16. **(新規 U6、U10 で変更)** スクリプトのインスペクターの Compute 欄に、宣言したカーネルと実行時に観測したカーネルが、状態・実行先・要素数・版・上書きつきで並ぶ (C++ と C# の両方)。宣言していないカーネルを呼ぶと**実行されずに失敗**し (戻り値 0、ログ、「宣言なしで失敗」の表示)、バッファの中身は変わらない。Start の中からの呼び出しと、ホットリロード直後の最初の tick の呼び出しも、同じ判定になる。— selftest (C++ / C# の両方。C# はプローブでも可) + スクリーンショット
17. **(新規、U12)** 外部プロジェクトの構成 (`<project>/assets/shaders/kernels` と `<project>/src/GameLogic/Scripts`) で、エディタの [Rebuild Scripts] の経路 (`PrepareProjectScriptsBat`) がカーネル表を生成して GameLogic.dll に組み込み、C++ スクリプトから State の ForEach が動く。同じ入力に対して、C++ の生成器と PowerShell の生成器 (sub-05) のカーネル表 (名前・ハッシュ・引数名・RW/RO・stride) が一致する。— selftest (生成) + 一時プロジェクトでの実ビルドと実走 (手順と結果を記録)

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | 単一ソースのカーネル (互換ヘッダ・KERNEL 展開・C++ の CPU 実行器・ハッシュ) | なし | 1, 2, 3 (C++ 側) | `M91a: *.kernel.hlsl を GPU と CPU の両方で走らせる互換ヘッダと実行器を作る` |
| sub-02 | サブセット検査器と、C# への変換器 | sub-01 | 3 (検査器), 14 | `M91b: カーネルの HLSL サブセット検査器と C# への変換器を作る` |
| sub-03 | ForEach のエンジン核 (ストア 2 つ・実行先の判断・Fx の GPU 経路・ジョブ・対応表) | sub-01 | 4, 6 | `M91c: Compute.ForEach のエンジン側 (State/Fx バッファ・実行先の判断・Fx の遅延読み戻し)` |
| sub-04 | State バッファのスナップショット・ハッシュ・寿命 | sub-03 | 5 | `M91d: State バッファを SimSnapshot とワールドハッシュに入れる (snapshot v49)` |
| sub-05 | GameLogic のカーネル表・COMPUTE_KERNELS・ABI・C++ 糖衣・版ずれ制限 | sub-02, sub-04 | 7, 9 | `M91e: GameLogic のカーネル表と ForEach の ABI を足す (ABI v29)` |
| sub-10 | 外部プロジェクトの C++ カーネル表 (エディタのスクリプトビルドへの組み込み) | sub-05 | 17 | `M91f: 外部プロジェクトの kernels から C++ のカーネル表を生成して GameLogic に組み込む` |
| sub-11 | net の selftest の切り分け (M91 の退行ではない) と、CRLF のチェックアウトで落ちるカーネルの試験の修正、net の selftest にファイルを開くリトライ (U13) | なし (sub-10 の後に行う) | — (品質。R11 / R12) | `M91 追補: CRLF のチェックアウトでカーネルの selftest が落ちるのを直し、net の selftest にファイルを開くリトライを足す` |
| sub-12 | リポジトリ全体の改行を .gitattributes で統一する (既定 LF、.bat/.cmd と VS のプロジェクトは CRLF、バイナリは明示。renormalize と再チェックアウト。replay_verify と golden の再確認) | sub-11 (作業ツリーが clean であること) | — (品質。R12 の再発防止、U14) | `M91 追補: リポジトリ全体の改行を .gitattributes で統一する (テキストは LF、.bat と VS のプロジェクトは CRLF)` |
| sub-13 | エンジン・エディタ・ツールが書くテキストの改行を規則に揃え、作業ツリーの見張りを有効にする | sub-12 | — (品質。U14) | `M91 追補: エンジンとツールが書くテキストの改行を .gitattributes の規則に揃える` |
| sub-06 | C# の同形 API・C# の CPU 版の実行・[ComputeKernels]・RyuJIT の決定論 | sub-05 | 3 (C# 側), 8, 15 | `M91g: C# から Compute.ForEach を呼べるようにし、C# の CPU 版を走らせる` |
| sub-07 | カーネルのアセット扱い・スクリプトのインスペクターの Compute 欄・project_settings | sub-06 | 11, 16 | `M91h: スクリプトのインスペクターに Compute 欄を足し、実行ポリシーと負荷枠を設定にする` |
| sub-08 | デモと compute A/B (`--compute-force-cpu`) | sub-06 | 10, 12 | `M91i: Compute デモと replay_verify の compute A/B を足す` |
| sub-09 | ADR-030 と文書 | sub-07, sub-08, sub-10 | 13 | `M91j: Compute.ForEach の ADR-030 を書き、engine_spec と検証表を更新する` |

sub-02 と sub-03 は互いに依存しない (並列にできる)。sub-07 と sub-08 も互いに依存しないが、`LocalizationTable.inl` や `EngineCli.cpp` を重ねて触るなら直列にする。

## 7. 未決事項・リスク

### 7.1 ユーザーの回答 (2026-10-12) と、残る `[ユーザーに聞ける]`
- 回答済み: U1 = C# 用の CPU 版も作る (planner 案の逆。D5 / D9 / D23〜D26 に反映)、U2 = WARP は使わない、U3 = エンジン所有のストア、U4 = 同期、U5 = 読み戻し経由だけ、U6 = スクリプトのインスペクター (planner 案の逆。D18 / D19 / D27 / 4.1.7 に反映)、U7 = 厳しめに絞る。
- 回答済み (2026-10-12、2 回目): U8 = カーネル単位、U9 = 常に C# 版、U10 = 失敗にする (planner 案の逆。D18 / D28 / 4.1.4 / 受け入れ 16 に反映)。
- `[ユーザーに聞ける]` U11 (D28-e): スクリプトのコールバックの外 (BT タスクの `MyeBtTaskDesc` など、宣言を持てない呼び出し元) からの ForEach の扱い / 裁定: 失敗にする (U10 の「宣言の無い呼び出しは失敗」と一貫させる) / 逆 (BT タスクにも宣言の欄を足して許す) にすると: `MyeBtTaskDesc` の末尾に追加する分の ABI が sub-05 に増え、インスペクターの表示先 (BT タスクには窓が無い) も決め直すことになる

- 回答済み (2026-10-12、3 回目): U12 = M91 でやる (planner 案「後回し」の逆)。sub-10 を新設した。
- 回答済み (2026-10-12、5 回目): U14 = リポジトリ全体 (planner 案「M91 の範囲だけ」の逆)。sub-12 をリポジトリ全体へ広げ、sub-13 を新設した。
- `[ユーザーに聞ける]` U15: 外部プロジェクト (HAL Collector / 三校) のリポジトリの改行も揃えるか / 裁定: 範囲外。別のリポジトリで、このリポジトリからは変えられない。エンジンが外部プロジェクトの管理対象に書くファイル (シーン、.meta、設定、Kernels.gen.cs) だけを LF で書く (sub-13) / 逆にすると: 各リポジトリに同じ .gitattributes を入れて再チェックアウトする作業 (リポジトリごとに 1 コミット) が、M91 の外で要る
- 回答済み (2026-10-12、4 回目): U13 = いま足す (planner 案「足さない」の逆)。sub-11 の追補コミットに、ServerNetSelfTest.cpp のリトライを含める。

### 7.2 リスク
- R1 (sub-01): 同じテキストが HLSL と C++ の両方で通るか。sub-02 の検査器の規則で、通らない書き方を禁止に回す。
- R2 (sub-08): DebugDrawLine が Runtime の `--screenshot` に写るか。
- R3 (sub-03): スクリプトの Update がジョブの中から呼ばれる経路が無いこと。
- R6 (sub-06、新規): RyuJIT でバイト一致が取れない関数がある可能性。その関数はホワイトリストから外し、検査器と spec 4.1.2 を直す (変更履歴に残す)。
- R7 (sub-06、新規): JobSystem のワーカーから managed のレンジ関数を呼ぶとき、スレッドのアタッチと GC の停止待ちに時間がかかる。速度を計測して記録する。正しさが崩れる場合 (例外・デッドロック) は C# の CPU 版を呼び出しスレッドだけで直列に走らせる案へ退避する (結果は同じビット)。
- R8 (sub-02): 変換器の構文解析の範囲がサブセットより広がると肥大化する。サブセットの外は解析せずに検査器でエラーにする。
- R9 (sub-02 で判明): 符号付き整数の + - * のオーバーフローは、C++ では UB、C# では巻き戻り。検査器では静的に弾けないので書き手の責任とし、オーバーフローしうる計算 (ハッシュなど) は `uint` で書く規約を ADR-030 と kernel_common.hlsli の冒頭に書く。MSVC は実際には巻き戻るので、C++ と C# のバイト一致の selftest (sub-06) で見張る。
- R10 (sub-05 で判明): 登録表 (プロセス共有) の construct を「最後に読んだ DLL」へ差し替える修正は、複数の ScriptHost が同時に生きている場合 (ServerNetSelfTest の 4 sim) に、先に解放された DLL を指す可能性が残る (既存の弱点)。エディタと Runtime と Server は ScriptHost が 1 つなので実害は selftest に限られる。M91 の範囲外として記録する。
- R11 (sub-11 で判明): Debug の selftest の net 試験 (V1 LoadPersist / LoadGame) が、まれに `cannot open` で FAIL する (HEAD 8 回中 2 回、基点 42c6b89 では 5 回中 0 回。有意差なし。原因は書いた直後のファイルを開けないことで、ScriptHost とは無関係と見る)。M91 の退行ではないと判断した。**ユーザーの回答 (U13 = いま足す) により、sub-11 で selftest の側に、ファイルが開けるまで待つリトライを足す** (製品コードの Replay.cpp などは変えない)。
- R12 (sub-11 で判明): core.autocrlf=true の新規チェックアウト (worktree、CI の Windows ランナー) ではカーネルのファイルが CRLF になる。selftest の「CRLF 化してもハッシュは同じ」が、すでに CRLF の入力にさらに CR を足して CRCRLF を作り、基点の worktree で 5 回とも FAIL した。ハッシュの正規化 (D17) は正しく、試験の側の誤り。生成物の比較 (sub-10 の C++ 版と PowerShell 版の比較など) も改行に依存していないか、新規チェックアウトで確かめる (sub-11 round 2)。
- R5: 解消 (DeepModal の未コミット変更は、司会が M91 の前に別コミットにする — 2026-10-12)。

## 8. 変更履歴
- 2026-10-12 (ユーザー回答・司会経由): 状態を「確定」にした。
  - U1 の逆 (C# 用の CPU 版を作る): D5 を書き直し、D9 / D23〜D26 を追加。受け入れ条件 8 を変更 (C# だけでも State が成功する)、14 と 15 を新設。スコープに検査器と変換器を追加。サブを再編 (sub-02 の検査器・変換器を新設し、旧 sub-02〜08 を sub-03〜09 へ繰り下げ。旧 sub-05 の C# は sub-06 で CPU 版の実行まで広げる)。理由: C# だけのプロジェクトでも CPU 版が要るというユーザーの判断。
  - U6 の逆 (スクリプトのインスペクター): D18 / D19 を書き直し、D27 と 4.1.7 を追加。受け入れ条件 11 を変更し、16 を新設。`.kernel.hlsl` アセットのインスペクターはスコープから外した。`COMPUTE_KERNELS` / `[ComputeKernels]` の宣言と実行時の対応表を追加 (MyeScriptDesc の末尾追加 = ABI の同じ bump に含める)。
  - U2〜U5 と U7 は裁定どおりで確定。U7 の範囲として 4.1.2 に、f 付きリテラル・縮小変換・整数除算・ベクトル比較の規則を追加 (D23)。
  - R5 を解消扱いにした。
- 2026-10-12 (ユーザー回答 2 回目・司会経由): U8 (カーネル単位) と U9 (常に C# 版) は裁定どおり。U10 は planner 案「実行して WARN」の逆で、「宣言なしは失敗」にした。D18 を書き直し、D28 (宣言の出所と時点、スクリプトではない呼び出し元) を追加。4.1.4 の最後の行を「失敗、他の判定より先」に変更。4.1.7 の状態と受け入れ条件 16 を変更。U11 を新設 (裁定済み・仮置き)。サブ: sub-03 (判断の行と順序)、sub-05 (宣言表の取り込み時点)、sub-06 (C# の属性を登録する時点)、sub-07 (表示)、sub-08 (デモのスクリプトに宣言を書くこと) を更新。
- 2026-10-12 (sub-01 round 1 の SELF_EVAL、coder の指摘を planner が採用): 4.1.1 の KERNEL の書式を「RW/RO をコンマなしで並べる」に変更した (FXC に可変長マクロが無いことを実測)。C++ の取り込み方 (名前空間で包む) を明記し、予約語と、2 本目以降の RW の要素数の条件を追加した。4.1.2 を変更: `[unroll]` と `out`/`inout` を禁止 (C++ で同じ綴りにできない)、式の中の代入と増減を禁止、スカラー混在を 6 関数に限定、sin/cos の範囲外の折り返しを明記。判定: 仕様の側の誤り (spec の例が FXC で成立しなかった)。
- 2026-10-12 (sub-02 round 1 の SELF_EVAL、coder の質問を planner が裁定): 4.1.2 を変更。float → int / uint は `FloatToInt` / `FloatToUint` の互換関数だけ (C 形式のキャストは禁止。C++ の UB と C# の 0x80000000 で結果が割れるため)。初期化子のない宣言を禁止 (AGENTS §4.3)。検査規則の正本を KernelLint.h に置き、表より厳しい規則も採用。生成 C# の名前空間を明記。符号付き整数のオーバーフローは書き手の責任のまま (7.2 R9)。
- 2026-10-12 (sub-03 round 1 の SELF_EVAL。coder の追加を planner が採用): 4.1.4 に MixedKinds の失敗、「Auto で CPU 版が無いときは枠を見ない」、枠は Auto だけ、引数誤りの失敗、dispatch 失敗時の CPU 代替、Fx の CPU 実行で GPU を待つことを追記した。4.1.5 に作成の上限の具体値とストアの印を追記した。ジョブハンドルの値の規約と、Fx のジョブハンドルを FIELD に保存しないことを追記した。判定: 仕様の不足。
- 2026-10-12 (sub-04 round 1 の SELF_EVAL、coder の質問を planner が裁定): State と Fx はスロット空間を共有したままにした (ストアを種別ごとに分ける案は却下。Fx の作成はデバイスに依存しないので採番は決定的。分けるとハンドルの種別印とスナップショットの形が増える)。そのかわり 4.1.4 に「Fx の Status / Target / Read / ハンドルを sim の分岐に使わない」規約を追記した。ReleaseAll で世代も初期化すること、復元では stateTotalBytes を見ないこと、Fx はスナップショットに居た印と世代だけを入れること、を採用した。
- 2026-10-12 (sub-05 round 1 の SELF_EVAL): ABI v29 = 174 スロット (ForEach 系 8 本 + C# の CPU 版と宣言を登録する 2 本)。ComputeForEach に callerScript 引数 (C# 用。C++ レーンではエンジンが実行中のスクリプトから決め、引数は信用しない)。宣言表を言語別にした。C++ の生成物を版管理にした (4.2)。外部プロジェクトの kernels の生成を後回しにした (3、U12)。判定: 仕様の不足と、現実の制約。
- 2026-10-12 (ユーザー回答 3 回目、U12 = M91 でやる。planner 案「後回し」の逆): 外部プロジェクトの C++ カーネル表の生成をスコープに入れ、受け入れ条件 17 と sub-10 を新設した。sub-10 は sub-05 の次に行う (M91f)。これに合わせて、以降のコミット件名の文字を 1 つずつ繰り下げた (sub-06 = M91g、sub-07 = M91h、sub-08 = M91i、sub-09 = M91j)。sub-09 の依存に sub-10 を追加した。
- 2026-10-12 (sub-10 round 1 の SELF_EVAL): Debug の selftest の net 試験 (V1 LoadPersist / LoadGame) が 1 回目だけ FAIL した。R10 (M91e の登録表の construct の差し替え) の退行が疑われるので、切り分けのために sub-11 を新設した。外部プロジェクトの生成は、include を絶対パスにすること、カーネル名とファイル名の不一致は検査器 (KL020) で表から外すこと、を採用した。
- 2026-10-12 (sub-11 round 1): net の selftest の不安定さは M91 の退行ではないと判断した (R11、U13)。切り分けの途中で、新規チェックアウト (CRLF) で kernel の selftest が落ちることが分かった (R12)。sub-11 の round 2 で直す。
- 2026-10-12 (ユーザー回答 4 回目、U13 = いま足す。planner 案の逆): R11 のリトライを sub-11 に入れた。仕様は sub-11.md の「追加 (U13)」節に書いた。
- 2026-10-12 (ユーザーの追加依頼「subを追加テストの改行コードを統一」): sub-12 を新設した (R12 の再発防止)。範囲は M91 が持ち込んだテキストにとどめ、リポジトリ全体に広げるかは U14 として裁定を仮置きした。sub-11 (実行中) とは、個別の試験の修正 = sub-11、リポジトリの規則と見張り = sub-12、と分けた。
- 2026-10-12 (ユーザー回答 5 回目、U14 = リポジトリ全体。planner 案の逆): sub-12 をリポジトリ全体の改行の統一に広げた。規則は、既定が `* text=auto eol=lf`、例外が .bat / .cmd と VS のプロジェクト (CRLF)、バイナリを明示。renormalize の結果は .gitattributes と同じ 1 コミットに入れ、作業ツリーが clean のときに再チェックアウトする。受け入れ条件に replay_verify と golden の再確認を入れた。エンジンとツールの書き込みを揃える sub-13 を新設した。外部プロジェクトの扱いは U15 として裁定を仮置きした。
