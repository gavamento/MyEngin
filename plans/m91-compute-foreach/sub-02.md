# sub-02: サブセット検査器と C# への変換器

- 依存: sub-01
- 状態: 未着手
- 往復: 0
- 新設: 2026-10-12 (U1「C# 用の CPU 版も作る」の反映)

## やること
spec D5 / D20 / D23 / D24、4.1.2、受け入れ 3 (検査器) / 14。

1. 検査器 (Engine 内の C++、例 `src/Engine/Engine/Compute/KernelLint.*`): `.kernel.hlsl` のテキストを字句・構文解析し、spec 4.1.2 の規則違反を (ファイル名, 行, 規則 ID, 説明) の一覧で返す。
   サブセットの外は解析を広げずにエラーにする (spec R8)。結果はカーネルごとにキャッシュし、ファイル変更で更新。
   違反のあるカーネルはどの実行先でも失敗にする (判断への入力は sub-03)。
2. 変換器: 検査に通った AST から C# ソースを生成する。
   - struct → `[StructLayout(LayoutKind.Sequential, Pack = 4)] public struct`、`KERNEL_PARAMS` も同じ
   - カーネル本体 → `[UnmanagedCallersOnly]` のレンジ関数 `(begin, end, params*, bufs**, counts*)` から呼ぶ静的メソッド。`id` は C# の `KernelId`、RW は `KernelId` 添字のみ、RO は範囲外 0
   - コンストラクタ `float3(a,b,c)` → `new float3(a,b,c)`、`out`/`inout` → `out`/`ref`、リテラルはそのまま (`f` 付き)
   - ハッシュ (D17) を定数として埋める
   - 生成したコードは例外を投げない形 (spec D26)
3. C# 互換ライブラリ (`src/Scripting/` 側、`float2/3/4` `int*` `uint*` と関数ホワイトリスト): sub-01 の C++ 互換ヘッダと**同じ演算順**で書く (`System.Numerics` 不使用、D24)。`sin`/`cos` は `MyeSinRad` 系の式を移植。
4. 生成先: `assets/scripts/Generated/Kernels.gen.cs` (SchemaCodegen と同じ流儀・同じ書込み関数を使う)。生成を呼ぶ箇所 (C# コンパイル前) の接続は sub-06 で行う。このサブでは生成関数と selftest まで。
4b. 2026-10-12 の spec 4.1.1 / 4.1.2 の改訂 (sub-01 round 1) を検査規則に含める: RW/RO はコンマなしの書式、`out`/`inout` の禁止、属性の禁止、式の中の代入と `++` `--` の禁止、予約語、スカラー混在は 6 関数だけ、f の無い小数リテラルと縮小変換 (C++ は黙って通すので、検査器で必ず弾く)。C# 移植の正本の式 (dot の加算順、sin/cos の折り返し、HashU32 など) は sub-01.md の実装メモにある。
5. selftest: (a) 規則ごとの負例カーネル (テキスト埋め込みで可) が正しい規則 ID と行で弾かれる、(b) 正例が通る、(c) sub-01 の自己テストカーネルの変換結果が golden テキストと一致。

## やらないこと (このサブでは)
- C# での実走とバイト一致の検証 (sub-06)。ABI (sub-05)。

## 触る場所 (planner の見立て)
- 新規: `src/Engine/Engine/Compute/` (検査器・変換器)、`src/Scripting/` (互換ライブラリ。MyeScripting のプロジェクトに含める)
- 参考: `src/Engine/Engine/Schema/SchemaCodegen.{h,cpp}` (生成先 `assets\scripts\Generated\`、書込みの流儀)
- `tools\gen_project_files.ps1` 再実行

## 受け入れ条件 (このサブ)
1. spec 5-14: 検査器が 4.1.2 の各規則の違反をファイル・行・規則付きで弾き、変換結果が golden と一致 (selftest)
2. spec 5-3 (検査器側): `RW` を `KernelId` 以外で添字する書き方を検査器が弾く
3. C# 互換ライブラリが MyeScripting と一緒にビルドできる (Debug / Release)

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest` (両構成)、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
