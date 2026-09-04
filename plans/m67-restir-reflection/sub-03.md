# sub-03: ReSTIR の数学 — HLSL ⇄ C++ ミラーと selftest

- 依存: sub-02 (`kRtReflClassCount` / `MYE_RT_REFL_CLASS_COUNT` を定数表と CB 配列長に使う)
- 状態: OK (commit: 司会が記入)
- 往復: 2

## やること

spec §4.2 の数式を**両言語で**書き、C++ 側を selftest で固定する。GPU 配管はしない。
`rt_refl.cs.hlsl` に `#include "rt_restir_common.hlsli"` だけ足して**呼ばない** (fxc に通す = 構文の検証。
golden 不変 = コンパイルできている証明でもある。コンパイル失敗は反射が IBL に落ちて golden が動く)。

1. `assets\shaders\rt_restir_common.hlsli` (新規): 
   - `#define MYE_RT_RESTIR_MAX_TAPS 8`。`MYE_RT_REFL_CLASS_COUNT` は rt_common 側 (sub-02) のものを使う。
   - `struct RtReservoir { float3 xs; float W; float3 Ls; float M; float3 ns; int cls; }` + `RtReservoirEmpty()`
   - `RtReservoirPack(r, out float4 pos, rad, nrm)` / `RtReservoirUnpack(float4 pos, rad, nrm)` (spec §4.2 の表)
   - `float RtGgxVndfPdf(float3 N, float3 V, float3 L, float alpha)` (Heitz: `G1(V)·D(H) / (4·NdotV)`、
     α は `max(α, kRtRestirAlphaMin)`、`NdotL ≤ 0` は 0)。`common.hlsli::DistributionGGX` と同じ α 規約
   - `float RtRestirTargetPdf(float3 Ls, float3 L, float3 V, float3 N, float alpha)` = `RtLuminance(Ls) · pdf`
   - `bool RtReservoirUpdate(inout RtReservoir r, <候補>, float w, float rnd)` (streaming RIS、wSum は inout 引数)
   - `RtReservoirMerge(inout r, inout wSum, RtReservoir cand, float pHatAtQ, float mCap, float J, float jMax, float rnd)`
     (`jMax` は CB `gRsJacobianMax` から。round 1 で署名を確定)。**候補を外す = M も数えない**: `cand.M ≤ 0` /
     J 範囲外 / **`w = pHat·W·min(M,cap)·J` が 0 または非有限** — この 4 種は `RtReservoirUpdate` を**呼ばずに** false。
     `RtReservoirUpdate` (自画素の初期サンプル用) は w = 0 でも M を数える (lum = 0 の黒サンプルで M = 1)。
     理由は spec §4.2「M を数える規則」(鏡面パッチの暗化を避ける)
   - `float RtRestirJacobian(float3 xs, float3 ns, float3 Pfrom, float3 Pto)` (スカイ = 1)
   - `void RtRestirClampM(inout r, inout wSum, float mCap)` (書き戻し前クランプ、W 不変)
   - `float3 RtRestirResolve(RtReservoir r, float wSum)` = `Ls · wSum / (M · lum)`、`lum ≤ 0 || M ≤ 0` は 0
2. `RtMath.h`: 同名の CPU ミラー (`RtReservoirCpu` 構造体、`RtGgxVndfPdf`、`RtRestirTargetPdf`、
   `RtReservoirMerge`、`RtRestirJacobian`、`RtRestirClampM`、`RtRestirResolve`)。**式は 1 文字も違えない**
   (既存の `RtGgxVndf` / `RtReflWeight` と同じ流儀のコメント)。
3. `RtTypes.h`: `kRtRestirMaxTaps = 8`、`kRtRestirMaxM = 32`、`kRtRestirAlphaMin = 1e-3f`、
   `kRtRestirJacobianMax = 10.0f`、`struct RtReflClassParams { float radiusPx; float taps; float mCap; float pad; }`、
   `constexpr RtReflClassParams kRtReflClassTable[kRtReflClassCount] = { {2,2,8}, {4,4,16}, {6,6,24}, {12,8,32}, {8,4,16} }`
   (元計画の表。**向きが初版と逆 = Hero ほど保守的**、の理由をコメント)、
   `struct RtReflRestirParams` (POD: `classTable[5]`、`svgfHistory = kRtReflMaxHistory`、
   `atrousIterations = kRtReflAtrousIterations`、`spatial = 1`、`visRay = 0`、`classOverride = -1`、既定 = 定数表。
   sub-04 以降が RenderView に載せる)。
4. `RtSelfTest.cpp` `TestRestir()` (spec A4 の全項目) を連鎖に追加。乱数は既存の `RtSeed` / `RtNextRand2`。
5. `check_rules.ps1` に `kRtRestirMaxTaps / MYE_RT_RESTIR_MAX_TAPS`。

## やらないこと (このサブでは)

- GPU 側の配管、テクスチャ、CB、CLI、UI、デバッグ表示 (sub-04〜06)。
- `RtTraceRadianceLod` の分解 (sub-04)。

## 触る場所 (planner の見立て)

- `assets\shaders\rt_restir_common.hlsli` (新規) / `assets\shaders\rt_refl.cs.hlsl` (12 行目の include の隣)
- `src\Engine\Renderer\RayTracing\RtMath.h` (187-252 の M46h 節の後ろ)
- `src\Engine\Renderer\RayTracing\RtTypes.h` (71-93 の M46h 節の後ろに M67 節)
- `src\Engine\Engine\RayTracing\RtSelfTest.cpp` (392-475 `TestReflection` が雛形。末尾の連鎖に `TestRestir` を足す)
- `tools\check_rules.ps1` `$constGroups`

## 受け入れ条件 (このサブ)

1. Debug / Release ビルド緑。`Editor.exe --selftest` 全緑で `[selftest] rt: restir` の PASS 行が出る (A4):
   - VNDF pdf: **上半球積分 + サンプラの下半球漏れ = 1 (±0.01、決定的 (cosθ, φ) グリッド)** (α = 0.36 と 0.04。
     初版の「半球積分 ≈ 1」は誤り — round 1 で訂正) + ピークが独立な Smith Λ 形の式と一致
   - reservoir 更新: 重み 1:2:7 の 3 候補を 2 万回 → 採用頻度 0.1 / 0.2 / 0.7 ± 0.02
   - M=1: `RtRestirResolve` が `Ls` とビット一致 (lum > 0)、lum = 0 で 0
   - Jacobian: `J(A→B)·J(B→A)` = 1 ± 1e-5、同一点 = 1、`ns = 0` = 1、受け側 2 倍遠で 1/4
   - Merge: 候補の重みが J に比例 / J 範囲外は候補外 (M 不変) / **p̂ = 0 の候補は M も wSum も不変** (round 1 の must) /
     `RtReservoirUpdate` の黒サンプルは M = 1
   - M 上限: 候補 M' = 100・cap 8 → 統合後 M = 9、候補の重みが `p̂·W'·8·J`
   - `RtRestirClampM`: クランプ前後で `wSum/M` 不変
   - 定数表: 5 行、taps ≤ `kRtRestirMaxTaps`、mCap ≤ `kRtRestirMaxM`、クラス 4 = {8, 4, 16}
2. `tools\shot_verify.bat` 21 枚全緑 (include を足した `rt_refl.cs.hlsl` がコンパイルされ、絵が不変) (A1)。
3. `check_rules.ps1` 緑 (A9)。

## 検証コマンド

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
```

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

実装:
- `assets/shaders/rt_restir_common.hlsli` (新規) — `MYE_RT_RESTIR_MAX_TAPS` / `MYE_RT_RESTIR_ALPHA_MIN`、
  ガード付き `RtLuminance`、`struct RtReservoir` + `RtReservoirEmpty` / `Pack` / `Unpack`、
  `RtGgxVndfPdf` / `RtRestirTargetPdf` / `RtRestirWeight` / `RtReservoirUpdate` / `RtReservoirMerge` /
  `RtRestirJacobian` / `RtRestirClampM` / `RtRestirResolve`。冒頭で `#error` により
  「rt_common.hlsli の後に include」を強制 (発火を fxc で実測)。
- `assets/shaders/rt_refl.cs.hlsl` — include を 1 行追加 (呼ばない)。
- `src/Engine/Renderer/RayTracing/RtMath.h` — 上と同名の CPU ミラー + `RtReservoirCpu`。
  **SVGF 節の後ろに置いた** (`RtLuminance` 依存で前方参照になるため)。
- `src/Engine/Renderer/RayTracing/RtTypes.h` — `kRtRestirMaxTaps/MaxM/AlphaMin/JacobianMax`、
  `RtReflClassParams` (16 B) + `kRtReflClassTable` + `RtReflRestirParams`。
- `src/Engine/Engine/RayTracing/RtSelfTest.cpp` — `TestRestir()` + 連鎖に追加。
- `tools/check_rules.ps1` — `kRtRestirMaxTaps / MYE_RT_RESTIR_MAX_TAPS` を `$constGroups` へ。

仕様との差分 (詳細は SELF_EVAL 本体):
- [逸脱] 受け入れ条件 1 の「VNDF pdf の半球積分 ≈ 1 (±3%)」は**事実として成立しない**ので
  「半球積分 + サンプラの下半球への漏れ = 1 (±0.01)」に置き換えた。実測 α=0.36: 0.8837 + 0.1166 = 1.0003 /
  α=0.04: 0.9982 + 0.0019 = 1.0001。積分は決定的な (cosθ,φ) 一様グリッド 256×256 の中点則
  (元の一様半球 1e5 点 MC は α=0.04 で標準誤差 ±3.3% = 判定が乱数任せになる)。
- [逸脱] `RtReservoirMerge` に `jMax` 引数を追加 (spec §4.3 の `gRsJacobianMax` を活かすため)。
- [逸脱] RtMath.h / RtTypes.h の挿入位置 (前方参照とファイル構成の都合)。
- [追加] `RtRestirWeight` / `RtReservoirUpdate` の CPU ミラー / `RtReservoirEmpty().cls = -1` /
  `RtLuminance` のガード / include 順の `#error` / pdf ピークを Smith Λ 形で検査。

検証: Debug + Release ビルド緑 (`MyeWarnAsError=true` でも 0 警告) / `Editor.exe --selftest` は
Debug・Release とも exit 0 (`rt: restir` 全 PASS、数値は上記) / `check_rules.ps1` 0 error 0 warning /
`shot_verify.bat` **21 枚全 PASS** (`demo_render_rtrefl` / `demo_render_rtgi` は tol=0 で maxDiff=0) /
`gen_project_files.ps1` は差分なし (新規は .hlsli のみ)。

### round 2 (VERDICT round 1 の指摘 3 件)

- **#1 (must)** `RtReservoirMerge` を両言語で修正 — `w = p̂·W'·min(M',mCap)·J` を求めた**後**に
  `!(w > 0.0f) || !(w < kWeightMax)` で弾き、**Update を呼ばずに false** (M も wSum も不変)。
  `RtReservoirUpdate` は無変更 (自画素の初期サンプルは lum=0 でも M=1)。
  ★非有限の判定に `isfinite()` を使うと **fxc が警告 X3577 を出して最適化除去しうる**
  (`/Gis` を渡していない) と実測で判明したため、上限 `1e30f` との比較に置き換えた。
  NaN も両方の比較に落ちる。これで実行時コンパイルの HLSL 警告もゼロのまま。
- **#2 (must)** `TestRestir` に 4 件追加: p̂=0 の候補 / W'=0 の候補 / 非有限重み (`INFINITY`) で
  `w3 == 0 && r3.M == 0`、および対比として自画素の黒サンプルが `M = 1` になること。
  **ガードを一時的に外して再ビルドし、この 3 行がちょうど FAIL することを実測**
  (= 検査が回帰を捕まえる形になっていることの確認) してから戻した。
- **#3 (nit)** `RtReservoirUpdate` のコメントに「ここは自画素専用の入口。w>0 のゲートを足すと
  黒い画素の M が 0 に落ちる。候補を外す判定は Merge の仕事」を両言語に追記。

検証 (round 2、すべて再実行): Debug / Release ビルド緑 (`MyeWarnAsError=true`) /
`--selftest` Debug・Release とも exit 0 (`rt: restir` 全 PASS、数値は round 1 と同一) /
`check_rules.ps1` 0 error 0 warning / `shot_verify.bat` **21 枚全 PASS・FAIL 0**
(`demo_render_rtrefl` / `demo_render_rtgi` は maxDiff=0)、実行時コンパイルの HLSL 警告 0 /
fxc で probe (全関数を呼ぶ) と `rt_refl.cs.hlsl` を再コンパイル (警告 0、rt_refl の
バイトコードは round 1 と SHA256 一致) / 両言語の行単位照合を再実行 (差異は言語イディオムのみ)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-09-05)。受け入れ条件 2・3 は緑 (golden 21 枚 tol=0 / check_rules 0-0)、
  条件 1 は coder の置き換え (pdf 半球積分 → 積分 + 漏れ = 1) が**正しく、仕様側の誤り** → 条件 1 を訂正した。
  planner が `rt_restir_common.hlsli` の `RtReservoirUpdate` / `RtReservoirMerge` を読んで確認: `r.M += mInc` が
  `w > 0` の判定より前 = p̂ = 0 の候補で M だけ増える (coder の不安 3 の申告どおり)。これは鏡面パッチ
  (α = 0.01、ローブ ≈ 1° ≈ Default 半径 8 px) で暗化するので **must で差し戻し** (spec §4.2「M を数える規則」)。
  [逸脱] jMax 引数 → 承認 (署名を仕様に反映)。[追加] `RtRestirWeight` / Update の CPU ミラー / `RtLuminance` ガード /
  `#error` / pdf ピーク照合 / 空 reservoir cls = -1 → 全て承認 (cls の規則は spec §4.2 に確定: スカイ = 4、空 = -1)。
  `.meta` の同梱 → 承認 (assets/shaders/*.meta は版管理対象)。replay_verify 未実行は指示外なので問題なし。
- round 2: VERDICT OK (planner、2026-09-05)。planner が両言語の `RtReservoirMerge` を読んで確認: `w` を先に求めて
  `!(w > 0) || !(w < 1e30)` で候補外 (M / wSum 不変)、`RtReservoirUpdate` は無変更 + 「ゲートを足さない」コメント、
  selftest に p̂ = 0 / W' = 0 / INFINITY の 3 件と自画素の対比、シェーダに `isfinite` の呼び出し無し。
  変異テスト (ガード除去で 3 件だけ FAIL) は検査の有効性の証明として妥当。[追加] `isfinite` → 定数比較を承認
  (fxc X3577 の実測根拠)。spec §4.2 / §4.5 と sub-04〜06 に「HLSL で isfinite / isinf を使わない」を反映。
