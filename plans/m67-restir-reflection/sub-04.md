# sub-04: reservoir の配管 (初期化 + resolve、再利用なし) + デバッグ 12 / 14 + `--rt-restir`

- 依存: sub-02, sub-03
- 状態: 未着手
- 往復: 0

## やること

spec §4.3 の配管を「再利用なし (M = 1)」で端から端まで通す = 薄い縦切り。元計画 S2。
ReSTIR on でも off と `--img-diff --tol 1` で一致することが、配管が正しい証拠になる。

1. `rt_common.hlsli`: `struct RtFirstHit { float3 pos; float3 nrm; int inst; int cls; }` と
   `float3 RtTraceRadianceFirstHit(ro, rd, tMax, bounces, inout seed, skyLod, envOnLastHit, out RtFirstHit fh)`。
   既存 `RtTraceRadianceLod` は**これを呼ぶ薄いラッパ** (式を複製しない)。ミスは `inst = -1`、`nrm = 0`、
   `pos = rd` (方向)、`cls = 4`。法線は既存の両面反転後の N。
2. `rt_reproject.hlsli` (新規): `RtClipToPrevUv` / `RtHistoryUv` / `RtReprojectValid` / `RtAdvanceHistory` /
   `RtTemporalAlpha` / `RtLuminance` を `rt_temporal.cs.hlsl` から**純移動** (rt_temporal は include に置き換え)。
   本サブでは rt_refl から使わないが、移動による不変を golden で先に固定しておく。
   ★sub-03 の申し送り: `rt_restir_common.hlsli` の `RtLuminance` は `#ifndef MYE_RT_LUMINANCE_DEFINED` で包んである。
   移動先でも**同じガード**を使うこと — でないと rt_refl が 2 定義を見て再定義エラー = 反射シェーダが落ちて golden が動く。
3. `rt_refl.cs.hlsl`: `cbuffer RtRestirCB : register(b3)` (spec §4.3 の項目。クラス表配列込み)、
   `RWTexture2D<float4> gRsOutPos/Rad/Nrm/Geom/Rpos : register(u1..u5)`、前フレーム reservoir の SRV `t11..t15`
   (本サブでは読まない)。`gRsOn == 0` は**現行と同一のコード経路** (uniform 分岐で早期に現行の書き出しへ)。
   `gRsOn != 0`: `RtTraceRadianceFirstHit` → 初期 reservoir (**sub-03 の申し送りの 4 行そのまま**: `w = lum(Ls)`、
   `wSum = w`、`M = 1` (`RtReservoirUpdate`。lum = 0 でも M = 1)、`W = RtRestirWeight(wSum, M, p̂)`) → u1-u5 へ
   (`geom` = 受け側 N + `length(P − cameraPos)`、`rpos` = G-Buffer の P そのもの、w = 0)、
   u0 (reflRt_) には現行どおり `float4(Ls, 1)`。ジオメトリ無し / roughness 超過は M = 0 を書く
   (`RtReservoirEmpty()` を pack = cls -1)。スカイヒットは `cls = 4`、`ns = 0`、`xs = 方向` (spec §4.2)。
   `RtRestirResolve` は **scale を先に求めてから Ls に掛ける**順序 (sub-03 の申し送り。`(Ls*wSum)/(M*lum)` に
   書き換えると 1 ulp ずれて A5 が落ちうる)。`RtReservoirUnpack` の cls は `round` (切り捨てだと -1 が 0 に化ける)。
4. `rt_refl_restir_spatial.cs.hlsl` (新規): B (t11-t15) と G-Buffer (t7-t10) を読み、**本サブはタップ 0** =
   `RtRestirResolve` → `gRsOut : u0` (reflRestirRt_)、書き戻し `u1-u5` (A。`rpos` = 自画素の P — 統合後の
   reservoir の受け側はこの画素なので、B から写すのではなく G-Buffer の P を書く。本サブでは同値)。
   spatial ループの骨組み (`[loop]`、`MYE_RT_RESTIR_MAX_TAPS`、`gRsSpatialOn` ゲート) は置いてよいが中身は sub-06。
5. `RtPasses`: `struct RtRestirCB` + `static_assert`、`RtReservoirSet { RenderTexture pos, rad, nrm, geom, rpos; }`
   (`rpos` は R32G32B32A32 — spec §4.2 の表)、
   `RtReservoirSlot { RtReservoirSet set[2]; int w, h; uint32_t lastSerial; bool hasLast; }` × `kHistorySlots`
   (遅延確保、リサイズで `hasLast = false`)、`restirCS_` / `restirCB_` / `restirTimer_`、`RestirGpuMs()`、
   `reflRestirRt_`。`RenderReflection`: `view.rtReflRestir != 0` なら u1-u5 を張って refl → spatial →
   `Accumulate(reflRestirRt_)` → `Denoise`。`RtReflResult` に `reservoirM` (A の `rad`) / `reservoirCls` (A の `nrm`)。
   off なら**現行コードのまま** (u1-u5 を張らない)。
6. `RenderDebug`: 12 = `Blit(reservoirM, mode 1, param 32)`、14 = `Blit(reservoirCls, mode 4)`。
   `rt_blit.hlsl` に mode 4 (`a` → `RtReflClassColor` と同じ 5 色。rt_common を include できないなら色表を複製し
   「rt_common.hlsli::RtReflClassColor と一致」のコメント。**空 reservoir = cls -1 = 範囲外 → 黒、スカイヒットは
   cls 4 なので `rgb (= ns) == 0` を見て黒に落とす** — spec §4.1。`nrm` テクスチャを丸ごと blit に渡せば両方見える)。
   ★sub-02 の申し送り: 4〜11 は Blit で早期 return し、**13 は「どの早期 return にも当たらない」ことで CS 経路に
   落ちている**。12 / 14 の if はその連鎖の中 (11 の直後) に置き、13 の経路を塞がないこと。
   12 / 14 は `--rt-refl` 前提 (反射パスの産物を読む)、13 は不要 (spec §4.4)。
7. `RenderView` 末尾 append: `int32_t rtReflRestir = 0; RtReflRestirParams rtReflRestirParams;`。
   `RenderSystem`: `bool rtReflRestir = false; RtReflRestirParams rtReflRestirParams;` → 毎フレーム写す。
   `rtDebugMode ∈ {12, 14}` で `view.rtReflRestir = 1`。
8. `EngineLoop::Config` に `rtRestir`、`--rt-restir` を両 main に。`EngineLoop.cpp` の `[rt]` ログ行と
   `ProfilerWindow` の `rt refl` 行に `restir %.3f ms`。
9. `EditorApp` RT Debug メニュー: `ReSTIR Reflection` トグル (Temporal / SVGF の並び)、mode 12 / 14 の項目。
   `LocalizationTable.inl` に `Menu_RtRestir` / `Menu_RtDbgReservoirM` / `Menu_RtDbgReflClassRefl`。
10. `RtScene::Update` の `classOverride` 引数 (-1 = off) を通すのは sub-06 でよい (ここでは配線不要)。

11. ★sub-03 round 2 の申し送り (spec §4.5): **新シェーダで `isfinite()` / `isinf()` を使わない** — fxc は `/Gis` 抜きだと
    警告 X3577 を出して最適化除去しうる。非有限の防波堤は `!(x < kMax)` の比較で。候補を「外す」ときは
    **`RtReservoirUpdate` を呼ばずに return** (呼んだ時点で M が増える設計)。幾何不一致の棄却も Update より前に置く。

## やらないこと (このサブでは)

- temporal / spatial の統合 (sub-05 / 06)。可視レイ、チューニング UI、`--rt-class-override`。
- `RtReflCB` の変更 (64 B のまま)。

## 触る場所 (planner の見立て)

- `assets\shaders\rt_common.hlsli` (517-562) / `rt_reproject.hlsli` (新規) / `rt_temporal.cs.hlsl` (63-121 → include)
- `assets\shaders\rt_refl.cs.hlsl` / `rt_refl_restir_spatial.cs.hlsl` (新規) / `rt_blit.hlsl` (33-44)
- `src\Engine\Renderer\RayTracing\RtPasses.h` (56-59 `RtReflResult`、116-125 `RtHistory` の隣、154-198 メンバ) /
  `RtPasses.cpp` (131-143 CB 群の隣、160-228 Init のシェーダ登録、649-725 `RenderReflection`、769-803 `RenderDebug`)
- `src\Engine\Renderer\RenderTypes.h` (`RenderView` 末尾 = 395 行付近の `acoustic*` の後ろ)
- `src\Engine\Engine\RenderSystem.h` (139-162) / `RenderSystem.cpp` (1112-1145)
- `src\Engine\Engine\EngineLoop.h` (118-136) / `EngineLoop.cpp` (266-275、1826-1837)
- `src\Editor\EditorMain.cpp` (341-346) / `src\Runtime\RuntimeMain.cpp` (337-342)
- `src\Editor\EditorApp.cpp` (1160-1240) / `src\Editor\Windows\ProfilerWindow.cpp` (134-138)
- `src\Engine\Core\LocalizationTable.inl`

## 受け入れ条件 (このサブ)

1. Debug / Release ビルド緑、`--selftest` 緑、`check_rules.ps1` 緑 (A8 / A9)。
2. `tools\shot_verify.bat` 21 枚全緑 (A1) — 特に `demo_render_rtgi` (ラッパ化) と `demo_render_rtrefl` (uniform 分岐)。
   **1 でも動いたら塗り潰さず報告** (spec §7)。
3. A5: `--render-demo --deferred --rt-refl --rt-no-temporal --rt-no-svgf` で `--rt-restir` 有 / 無の 2 枚が
   `--img-diff --tol 1` PASS (M = 1 の等価性)。maxDiff と diffPixels を実装メモに。
4. A14: `--rt-restir` の絵を 2 回撮って `--tol 0` PASS。
5. `--rt-restir --rt-debug 12` の画像が全面ほぼ赤 (M = 1)、`--rt-debug 14` で鏡面パッチに spin = 赤・柱 = 水色・
   空の映り = 黒。`tests\actual\probe_rtdebug12.png` / `probe_rtdebug14.png` に残す。
6. `[rt]` ログ行に `restir` の ms が出る (A11)。**計測 run は `--frames 20`** (GpuTimer は kFrames=6 のリングを
   7 フレーム目からしか回収しないので frames 6 では全項 0 — sub-01 の実測)。`--rt-restir` 無しのときは 0 で、
   off 経路で `restirTimer_` が Begin/End されないこと。sub-01 のベースライン (frames 20: refl 4.275 ms +
   denoise 22.646 ms) と並べて実装メモに残す。
7. `rt_refl.cs.hlsl` の `gRsOn == 0` 経路は、diff 上で現行の計算行が**そのまま残っている**こと (reviewer が読む)。

## 検証コマンド

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
set B=--render-demo --deferred --rt-refl --rt-no-temporal --rt-no-svgf --warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3 --no-fxaa
cmd /c bin\x64\Release\Runtime.exe %B% --screenshot tests\actual\m1_off.png
cmd /c bin\x64\Release\Runtime.exe %B% --rt-restir --screenshot tests\actual\m1_on.png
cmd /c bin\x64\Release\Runtime.exe %B% --rt-restir --screenshot tests\actual\m1_on2.png
cmd /c bin\x64\Release\Editor.exe --img-diff tests\actual\m1_off.png tests\actual\m1_on.png --tol 1
cmd /c bin\x64\Release\Editor.exe --img-diff tests\actual\m1_on.png tests\actual\m1_on2.png --tol 0
cmd /c bin\x64\Release\Runtime.exe %B% --rt-restir --rt-debug 12 --screenshot tests\actual\probe_rtdebug12.png
cmd /c bin\x64\Release\Runtime.exe %B% --rt-restir --rt-debug 14 --screenshot tests\actual\probe_rtdebug14.png
rem GPU 時間の計測 run (frames 20。標準出力の [rt] 行を実装メモへ)
cmd /c bin\x64\Release\Runtime.exe --render-demo --deferred --rt-refl --rt-restir --warp --no-audio --font-embedded --width 960 --height 540 --frames 20 --shot-frame 3 --no-fxaa --screenshot tests\actual\measure_restir.png
```

## 実装メモ (coder が追記)

## フィードバック履歴
