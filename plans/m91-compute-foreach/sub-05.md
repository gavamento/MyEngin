# sub-05: GameLogic のカーネル表・COMPUTE_KERNELS・ABI・C++ 糖衣・版ずれ制限

- 依存: sub-02, sub-04
- 状態: OK (commit 92bde8a)
- 往復: 0
- 更新: 2026-10-12 (U10「宣言なしは失敗」と、宣言を取り込む時点)
- 再編: 2026-10-12 (旧 sub-04。U6 を反映して `COMPUTE_KERNELS` 宣言を追加し、U1 を反映して C# のレンジ関数を登録するスロットを ABI に含める)

## やること
spec 4.1.3 / 4.1.6 / 4.2 (ABI)、D17 / D18 / D22。**ABI の bump は M91 の中でこの 1 回だけ** (C# 用のスロットもここで確保し、sub-06 は ABI を変えない)。

0a. (sub-03 からの申し送り) カーネル表の引数ごとに **HLSL の変数名** (RW / RO の宣言名) を出力する (`KernelArgDesc::name`、空だと登録を拒否する)。GPU のバインドはリフレクションの名前で行う。FXC は未使用の資源を落として詰めるので、位置で引いてはいけない。
0. (sub-01 からの申し送り) 生成する .cpp は `#include "Shared/KernelCompat.h"` → `namespace mye::kernel::cpu::<Name> { #include "...<Name>.kernel.hlsl" }` の形にする。実行器は `RunKernelCpu(js, &Name_Cpu, params, RunOptions, RWView, ROView...)`。PowerShell 版のハッシュの手順と参照値 ("a" = af63dc4c8601ec8c) は sub-01.md の実装メモにある。
1. codegen: `tools\gen_project_files.ps1` が `kernels/*.kernel.hlsl` (プロジェクト側とエンジン側) を走査し、`src/GameLogic/Generated/Kernels.gen.{h,cpp}` を生成する
   (カーネル表 = 名前・C++ のレンジ関数・正規化したハッシュ・バッファ引数ごとの stride と RW/RO・params の大きさ)。
   PowerShell 版のハッシュは、sub-01 の C++ 版とテストベクタで照合する。struct は Kernels.gen.h からスクリプトで使える。
2. 宣言: `COMPUTE_KERNELS(ScriptType, "A", "B")` のような形で、`MyeScriptDesc` の末尾に名前表 (`kernelNameCount` / `kernelNames`) を足す。形は `REGISTER_SCRIPT` / `FIELDS` の流儀に合わせて coder が決め、実装メモに書く。
3. ABI: 着手時点の次の版 (今なら v29)。末尾に追加するだけ。ForEach 用のスロット群に、呼び出し元のスクリプト型 (ctx から取れるなら引数は不要) と、C# のレンジ関数ポインタを登録するスロット (sub-06 が使う) を含める。
   `MyeScriptModule` の末尾にカーネル表を足す。`Interop.cs` の位置ミラーも同時に直す。確定した本数とシグネチャを実装メモに書く。
4. エンジン側: DLL の読込 / ホットリロードのときにカーネル表と宣言を取り込む。**宣言表は `MyeScriptModule` を取り込むのと同じ箇所・同じ時点で丸ごと差し替え、モジュールのコードと同じ世代にする** (spec D28-a)。これは Start より前に揃っていること。そのうえでハッシュを照合し、sub-02 の検査結果と合わせて判断 (sub-03) へ渡す。
5. C++ 糖衣 (`ScriptAPI.h`): `Compute::CreateBuffer<T>` / `Buffer<T>` / `Compute::ForEach` / 配列を直接渡す糖衣 / `Job`。Fx の Read 結果をハッシュ対象に書くな、という注記 (D11)。Fx のジョブハンドルを FIELD に保存するな、という注記 (spec 4.1.4)。
   (sub-03 からの申し送り) カーネルの登録は `ComputeService::RegisterKernel(ComputeKernelDesc)`、宣言は `ReplaceDeclarations`、ソース情報は `UpdateKernelSource`、C++ 版は `kernel::MakeRangeFn<&ns::Name_Cpu>()` で作る。
6. 同梱の GameLogic に最小のスクリプト (宣言つき) を置き、selftest または headless で実際に走らせる。宣言していないカーネルを呼ぶと失敗すること、Start の中からの呼び出しとホットリロード直後の最初の tick の呼び出しも同じ判定になることを確かめる (spec 5-16 の C++ 部分)。
7. ctx から呼び出し元のスクリプト型が取れない呼び出し (BT タスクなど) は失敗にする (spec D28-e。U11 の裁定は仮置き)。

## やらないこと (このサブでは)
- C# の公開 API と C# 版の実行 (sub-06)、インスペクター (sub-07)、デモ (sub-08)

## 触る場所 (planner の見立て)
- `tools\gen_project_files.ps1:117-123`、`src/Shared/ScriptTypes.h` (`MyeScriptDesc`:49、`MyeScriptModule`:111)、`src/Shared/EngineAPI.h`、`src/Shared/ScriptAPI.h`
- `src/Engine/Engine/Script/EngineApiTable.cpp`、ScriptHost / DllReloader、`src/Scripting/Interop.cs` (ミラーだけ)、`src/GameLogic/Scripts/`

## 受け入れ条件 (このサブ)
1. spec 5-7
4. spec 5-16 の C++ 部分 (宣言なしの失敗と、判定の時点の一貫性) を selftest で確かめる
2. spec 5-9 (Interop.cs の順序を機械で照合する。memory `abi-bump-verification.md`)
3. spec 5-11 のハッシュ照合の部分 (PowerShell 版と C++ 版が同じ値になる。1 文字変えると不一致になり、C++ の State の ForEach が失敗する)

## 検証コマンド
- `tools\gen_project_files.ps1`、Debug / Release ビルド (GameLogic を含む)、`Editor.exe --selftest`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL)
実装:
- Shared/EngineAPI.h: ABI v29 = 174 スロット (164 + 10)。末尾に `ComputeCreateBuffer(engine, owner, count, stride, kind)` / `ComputeReleaseBuffer` / `ComputeWriteBuffer` / `ComputeReadBuffer(.., wait)` / `ComputeForEach(engine, kernel, execution, params, paramsSize, buffers, bufferCount, callerScript)` / `ComputeJobStatus` / `ComputeJobTarget` / `ComputeJobWait` / `ComputeRegisterManagedKernels(engine, kernels, count)` / `ComputeSetManagedDeclarations(engine, decls, count)`。POD は `MyeKernelBuffer` / `MyeKernelRangeFn` / `MyeKernelArg` / `MyeKernelDesc` / `MyeComputeDecl` (スロットの前に定義)。Fx のハンドル・Status・Target・Read の結果を FIELD や sim の分岐に使うなという注記 (spec 4.1.4) を EngineAPI.h と ScriptCompute.h に書いた
- Shared/ScriptTypes.h: `MyeScriptDesc` 末尾に `kernelNameCount` / `kernelNames`、`MyeScriptModule` 末尾に `kernelCount` / `kernels`
- Shared/ScriptAPI.h: `COMPUTE_KERNELS(ScriptType, "A", "B")` (REGISTER_SCRIPT の前後どちらでも効く。保留表 `KernelDeclarations()` を介して結び付ける)、カーネル表の登録簿 `KernelRegistry()` / `KernelRegistrar`。末尾で ScriptCompute.h を include
- Shared/ScriptCompute.h (新規): `Compute::CreateBuffer<T>` / `Compute::Buffer<T>` (Write/Read/Release/FromHandle) / `Compute::ForEach` (バッファ版、ポインタ+要素数版、std::vector 版、C 配列版) / `Compute::Job` (Ok/Status/Target/Wait)、`BufferKind` / `Execution`
- Shared/KernelRangeFn.h (新規): `MakeRangeFn` と `KernelLauncher` を GameLogic.dll でも使えるよう Shared へ移し (KernelCompat.h に KernelLauncher、Engine/Compute/KernelRange.h は実行器だけ残す)、`MakeKernelArgs<&Name_Cpu>(names)` / `MakeKernelDesc<&Name_Cpu>(name, hash, args)` で引数の数・RW/RO・stride・params の大きさを型から作る
- tools/gen_project_files.ps1: `assets\shaders\kernels\*.kernel.hlsl` を走査して `src/GameLogic/Generated/Kernels.gen.{h,cpp}` を生成 (ハッシュは C# の unchecked 算術 `MyeKernelHash` で FNV-1a、参照値 "a" = af63dc4c8601ec8c を実行のたびに検査。HLSL の変数名は `KERNEL(...)` の宣言から取り出す。変更が無ければ書き換えない)。.h はカーネルを `mye::kernel::cpu::<Name>::{無名名前空間}` へ取り込み (カーネルの自由関数が inline でなく、複数 TU が include するとリンクが衝突するため)、`MyeKernels::<Name>` で struct を使える
- GameLogic: `src/GameLogic/GameLogicMain.cpp` と `AssetOps.cpp` の生成 main に `mod.kernels` / `kernelCount`。`Scripts/ComputeProbe.cpp` (宣言つきの恒久 probe)
- ComputeService: `ReplaceKernels(lane, descs)` (言語ごとに CPU 版の表を丸ごと差し替え。もう一方の言語は残す。両方空になったカーネルは消える。引数表が食い違うと他方の古い CPU 版を捨てる)、宣言表を言語ごとに持つ `ReplaceDeclarations(lane, ..)` / `IsDeclared(lane, ..)` (ForEach は呼び出し元の言語の宣言だけを見る)、`ScanKernelSources(dir)`、`HasKernel` / `HasKernelSource` / `FindKernel` / `FindKernelSource`、`KernelDescFromAbi`。sub-03 nit: ForEach から `MakeDecisionInput` を分離
- EngineApiTable.{h,cpp}: `ScriptApiContext` に `compute` / `computeLane` / `runningScript`。10 スロットの実体は `FillComputeSlots`。C++ レーンは `runningScript` だけを見て引数の callerScript を信用しない (偽造防止)。C# レーンは引数の callerScript を使う。C++ レーンから C# の登録スロットを呼ぶと何もせず 0
- ScriptHost: `SetComputeService(service, kernelDir)`、`LoadModule` がカーネル表と宣言表を **モジュールと同じ箇所・同じ時点 (旧 DLL の解放の前)** で丸ごと差し替え (`PublishCompute`)、同時に kernelDir のソースを再走査。Shutdown でも外す。各コールバック (Start / Update / LateUpdate / Trigger / Collision / Break) の前後で `RunningScriptScope` が `runningScript` を入れ外し。宣言したのにカーネルのファイルが無いと WARN (spec D28-f)。BT タスクはコールバックの外なので `runningScript` が null → 失敗 (U11)
- ManagedHost.h: `SetComputeService` (lane = CSharp)。SimInit.{h,cpp} `SimSharedServices::compute` / `kernelDir`、EngineLoop / HeadlessSim が配線 (kernelDir = `<assetsRoot>\shaders\kernels`)。DllReloader `ReloadNow()` と HeadlessSim `ReloadGameLogic()` (selftest がホットリロード直後を作る)
- Scripting/Interop.cs: 位置ミラー 10 本 + `MyeKernelBuffer` / `MyeKernelArg` / `MyeKernelDesc` / `MyeComputeDecl` (sub-06 が使う)。tools/check_rules.ps1 の版表に `29 = 174`。PartSelfTest の版固定を 29 へ。docs/history/api-scripting-tools.md に v29 の 1 行
- Engine/Compute/ComputeScriptSelfTest.{h,cpp} (新規) と EditorMain への配線。ComputeServiceSelfTest の宣言表は言語ごとに両方へ入れる形へ

確定した COMPUTE_KERNELS の形: `COMPUTE_KERNELS(Swarm, "ParticleUpdate", "Collide");` (REGISTER_SCRIPT の前後どちらでもよい)。
確定した ABI: 10 本・174 スロット。シグネチャは EngineAPI.h と上の実装欄のとおり。

仕様との差分:
- [追加] `ComputeForEach` に `callerScript` 引数を足した。C# は managed 側が呼び出し元のスクリプト型を知っているので、sub-06 が ABI を変えずに済むようにするため。C++ レーンでは無視する (エンジンが `runningScript` で決める)
- [追加] `ComputeRegisterManagedKernels` / `ComputeSetManagedDeclarations` の 2 本 (C# の CPU 版と `[ComputeKernels]` の宣言を登録する口。sub-05.md の 3 の「C# のレンジ関数ポインタを登録するスロット」を、宣言を運ぶ分も含めて 2 本にした)。宣言表を言語ごとに持つ必要が出たので ComputeService の宣言表を言語ごとにした
- [追加] 生成物 `Kernels.gen.{h,cpp}` は .gitignore に入れず版管理する (入れないと、gen_project_files.ps1 を回していない新規クローンでは GameLogic.vcxproj が存在しない .cpp を参照して落ちる。ハッシュ込みなので、編集後に再生成しないと版ずれとして検出される)
- [追加] カーネルを無名名前空間に入れて取り込む (カーネルの自由関数が inline でなく、Kernels.gen.h を複数の TU が include するとリンクが衝突したため)
- [追加] `ScriptHost::LoadModule` の isNew 経路: 登録表 (プロセス共有) に同名の型が既にある場合は `UpdateDesc` で construct を今の DLL へ差し替える。以前は Register が既存 id を返すだけで、別の HeadlessSim が先に登録して解放した DLL の construct が残り、selftest が 1 プロセスに順に立てる 2 つ目以降の sim で ComputeProbe を AddComponentRaw すると Debug でアクセス違反になった (診断: 例外アドレスがどのモジュールにも属さない = 解放済み DLL)
- [追加] `KernelRunner.h` の `KernelLauncher` 定義を `Shared/KernelCompat.h` へ移動。`KernelRange.h` の `KernelBufferRef` / `KernelRangeFn` は Shared の POD の別名にした
- [追加] 宣言したのにカーネルのファイルが無いときの WARN (D28-f) を LoadModule 時に出す。インスペクターの「宣言あり・カーネル無し」表示は sub-07
- [未実装] `.kernel.hlsl` の編集を FileWatcher で拾って検査・ハッシュを評価し直すこと (spec 4.1.6)。評価し直すのは DLL の読込時のみ。sub-07 のアセット扱いの担当と解釈した
- [未実装] C# 側の実走 (新しい 10 スロットを C# から呼ぶこと) は sub-06。このサブでは Interop.cs の位置ミラーを check_rules 規則 11 (順序・件数・名前・引数個数) と C# のビルドで確認しただけ
- [未実装] kernelDir は `<assetsRoot>\shaders\kernels` の 1 か所だけ走査する。「プロジェクト側とエンジン側」の 2 か所に分かれる構成 (外部プロジェクトがエンジンのカーネルを使う) は、このリポジトリでは同じ場所なので扱っていない

検証:
- `pwsh tools\gen_project_files.ps1` → 成功 (2 回目は生成物に変更なし = 冪等)。"a" の参照値検査つき
- Debug / Release|x64 の `MyEngine.sln` ビルド → エラーなし (警告は既存の SelfTestUnused C4100 / TagSelfTest C4456 のみ)。`tools\build_managed.bat Debug` → 成功 (Interop.cs のミラー)。ただし `/p:TreatWarningsAsErrors=true` (CI の指定) だと sub-02 の `src/Scripting/KernelCompat.cs(413)` の CS1718 で失敗する (このサブの変更ではない。申し送り)
- `bin\x64\Debug\Editor.exe --selftest` → exit 0、FAIL 0。Compute service / state / script ABI とも PASS。`bin\x64\Release\Editor.exe --selftest` → exit 0、FAIL 0
- `bin\x64\Debug\Server.exe --selftest` → ALL PASS
- `tools\check_rules.ps1` → 0 error (50 warning は既存)。規則 11 が 29 = 174 を照合して通る
- `tools\replay_verify.bat` → PASS (19 jobs、13 シーン + snapshot round-trip + jobs A/B + time travel + rule check)
- ComputeScriptSelfTest の内訳 (全 PASS): ABI スロットの往復 (作成・書き込み・読み出し・ForEach・Status/Target/Wait、owner の受け渡し、要素数超過・無効ハンドルの拒否) / 宣言なし・スクリプトの外 (runningScript = null、BT タスク相当)・大文字小文字違い・callerScript の偽造はいずれも実行されずに 0 でバッファ不変 / C# レーンの登録スロットと callerScript、ストアが言語ごとに別 / ReplaceKernels と宣言表の丸ごと差し替え / GameLogic.dll の ComputeProbe を HeadlessSim で Start・Update・ソース 1 行追加 (ハッシュ不一致で State の ForEach 失敗・結果不変)・戻して一致・`ReloadGameLogic()` 直後の新しい Start と既存の Update が同じ判定 / PowerShell が埋めたハッシュ 3 本がエンジンの C++ 版と一致 / 持ち主エンティティの破棄で ABI 経由の 6 本が解放
- PowerShell 版ハッシュの手動確認: "a" = af63dc4c8601ec8c、空 = cbf29ce484222325、CRLF と LF、BOM あり/なしが同値、単独 CR は保たれる
- 未実行: 実機 GPU (このサブは CPU 経路のみ)、C# から新スロットを呼ぶ実走 (sub-06)、Debug の selftest の再実行回数は 1 回 (修正前は 3 回連続で落ちていたのが修正後 1 回通った)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜4 を満たした。FileWatcher での再評価と C# の実走は後続サブ
  正しさ: 4 — selftest Debug/Release/Server・replay_verify・check_rules。実機 GPU と C# 実走は未検証
  コード品質: 4 — 無名名前空間、保留表、construct 差し替えなどに理由コメントあり。ScriptHost.cpp が長くなった
  テスト: 4 — ABI 往復から GameLogic の実走・ホットリロード・版ずれ・所有者解放まで網羅。C# 側の実走は無し

不安・質問:
- ComputeForEach に足した callerScript と、C# の登録 2 本で sub-06 に足りるか (sub-06 が追加で ABI を要求すると M91 中の ABI bump が 2 回になる)。特に C# が「実行中のスクリプト型名」をどう知るか (managed 側で呼び出しの前に自分で持つ想定) を sub-06 で確認してほしい
- 生成物 Kernels.gen.{h,cpp} を版管理にした判断 (spec 4.2「Schema.gen と同じ扱い」は .gitignore 側。C++ の生成物は vcxproj に載るので逆にした)
- 申し送りの 1 件: ScriptHost の construct 差し替えは、同時に生きている複数の ScriptHost (ServerNetSelfTest の 4 sim) では「最後に読んだ DLL」を指す。先に解放する側が最後だと残る側が dangling になる既存の弱点は残っている (今回は順に立てる場合の修正)

触ったファイル:
- src/Shared/EngineAPI.h, ScriptTypes.h, ScriptAPI.h, KernelCompat.h, KernelRangeFn.h (新規), ScriptCompute.h (新規)
- src/Scripting/Interop.cs, tools/check_rules.ps1, tools/gen_project_files.ps1
- src/GameLogic/GameLogicMain.cpp, src/GameLogic/Scripts/ComputeProbe.cpp (新規)
- src/GameLogic/Generated/Kernels.gen.h, Kernels.gen.cpp (新規。生成物だが版管理にした)
- src/Editor/Asset/AssetOps.cpp, src/Editor/App/EditorMain.cpp, src/Editor/SelfTest/PartSelfTest.cpp
- src/Engine/Engine/Compute/ComputeService.{h,cpp}, ComputeServiceSelfTest.cpp, ComputeScriptSelfTest.{h,cpp} (新規), KernelRange.h, KernelRunner.h
- src/Engine/Engine/Script/EngineApiTable.{h,cpp}, ScriptHost.{h,cpp}, ManagedHost.h
- src/Engine/Engine/Loop/SimInit.{h,cpp}, EngineLoop.cpp, HeadlessSim.{h,cpp}
- src/Engine/Engine/HotReload/DllReloader.h
- docs/history/api-scripting-tools.md
- (gen_project_files.ps1 の出力: build/Engine.vcxproj(.filters), build/GameLogic.vcxproj(.filters)。追跡対象なので一緒にステージが要る)
申し送り:
- sub-06: C# が登録する CPU 版は `MyeKernelDesc` (Interop.cs に struct 済み。`Range` は `[UnmanagedCallersOnly]` の関数ポインタ) の配列を `ComputeRegisterManagedKernels` へ渡す (C# レーンの表を丸ごと差し替える。0 件で空にする)。`[ComputeKernels]` の宣言は `ComputeSetManagedDeclarations` (これも丸ごと)。ForEach の `callerScript` は UTF-8 の型名 (null = スクリプトの外 = 失敗)。登録の順序は「Roslyn のコンパイル後・スクリプトをインスタンス化する前」(spec D28-b)
- sub-07: `.kernel.hlsl` の変更を拾うなら `ComputeService::UpdateKernelSource(fileName, text)` を呼ぶ (ハッシュと検査を更新する)。全部読み直すなら `ScanKernelSources(dir)`。インスペクターの「宣言あり・カーネル無し」は `IsDeclared` と `HasKernel` / `HasKernelSource` で出せる。`FindObservation` / `Observations()` は sub-03 から変わらず
- sub-09: engine_spec.md と docs の ABI の版・スロット数の記述 (v28 = 164) は未更新 (docs/history だけ更新した)。memory の M75h は v29 → v30
- sub-02 の C# `src/Scripting/KernelCompat.cs(413)` に CS1718 (`x != x` の NaN 判定) があり、CI の `TreatWarningsAsErrors=true` で `tools\build_managed.bat` が落ちる。CI を確認してほしい
- HeadlessSim は、GameLogic.dll の型の construct を差し替えた後は、その sim が生きている間だけ安全 (上記)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。must 1 件: `src/Scripting/KernelCompat.cs:413` の `x != x` が CS1718 になり、CI (`.github/workflows/ci.yml:98` の `/p:TreatWarningsAsErrors=true`) の build_managed が落ちる。sub-02 で既にコミット済み (f71dc9f) なので、このサブで直す。追加の確認: 修正後に Debug の selftest をもう 2 回回し、安定していることを示す。差分の [追加] と [未実装] はすべて採用し、spec の 3 / 4.2 / 7 に反映した。
- round 2: VERDICT OK (planner、2026-10-12)。must 2 件とも解消した (CS1718 を float.IsNaN に置き換えた。Debug の selftest は 3 回連続 PASS、Server も PASS)。
