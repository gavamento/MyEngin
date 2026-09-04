# sub-03: ReSTIR の数学 — HLSL ⇄ C++ ミラーと selftest

- 依存: sub-02 (`kRtReflClassCount` / `MYE_RT_REFL_CLASS_COUNT` を定数表と CB 配列長に使う)
- 状態: 未着手
- 往復: 0

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
   - `RtReservoirMerge(inout r, inout wSum, RtReservoir cand, float pHatAtQ, float mCap, float J, float rnd)`
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
   - VNDF pdf の半球積分 ≈ 1 (α = 0.36 と 0.04、一様半球 1e5 点、±3%)
   - reservoir 更新: 重み 1:2:7 の 3 候補を 2 万回 → 採用頻度 0.1 / 0.2 / 0.7 ± 0.02
   - M=1: `RtRestirResolve` が `Ls` とビット一致 (lum > 0)、lum = 0 で 0
   - Jacobian: `J(A→B)·J(B→A)` = 1 ± 1e-5、同一点 = 1、`ns = 0` = 1
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

## フィードバック履歴
