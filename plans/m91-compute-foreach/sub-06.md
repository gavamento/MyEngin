# sub-06: C# の同形 API・C# の CPU 版の実行・[ComputeKernels]・RyuJIT の決定論

- 依存: sub-05
- 状態: 未着手
- 往復: 0
- 更新: 2026-10-12 (U10「宣言なしは失敗」と、属性を登録する時点)
- 再編: 2026-10-12 (旧 sub-05 を U1 の反映で拡張。C# の CPU 版をここで実際に走らせる)

## やること
spec 4.1.3 (C#) / 4.1.6 (C#) / 4.1.7 (C# の宣言)、D9 / D24 / D25 / D26、受け入れ 3 (C# 側) / 8 / 15。ABI は変えない (sub-05 で確保したスロットを使う)。

1. 生成の接続: C# をコンパイルする前に、sub-02 の変換器で `Kernels.gen.cs` を生成する。`.kernel.hlsl` が変わったら、再生成と C# の再コンパイルを要求する (それまでは不一致として扱う)。
2. C# 版の登録: コンパイルした後、`[UnmanagedCallersOnly]` のレンジ関数のポインタとハッシュを、sub-05 のスロットでエンジンへ登録する。エンジンはそれを C# からの呼び出し専用の CPU 版として扱う (D25)。C# を再読込するときに、登録を外してから付け直す。
3. 公開 API (`MyeScript` から): `Compute.CreateBuffer<T>` / `Buffer<T>` / `Compute.ForEach` / 配列を直接渡す糖衣 / `Job`。C++ と同じ名前・引数の順・戻り値にする。`T : unmanaged`、pin して渡す。バッファは C# 用のストアに置く。
4. 宣言: `[ComputeKernels("A", "B")]` 属性を managed 側が読み、スクリプト型ごとにエンジンへ渡す。**Roslyn のコンパイル後、スクリプトをインスタンス化する前に、レンジ関数と同じ時点で登録する。C# を再読込するときは、外してから付け直す** (spec D28-b)。宣言していないカーネルを呼ぶと失敗する (U10)。
5. 決定論の検証 (D24): sub-01 の自己テストカーネル群 (ホワイトリストのすべての関数を通る 1 本を含む) を、同じ入力で C++ の CPU 版と C# の CPU 版で走らせ、**出力のバイト一致**を確かめる。既定の JIT と `DOTNET_TieredCompilation=0` の両方で行う。
   常設の selftest にできるならそうする。C# レーンが selftest で動かない場合は temp プローブ (memory `abi-bump-verification.md`) で行い、手順と結果を実装メモに書く。
   一致しない関数が出たら、その関数をホワイトリストから外す案を SELF_EVAL の「仕様との差分」に出す (spec R6)。
6. 速度を計測する (spec R7): C++ 版と C# 版の 1M 要素の ms。ワーカーから managed を呼ぶことで正しさが崩れる場合は、直列にする退避案を「仕様との差分」に出す。

## やらないこと (このサブでは)
- インスペクター (sub-07)

## 触る場所 (planner の見立て)
- `src/Scripting/MyeScript.cs`、`src/Scripting/Interop.cs`、`src/Engine/Engine/Script/ManagedHost.{h,cpp}` (コンパイルの前後、再読込)
- `src/Engine/Engine/Schema/SchemaCodegen.*` の呼び出し点に並べる

## 受け入れ条件 (このサブ)
1. spec 5-8: C# スクリプトから State / Fx の ForEach が動く。**GameLogic.dll にカーネルが無くても C# の State が成功する**
2. spec 5-15: C# 版と C++ 版の出力がバイト一致する (tiered JIT の有無の両方)
3. spec 5-3 (C# 側): `RW` を `KernelId` 以外で添字すると C# でコンパイルエラーになる (手順を記録)
4. spec 5-9: `tools\check_rules.ps1` が PASS する
5. spec 5-16 の C# 部分: 宣言していないカーネルを C# から呼ぶと失敗する。C# の再読込直後と Start の中からの呼び出しでも同じ判定になる

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest`、`tools\check_rules.ps1`、プローブ (使ったら残さない)

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL)
実装:
- src/Scripting/Compute.cs (新規): 公開 API。`BufferKind` / `Execution` / `JobStatus` / `JobTarget`、`[ComputeKernels("A","B")]`、`Job` (Handle/Ok/Status/Target/Wait)、`Buffer<T>` (FromHandle/Valid/Handle/Count/Write/Read/Release)、`Compute.CreateBuffer<T>(count, kind)`、`Compute.ForEach(kernel, execution, in params, buffers 1〜4 本)`、配列を直接渡す糖衣 `Compute.ForEach(T[] data, kernel, execution, in params, extra 0〜2 本)`。C++ (ScriptCompute.h) と名前・引数の順・戻り値が同じで、違いは ctx だけ。Fx のハンドル/Status/Target/Read を sim の分岐や FIELD に使うなという注記を冒頭に書いた
- 同: `ComputeCaller` — Start/Update/LateUpdate/Trigger/Collision/Break のコールバックの間だけ、呼び出し元のスクリプト型 (FullName) とエンティティを持つ。BT タスクなどコールバックの外では null → エンジンが宣言なしとして失敗にする (U11)。バッファの owner も同じエンティティ
- 同: `ComputeRegistration.Register(allTypes, scriptTypes)` — コンパイル後のアセンブリから `MyeKernels.<Name>.CpuKernel` を集め (KernelName/SourceHash 定数、Body の引数 = HLSL の宣言順と変数名・RW/RO・stride、params の大きさ、`Range` の `GetFunctionPointer()`)、`[ComputeKernels]` を読み、`ComputeRegisterManagedKernels` / `ComputeSetManagedDeclarations` でレーンの表を丸ごと差し替える
- src/Scripting/ScriptRuntime.cs:Compile — `_types` を並べた後、旧 ALC を捨てる前、インスタンス化の前に Register (spec D28-b)。Invoke*/Trigger/Collision/Break を `ComputeCaller.Scope` で包む。コンパイル失敗時は Register に到達しないので前の登録が残る
- src/Scripting/Interop.cs:Engine — v29 の 10 スロットの薄い窓口 (`ComputeCreateBuffer` ほか)。ABI は変えない (174 のまま)
- src/Scripting/KernelCompat.cs: 公開 `KernelBuffer` (MyeKernelBuffer と同レイアウト)
- src/Engine/Engine/Compute/KernelCSharp.cpp (EmitRange) と KernelLintSelfTest.cpp の golden: 生成する `Range` の署名を ABI の `MyeKernelRangeFn` に合わせた (`Range(void* paramsPtr, KernelBuffer* buffers, uint bufferCount, uint begin, uint end)`)
- src/Engine/Engine/Script/ManagedHost.cpp: `CompileScripts` が `kernel::WriteCSharpKernels(<scriptsDir の親>)` を C# のコンパイルの前に呼ぶ (呼び出し点 4 か所を個別に直さず 1 か所に集約)。`Shutdown` は C# レーンの CPU 版と宣言を表から外す (ランタイムを閉じる前)
- src/Engine/Engine/Loop/HeadlessSim.{h,cpp}: `Managed()` (selftest が Init して使う)
- assets/shaders/kernels/SelfTestAllFuncs.kernel.hlsl (+ .meta、guid 手付け): ホワイトリストの全関数・int2・FloatToInt/FloatToUint・while/break/continue を通る 1 本 (KernelLintSelfTest の LintAll 正例を実ファイル化)。tools\gen_project_files.ps1 で GameLogic の Kernels.gen.{h,cpp} が 4 本に増えた
- src/Engine/Engine/Compute/ComputeManagedSelfTest.{h,cpp} (新規) と EditorMain への配線: 実際の CoreCLR + Roslyn で C# スクリプトを走らせる常設 selftest (MyeScripting.dll や .NET が無ければ SKIP)

仕様との差分:
- [追加] 生成 C# の `Range` の署名変更 (sub-02 の出力形式)。sub-02 は (begin, end, void* paramsPtr, void** buffers, uint* counts) だったが、sub-05 が確定した ABI (`MyeKernelRangeFn` = params, bufs, bufCount, begin, end) と合わず、そのままでは登録できないため。golden も更新した
- [追加] SelfTestAllFuncs カーネル。D24 の「ホワイトリストのすべての関数を通る 1 本」を実走で比べるのに実ファイルが要る。副作用として GameLogic のカーネル表が 3 → 4 本
- [追加] `HeadlessSim::Managed()`、`ManagedHost::Shutdown` での C# 登録の取り外し、`CompileScripts` 内での Kernels.gen.cs 生成
- [未実装] `.kernel.hlsl` の編集をエディタが検知して C# の再コンパイルを自動で要求すること (sub-06 の 1)。FileWatcher 側の受け口 (`UpdateKernelSource` を呼ぶ側) が sub-07 の担当で、まだ無い。いまは [Compile C# Scripts] / [Rebuild Scripts] / `CompileScripts` を呼ぶと再生成と再コンパイルが走る。それまでの「不一致として扱う」は、`UpdateKernelSource` で版ずれにすると C# の State の ForEach が失敗し、再コンパイルで一致に戻ることを selftest で確認した
- [追加] ホワイトリストから外す関数は無かった (R6 の退避は不要)。ワーカーからの managed 呼び出しも正しさは崩れなかった (R7 の直列退避は不要)

検証:
- `dotnet build` 相当 `tools\build_managed.bat Debug` / `Release` (環境変数 `MYE_DOTNET_ARGS=/p:TreatWarningsAsErrors=true`) → 警告 0・エラー 0
- `MyEngine.sln` Debug|x64 / Release|x64 ビルド → エラーなし (警告は既存の ServerNetSelfTest C4834 のみ)
- `bin\x64\Debug\Editor.exe --selftest` → exit 0、FAIL 0。`bin\x64\Release\Editor.exe --selftest` → exit 0、FAIL 0。Compute C# の全項目が PASS: Kernels.gen.cs の生成と登録 (GameLogic の C++ レーンを空にした状態で成功 = GameLogic.dll にカーネルが無くても C# の State が成功する)、Start/Update/再読込直後の宣言判定、大文字小文字違い、BT タスク、配列の糖衣、C++ の CPU 版とのバイト一致 (Saxpy 100k・Mix 20k・AllFuncs 20k)、10 回繰り返しのハッシュ一致、版ずれ → 再コンパイルで復旧、RW の int 添字のコンパイルエラー (CS1503) と KernelId の対照
- 検出力の確認 (変異テスト): KernelCompat.cs の `dot(float3)` の加算順を `a.x*b.x + (a.y*b.y + a.z*b.z)` に変えて build_managed → selftest で Mix (要素 4)・AllFuncs (要素 2) が不一致で FAIL、再読込後も FAIL。戻して PASS
- JIT の組み合わせ (Release の Editor.exe --selftest を環境変数を変えて 6 回、Compute C# は全て PASS・exit 0): 既定 / `DOTNET_TieredCompilation=0` / `DOTNET_TieredPGO=0` / `DOTNET_TC_QuickJitForLoops=0` + `DOTNET_OSR_HitLimit=1` / `DOTNET_EnableAVX2=0` + `DOTNET_EnableAVX=0` / `DOTNET_ReadyToRun=0` + `DOTNET_TieredCompilation=0`。この 6 回は「版ずれ → 再コンパイル」の項目を足す前の実行で、足した後は Debug / Release の既定で再実行して PASS。既定の tiered JIT では、時間を空けて (90 ms × 10 回) 同じ ForEach を重ねて tier-0 → tier-1 の切り替わりをまたいだ
- 速度 (1M 要素、jobs 10 本、ログのみ・Release): Saxpy C++ 0.21〜0.34 ms / C# 0.26〜0.60 ms、Mix C++ 12.8〜22.4 ms / C# 15.1〜28.6 ms (環境変数の組み合わせでばらつく)。Debug の C++ は Saxpy 11.7 ms / Mix 124 ms で、C# は常に Release でコンパイルされるので Saxpy 1.9 ms / Mix 56 ms
- `bin\x64\Debug\Server.exe --selftest` → ALL PASS
- `tools\check_rules.ps1` → 0 error (50 warning は既存)。規則 11 (Interop.cs の位置ミラー) は ABI を変えていないので変更なしで通る
- `tools\replay_verify.bat` → PASS (19 jobs、13 シーン + snapshot round-trip + jobs A/B + time travel + rule check)
- 未実行: 実機 GPU 上の C# Fx、エディタ GUI からの [Compile C# Scripts] の手動確認 (ManagedHost::CompileScripts の経路は selftest で通した)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜5 を実走で確認。.kernel.hlsl の編集を検知して自動で再コンパイルを要求する部分だけ sub-07 の受け口待ち
  正しさ: 4 — C++ とのバイト一致を 6 通りの JIT 設定 + 変異テストで確認。実機 GPU・GUI の手動確認は未実施
  コード品質: 4 — 登録は Compile の 1 点、呼び出し元の識別は Scope に集約。Compute.cs の ForEach のオーバーロードは 1〜4 本の手書き (可変長ジェネリックが無いため)
  テスト: 4 — 常設 selftest (CoreCLR 実走)。JIT 設定の切り替えは環境変数を変えた手動の再実行で、常設では既定の 1 回分だけ

不安・質問:
- sub-07 へ: `.kernel.hlsl` の FileWatcher が `UpdateKernelSource` を呼ぶ箇所で、C# のスクリプトが読み込まれていれば `ManagedHost::CompileScripts` も呼ぶ (または要求を立てる) 形にしてほしい。生成は CompileScripts の中で済むので、呼ぶだけでよい
- tiered JIT の切り替えを `DOTNET_*` 環境変数で変えた再実行は常設化していない (CI で同じ時間が 6 倍になるため)。常設の selftest は既定 1 回 + 時間を空けた 10 回の繰り返し。環境変数付きの再実行を CI に足すかは planner 判断

触ったファイル:
- src/Scripting/Compute.cs (新規), src/Scripting/ScriptRuntime.cs, src/Scripting/Interop.cs, src/Scripting/KernelCompat.cs
- src/Engine/Engine/Compute/KernelCSharp.cpp, src/Engine/Engine/Compute/KernelLintSelfTest.cpp, src/Engine/Engine/Compute/ComputeManagedSelfTest.{h,cpp} (新規)
- src/Engine/Engine/Script/ManagedHost.cpp, src/Engine/Engine/Loop/HeadlessSim.{h,cpp}, src/Editor/App/EditorMain.cpp
- assets/shaders/kernels/SelfTestAllFuncs.kernel.hlsl, assets/shaders/kernels/SelfTestAllFuncs.kernel.hlsl.meta (新規)
- src/GameLogic/Generated/Kernels.gen.h, Kernels.gen.cpp (生成物だが版管理)
- build/Engine.vcxproj, build/Engine.vcxproj.filters (gen_project_files.ps1 の出力。追跡対象)
申し送り:
- sub-07: 上の「不安・質問」の FileWatcher の件。インスペクターの C# の状態は `ComputeService::IsDeclared(CallerKind::CSharp, ...)` と `FindKernel(...)->csharp` (range/sourceHash) で出せる。C# レーンの宣言表のスクリプト型名は C# の `Type.FullName` (名前空間なしのクラスならクラス名)
- sub-08: C# のデモは `[ComputeKernels]` を付けたスクリプトを assets\scripts に置く。生成は CompileScripts が `assets\shaders\kernels` から自動で行う (assets\scripts\Generated\ は .gitignore 済み)
- sub-09: ADR-030 に書く事実 — (1) 生成 C# の Range の署名は ABI の MyeKernelRangeFn と同じ。(2) C# の CPU 版は `MethodHandle.GetFunctionPointer()` で collectible ALC 内の `[UnmanagedCallersOnly]` から取れ、JobSystem のワーカーから呼べる (Debug / Release とも確認)。(3) D24: ホワイトリストの全関数が 6 通りの JIT 設定で C++ とバイト一致。(4) 速度は上の表。(5) RW を int で添字するとコンパイルエラー CS1503。(6) 変異テストの手順 (`dot(float3)` の加算順を変える)
- 環境: Bash ツールの heredoc はバックスラッシュを 1 段潰す (Python の `"\\n"` が改行になった)。C++ の文字列リテラルを Python で差し込むときは Edit ツールで直すこと

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。
  - 受け入れ 1〜5 を実走で確かめた。D24 は、6 通りの JIT 設定と変異テストで、C++ の CPU 版とのバイト一致を確かめた。R6 (一致しない関数が出る) と R7 (ワーカーから managed を呼ぶ問題) の退避は、どちらも要らなかった。
  - `.kernel.hlsl` の編集を検知したときに C# の再コンパイルを要求する部分は、sub-07 へ移した。
  - JIT の設定を切り替えて回す試験は CI に常設しない。手順を ADR-030 に書く。
