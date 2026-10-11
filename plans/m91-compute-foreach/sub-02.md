# sub-02: サブセット検査器と C# への変換器

- 依存: sub-01
- 状態: OK (M91b)
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

### round 1 (coder)

#### 構成
- `src/Engine/Engine/Compute/`: `KernelAst.h` (構文木・型)、`KernelParser.cpp` (字句 + 構文。構文違反は最初の 1 件で打ち切り)、`KernelLint.{h,cpp}` (型検査・規則 KL001〜KL027・`LintKernelSource`・`KernelLintCache`)、`KernelCSharp.{h,cpp}` (変換器・`BuildKernelsCSharp`・`WriteCSharpKernels`)、`KernelLintSelfTest.{h,cpp}`。
- `src/Scripting/KernelCompat.cs` (`MyeScripting.KernelCompat`: float2/3/4・int2/3/4・uint2/3/4・`KernelId`・`RWView`/`ROView`・`KernelMath`)。MyeScripting.csproj の既定の glob で入る (csproj 変更なし)。
- 生成物の形: `namespace MyeKernels.<Name>` の中に struct (Pack = 4) と `public static unsafe class CpuKernel` (`KernelName` / `SourceHash` / 自由関数 / `Body` / `[UnmanagedCallersOnly] Range(begin, end, paramsPtr, buffers, counts)`)。生成物の先頭は `using System.Runtime.InteropServices; using MyeScripting.KernelCompat; using static ...KernelMath;`。

#### 検査器が C++ / HLSL / C# の食い違いを塞ぐために足した規則 (spec 4.1.2 への追記案)
- 1 引数のコンストラクタ (`float3(1.0f)`) は禁止: FXC が X3014 で拒否する (実測)。
- 整数リテラルは `int` に収まる (u を付ければ uint)。先頭 0 の整数 (8 進) 禁止。`1f`・`1.0h` 禁止。
- 条件式は bool のみ。`==` `<` などはスカラーどうしで同じ型 (int と float の混在だけ可)。bool 型の変数は無く、`true`/`false` は条件にだけ置ける。
- `uint` の単項マイナス禁止 (C# で long になる)。`~` はスカラー整数のみ。ベクトルのシフト禁止。シフト量は 0..31 の整数リテラル。
- RO の添字は `uint` (int 禁止)。RW の添字は識別子 `id` そのもの。
- ローカル変数の遮蔽禁止 (外側のスコープにある名前、まだ宣言されていない外側の名前を含む。C# の CS0136 に合わせる)。
- 自由関数は値を返す関数のみ (void 禁止)・全経路で return・id/params/バッファは見えない。再帰は「宣言前の呼び出し」として弾く。
- `min` `max` のスカラー混在は (ベクトル, スカラー) の順だけ (C++ 互換層に `min(S, V)` が無いため)。`dot` `distance` のスカラー混在は禁止 (KL017)。
- 入れ子 200 / 1 式 500 演算子を超えたら打ち切り (再帰降下のスタック保護)。
- 宣言のみで初期化しない変数 (`T e;`) は許す。C# は `= default` で出す (C++ / HLSL は未初期化なので、読む前に代入するのは書き手の責任)。

#### 検証 (実測)
- Debug / Release ビルドとも警告 0。`Editor.exe --selftest` Debug / Release とも exit 0 (`--no-jobs` 付き Release も exit 0)。Kernel lint 節 PASS 160 件・FAIL 0。
- 負例は KL001〜KL027 の全 27 規則に最低 1 本 (計 118 本)。規則 ID と行番号の両方が一致しないと FAIL する。「全規則に負例がある」こと自体も selftest で確認。
- golden: SelfTestSaxpy / SelfTestMix の変換結果。SourceHash は `@HASH@` に置換して比較 (カーネルを編集したら `MYE_KERNEL_GOLDEN_DUMP=<フォルダ>` を設定して selftest を回すと実際の出力を書き出す)。
- `MYE_KERNEL_LINT_FILES` (; 区切りのパス) を設定すると任意のカーネルを検査して違反をログに出す (開発用の検査 CLI 代わり)。
- tools\check_rules.ps1: 0 error / 50 warning (sub-01 と同数 = 新規なし)。
- MyeScripting.csproj の `dotnet build` Debug / Release: 警告 0・エラー 0。
- **範囲外の追加確認** (一時プローブ、コミットしない): 検査を通った全機能入りカーネル `LintAll` と `LintMore` を (a) MSVC `/std:c++20 /W4 /WX /fp:precise` (b) FXC cs_5_0 (c) 生成 C# で通した。3 つとも警告なしで通った。さらに C# を実走し、C++ の出力とバイト一致を確認: SelfTestSaxpy `1e2749c7308fa99c` / SelfTestMix `938f8aef9ad7c55c` (sub-01 の固定値と同値)・LintAll `87e05c2d1f0772d3`・LintMore `6b798738544e65d3`。`DOTNET_TieredCompilation=0` でも同じ。ホワイトリスト全関数を通ったのは LintAll。受け入れ 15 の正式な確認は sub-06。
  - 再現: `LintAll.kernel.hlsl` は `KernelLintSelfTest.cpp` の `kLintAllSource`。C++ 側は `KernelCompat.h` + `namespace ...cpu::LintAll { #include }` を 1 ファイルで包み、`KernelLauncher::Make(i)` で 1 要素ずつ呼ぶだけの小さな exe。

#### SELF_EVAL
SELF_EVAL: sub-02 (round 1)
実装: 上記「構成」のとおり。
仕様との差分:
  - [追加] 検査規則を spec 4.1.2 より多く足した (上の追記案一覧)。理由: C++ / FXC / C# が黙って通して結果が割れる書き方を塞ぐため。特に「1 引数コンストラクタ」は FXC が実際に拒否するので spec の表 (コンストラクタ `float3(a,b,c)`) に「引数は成分ちょうど。1 引数の splat 不可」を足してほしい。
  - [追加] 生成物を `namespace MyeKernels.<Name>` で分けた (spec は「struct → [StructLayout...] public struct」だけ)。理由: 別カーネルが同名の struct (例 `Particle`) を持っても衝突させないため。sub-05/06 は `MyeKernels.<Name>.<Struct>` で参照する。
  - [追加] 生成関数 `WriteCSharpKernels(assetsRoot)` は `schema::WriteIfChanged` を再利用 (SchemaCodegen と同じ書込み関数)。カーネル 0 本 + 既存ファイル無しなら何も作らない (Schema と同じ流儀)。呼び出し側の接続は sub-06。
  - [追加] 検査エラーの構文違反は最初の 1 件で打ち切り (R8: サブセットの外は解析を広げない)。型検査系は複数件返る。
  - [追加] 開発用の環境変数 `MYE_KERNEL_GOLDEN_DUMP` / `MYE_KERNEL_LINT_FILES` を selftest に足した (合否には影響しない)。
  - [逸脱] 受け入れ条件 3 (C# 互換ライブラリのビルド) は MyeScripting.csproj 単体の `dotnet build` で確認。ソリューション全体のビルドでも MyeScripting は走るが、並列ビルドの nethost.dll コピー競合 (既存) でログにエラーが出ることがあった (リンクは成功)。
検証: 上記「検証」のとおり。失敗なし。
自己採点: 仕様適合 4 / 正しさ 4 / コード品質 4 / テスト 4
  - 仕様適合 4: spec 4.1.2 の全行に負例があり、変換結果の golden もある。ただし仕様の表を超える規則を足した (差分欄)。
  - 正しさ 4: 3 ツールチェーンで受理されることと C# 実走のバイト一致まで確認したが、「検査を通るのに結果が割れる」書き方の網羅は証明していない。
  - コード品質 4: パーサ・検査器・変換器を分離。検査器は 1 つの Checker クラスに集約 (約 900 行)。
  - テスト 4: 負例 118 / 正例 / golden / 生成 / キャッシュ。C# の実走は selftest に入っていない (プローブのみ。sub-06 で入れる)。
不安・質問:
  - float→int / float→uint のキャスト (`(int)x`) は範囲外 / NaN で C++ (未定義)・C# (.NET 8 x64 は 0x80000000)・HLSL が割れる。spec は明示キャストを許可しているので検査器では弾いていない。ホワイトリスト化 (例: `(int)clamp(x, ...)` のみ許す) か、互換ライブラリ側で定義した変換関数に寄せるかは planner 判断を仰ぎたい。
  - `T e;` (未初期化宣言) を許している。C# は `= default` で 0 初期化、C++ / GPU は未定義。禁止にするなら 1 規則足すだけ。
  - 整数の符号付きオーバーフロー (`int` の `+ - *`) は書き手の責任のまま (spec 4.1.2 末尾の注記どおり)。C# は unchecked で巻き戻るが C++ は UB。
触ったファイル:
  - src/Engine/Engine/Compute/KernelAst.h, KernelLint.h, KernelLintInternal.h, KernelParser.cpp, KernelLint.cpp, KernelCSharp.h, KernelCSharp.cpp, KernelLintSelfTest.h, KernelLintSelfTest.cpp (新規)
  - src/Scripting/KernelCompat.cs (新規)
  - src/Editor/App/EditorMain.cpp (selftest の呼び出し 2 行)
  - build/Engine.vcxproj, build/Engine.vcxproj.filters (gen_project_files の再実行結果。ファイル追加に伴う更新なのでステージしてよい)
申し送り:
  - sub-05/06: 生成 C# の呼び口は `MyeKernels.<Name>.CpuKernel.Range` (`delegate* unmanaged<uint, uint, void*, void**, uint*, void>`)。`buffers[i]` / `counts[i]` は KERNEL の宣言順 (RW も RO も混ぜた並び)。`params` は 1 要素へのポインタ。`SourceHash` は `HashKernelSource` と同じ手順の値 (定数 `CpuKernel.SourceHash`)。`KernelName` 定数もある。
  - sub-05: PowerShell のハッシュ照合は未実装のまま (sub-01 の申し送りどおり)。C# 側のハッシュは `WriteCSharpKernels` が C++ の `HashKernelSource` で出すので同じ値になる。
  - sub-06: 生成ファイルを書く箇所に `kernel::WriteCSharpKernels(assetsRoot, &cache)` を呼ぶ。検査に落ちたカーネルは Kernels.gen.cs に入らない = C# の CPU 版なし (ログにも違反が出る)。`KernelLintCache` はスレッドセーフ (mutex)。
  - sub-06: C# の State が使う `RWView<T>` は `T : unmanaged` が必要。ユーザーの struct を生成 struct と同じレイアウトで受ける設計にすること (MyeKernels.<Name>.<Struct> をそのまま使うのが一番簡単)。
  - 次の reviewer 向け: `KernelLint.cpp` の `BinaryType` と `CheckBuiltin` が規則の中心。golden を変えるときは `MYE_KERNEL_GOLDEN_DUMP` で実際の出力を取る。

### round 2 (coder) — FIX_REQUEST #1〜#3

- #1: float -> int / uint の C 形式キャストを KL008 で禁止 (メッセージで FloatToInt / FloatToUint を案内)。`FloatToInt` / `FloatToUint` を検査器のビルトイン・予約語に追加し、同じ式を `src/Shared/KernelCompat.h`・`assets/shaders/kernels/kernel_common.hlsli`・`src/Scripting/KernelCompat.cs` に足した (NaN は 0、範囲外は飽和、0 方向へ切り捨て。上限は int 2147483520.0f / uint 4294967040.0f)。HLSL の NaN 判定は `x != x`。C++ 側の単体確認は KernelSelfTest.cpp `TestFloatConversions` (NaN・±Inf・±1e10・-0.5f・-0.0f・境界値)。C# も同じ表で出力一致を一時プローブで確認。LintAll に 2 関数を入れた。負例 3 本 (C 形式キャスト x2・引数型)。
- #2: 初期化子のない宣言を新規則 KL028 `uninitialized` で禁止 (const 無初期化もここ)。負例 3 本。実カーネルの `SelfTestMixElem e;` は局所 struct をやめ `outv[id].pos = ...` のように要素へ直接書く形に変更 (3 ツールチェーンで確実に通る形。struct 初期化子リストは C# に対応が無いため検査器に足さなかった)。LintAll も同様。C# 変換器の `= default` は到達しないので削除。
- #3: 環境変数 2 つの用途を `KernelLintSelfTest.h` のコメントに書いた (ADR-030 への記載は sub-09 の申し送り)。
- R9: kernel_common.hlsli の冒頭に「オーバーフローしうる整数の計算は uint」の 1 行を足した。

検証: Debug / Release ビルド警告なし。`Editor.exe --selftest` Debug / Release とも exit 0 (lint 節 PASS 167・FAIL 0、規則 KL001〜KL028 すべてに負例、負例 124 本)。check_rules 0 error / 50 warning。MyeScripting.csproj Debug/Release ビルドは前回同様 (Release は再ビルド済み・エラー 0)。FXC (cs_5_0)・MSVC (/W4 /WX /fp:precise) で LintAll / LintMore が通り、C# 実走の出力が C++ とバイト一致 (LintAll 87e05c2d1f0772d3 / LintMore d0d3f9dbd7b6e222。LintMore は FloatToInt を使う形に変更したため前回値と違う)。TieredCompilation=0 でも同じ。

SELF_EVAL: sub-02 (round 2)
実装: #1 FloatToInt/FloatToUint (3 バックエンドの互換層・検査器・負例・正例・C++ 単体 selftest)、#2 KL028 (規則・負例・SelfTestMix の書き換え・golden 更新)、#3 コメント。
仕様との差分:
  - [追加] `SelfTestMix.kernel.hlsl` を書き換えた (局所 struct `e` を廃止して `outv[id].member = ...`)。CPU 版の固定ハッシュは変わらない (Saxpy 1e2749c7308fa99c / Mix 938f8aef9ad7c55c のまま。Kernel 節 PASS で確認)。golden の Mix は新しい出力に更新。
  - [追加] 新規則 KL028 `uninitialized`。`const T x;` もここで弾く (旧: KL001)。
  - [未実装] GPU 側の FloatToInt の実行比較は未実施 (FXC でコンパイルが通ることのみ。FIX_REQUEST どおり Fx は許容)。
検証: 上記のとおり。失敗なし。
自己採点: 仕様適合 5 / 正しさ 4 / コード品質 4 / テスト 4
不安・質問: なし
触ったファイル: src/Engine/Engine/Compute/{KernelLint.h,KernelLint.cpp,KernelParser.cpp,KernelCSharp.cpp,KernelLintSelfTest.h,KernelLintSelfTest.cpp,KernelSelfTest.cpp}、src/Shared/KernelCompat.h、src/Scripting/KernelCompat.cs、assets/shaders/kernels/{kernel_common.hlsli,SelfTestMix.kernel.hlsl}、(round 1 のファイル一式)
申し送り: ADR-030 (sub-09) に環境変数 `MYE_KERNEL_GOLDEN_DUMP` / `MYE_KERNEL_LINT_FILES` と FloatToInt/FloatToUint の仕様を載せる。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。must 2 件: (1) float→int/uint の C 形式のキャストを禁止し、`FloatToInt` / `FloatToUint` (NaN→0、飽和、0 方向への切り捨て) を HLSL・C++ (KernelCompat.h)・C# (KernelCompat.cs) に同じ式で足す。(2) 初期化子のない宣言 `T e;` を禁止する。追加した規則・名前空間・開発用の環境変数は採用し、spec 4.1.1 / 4.1.2 に反映した。
- round 2: VERDICT OK (planner、2026-10-12)。must 2 件とも解消した (FloatToInt / FloatToUint を 3 層で同じ式にした、KL028)。nit: GPU 側の FloatToInt の実行比較は未実施 (Fx なので許容) — 申し送り。
