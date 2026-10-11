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

## フィードバック履歴
