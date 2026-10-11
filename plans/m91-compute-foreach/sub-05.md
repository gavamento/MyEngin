# sub-05: GameLogic のカーネル表・COMPUTE_KERNELS・ABI・C++ 糖衣・版ずれ制限

- 依存: sub-02, sub-04
- 状態: 未着手
- 往復: 0
- 更新: 2026-10-12 (U10「宣言なしは失敗」と、宣言を取り込む時点)
- 再編: 2026-10-12 (旧 sub-04。U6 を反映して `COMPUTE_KERNELS` 宣言を追加し、U1 を反映して C# のレンジ関数を登録するスロットを ABI に含める)

## やること
spec 4.1.3 / 4.1.6 / 4.2 (ABI)、D17 / D18 / D22。**ABI の bump は M91 の中でこの 1 回だけ** (C# 用のスロットもここで確保し、sub-06 は ABI を変えない)。

0. (sub-01 からの申し送り) 生成する .cpp は `#include "Shared/KernelCompat.h"` → `namespace mye::kernel::cpu::<Name> { #include "...<Name>.kernel.hlsl" }` の形にする。実行器は `RunKernelCpu(js, &Name_Cpu, params, RunOptions, RWView, ROView...)`。PowerShell 版のハッシュの手順と参照値 ("a" = af63dc4c8601ec8c) は sub-01.md の実装メモにある。
1. codegen: `tools\gen_project_files.ps1` が `kernels/*.kernel.hlsl` (プロジェクト側とエンジン側) を走査し、`src/GameLogic/Generated/Kernels.gen.{h,cpp}` を生成する
   (カーネル表 = 名前・C++ のレンジ関数・正規化したハッシュ・バッファ引数ごとの stride と RW/RO・params の大きさ)。
   PowerShell 版のハッシュは、sub-01 の C++ 版とテストベクタで照合する。struct は Kernels.gen.h からスクリプトで使える。
2. 宣言: `COMPUTE_KERNELS(ScriptType, "A", "B")` のような形で、`MyeScriptDesc` の末尾に名前表 (`kernelNameCount` / `kernelNames`) を足す。形は `REGISTER_SCRIPT` / `FIELDS` の流儀に合わせて coder が決め、実装メモに書く。
3. ABI: 着手時点の次の版 (今なら v29)。末尾に追加するだけ。ForEach 用のスロット群に、呼び出し元のスクリプト型 (ctx から取れるなら引数は不要) と、C# のレンジ関数ポインタを登録するスロット (sub-06 が使う) を含める。
   `MyeScriptModule` の末尾にカーネル表を足す。`Interop.cs` の位置ミラーも同時に直す。確定した本数とシグネチャを実装メモに書く。
4. エンジン側: DLL の読込 / ホットリロードのときにカーネル表と宣言を取り込む。**宣言表は `MyeScriptModule` を取り込むのと同じ箇所・同じ時点で丸ごと差し替え、モジュールのコードと同じ世代にする** (spec D28-a)。これは Start より前に揃っていること。そのうえでハッシュを照合し、sub-02 の検査結果と合わせて判断 (sub-03) へ渡す。
5. C++ 糖衣 (`ScriptAPI.h`): `Compute::CreateBuffer<T>` / `Buffer<T>` / `Compute::ForEach` / 配列を直接渡す糖衣 / `Job`。Fx の Read 結果をハッシュ対象に書くな、という注記 (D11)。
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

## フィードバック履歴
