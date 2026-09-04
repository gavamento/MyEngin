# sub-06: spatial reuse (ReflectionClass 駆動) + 可視レイ + `--rt-class-override` + チューニング UI

- 依存: sub-05
- 状態: 未着手
- 往復: 0

## やること

spec §4.3 の spatial、§4.1 の `--rt-class-override`、§4.4 のチューニング UI、§4.6 の coder 側。元計画 S4 + S5 の道具。

1. `rt_refl_restir_spatial.cs.hlsl`: 中心 reservoir (B) のクラス `c0` → `k = gRsClass[c0].y`、`r = gRsClass[c0].x`。
   Vogel 螺旋 k 点 (半径 r、内部解像度 px、回転角 = `RtNextRand2(seed)` から。seed は `uint3(px, gRsFrameIndex*16+23)`
   など既存と衝突しない系列)。候補ごとに: 画面内 → B の `geom` で `RtReprojectValid` (受け側の面一致) →
   候補の `cls_n` で `length(offset) ≤ gRsClass[cls_n].x` → `L' = normalize(xs_n − P)` (スカイは `xs_n`) が
   `dot(L', N) > 0` → `J = RtRestirJacobian(xs_n, ns_n, P_n, P)` (`P_n` は候補の `gp` から `gRfPosition.Load`) が
   `[1/gRsJacobianMax, gRsJacobianMax]` 内 → `gRsVisRay != 0` なら `RtTraceAnyHit(P + N·eps, L', d − 2·eps)` で
   遮蔽なし → `RtReservoirMerge(..., mCap[cls_n], J, rnd)`。統合後 `RtRestirClampM` → resolve → 書き戻し。
   `gRsSpatialOn == 0` は sub-04 のタップ 0 経路。
2. `RtScene::Update(instances, resources, int classOverride)`: `≥ 0` なら全 `inst.reflectionClass` に強制。
   `RenderSystem` から `rtReflRestirParams.classOverride` を渡す。**Material は書き換えない**。
3. CLI (両 main): `--rt-restir-no-spatial` / `--rt-restir-visray` / `--rt-class-override N` → `EngineLoop::Config` →
   `RenderSystem.rtReflRestirParams`。
4. `RenderReflection`: `Accumulate` の maxHistory と `Denoise` の iterations を、ReSTIR on のとき
   `rtReflRestirParams.svgfHistory / atrousIterations` から (off は定数のまま)。
5. `EditorApp` RT Debug → `ReSTIR` サブメニュー (spec §4.4): Spatial / Visibility ray / クラス上書き (Off + 5) /
   クラス表 5 行 × 3 スライダ / SVGF history / A-Trous iterations / Reset。ReSTIR off で `BeginDisabled`。
   スライダは `SliderFloat` / `SliderInt` を `SetNextItemWidth(160)` で (Froxel サブメニューの流儀)。
   クラス名は sub-02 の `ReflClass_*` を再利用。**`Tr()` を printf の唯一の引数にしない**。
6. `LocalizationTable.inl`: `Restir_Menu` / `Restir_Spatial` / `Restir_VisRay` / `Restir_ClassOverride` /
   `Restir_Radius` / `Restir_Taps` / `Restir_MCap` / `Restir_SvgfHistory` / `Restir_Atrous` / `Restir_Reset` (en/ja)。
   同じ `###` 右辺が 5 行で衝突しないよう行ごとに `PushID(cls)`。
7. A7 の観測 (sub-05 の Python を再利用)。

## やらないこと (このサブでは)

- 既定値の調整 (S5 = ユーザー)。ADR / spec / README (sub-07)。
- クラス間の線形補間 (境界アーティファクトが S5 で見えたときの手段。今は入れない)。
- unbiased (MIS) 化。

## 触る場所 (planner の見立て)

- `assets\shaders\rt_refl_restir_spatial.cs.hlsl`
- `src\Engine\Engine\RayTracing\RtScene.h/.cpp` (`Update` の引数) / `src\Engine\Engine\RenderSystem.cpp` (1120 行付近の呼び出し)
- `src\Engine\Renderer\RayTracing\RtPasses.cpp` (`RenderReflection` の SVGF 引数)
- `src\Engine\Engine\EngineLoop.h/.cpp` / `src\Editor\EditorMain.cpp` / `src\Runtime\RuntimeMain.cpp` (CLI)
- `src\Editor\EditorApp.cpp` (RT Debug メニュー、Froxel サブメニュー 1131-1157 行が雛形) / `LocalizationTable.inl`

## 受け入れ条件 (このサブ)

1. ビルド緑 / selftest 緑 / check_rules 緑 (規則 10 = 書式指定子) / `shot_verify.bat` 21 枚緑 (A1)。
2. A14: `--rt-restir` (spatial 既定 on) の絵を 2 回撮って `--tol 0` PASS (タップ回転が凍結シードで固定される)。
3. A7-a: `--rt-restir --rt-class-override 0` と `--rt-class-override 3` の絵 (frame 40、`--rt-anim-seed
   --rt-no-temporal --rt-no-svgf`) が `--img-diff --tol 0` で **FAIL** (= クラスが効いている)。
4. A7-b: フリッカー指標 (sub-05 と同じ領域・手順) が `override 3 (Prop)` ≤ `override 0 (Hero)` ≤ `off`。
   4 値 (off / on 既定 / Hero / Prop) を実装メモの表に。
5. A7-c: `--rt-debug 14` で鏡面パッチに spin = 赤 / 柱 = 水色が映る (`tests\actual\probe_rtdebug14_spatial.png`)。
6. `--rt-restir --rt-restir-no-spatial` の絵が sub-05 時点の `--rt-restir` と `--tol 0` 一致 (spatial off = 純粋な
   temporal のまま。sub-05 の `flicker_on_40.png` と比較)。
7. `--rt-restir-visray` で絵が変わる (maxDiff > 0) かつ落ちない。`[rt]` の `restir` ms が visray 有で増える
   (計測 run は `--frames 20` — A11。frames 6 では GpuTimer が回収前に終わり 0 になる)。
8. A13: 実機 (`cmd /c bin\x64\Release\Editor.exe --render-demo --deferred --rt-refl --rt-restir`) で
   サブメニューのスライダが即時に効き、Reset で既定に戻る。coder は起動して落ちないことまで確認し、
   操作感は reviewer に委ねる (「不安・質問」に書く)。
9. 新しい `themeColor::*` の割り当ては**足していない**こと (足したら固定テストが要る — CLAUDE.md)。

## 検証コマンド

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat
set D=--render-demo --deferred --rt-refl --rt-restir --rt-anim-seed --rt-no-temporal --rt-no-svgf --warp --no-audio --font-embedded --width 960 --height 540 --no-fxaa
cmd /c bin\x64\Release\Runtime.exe %D% --rt-class-override 0 --frames 41 --shot-frame 40 --screenshot tests\actual\cls_hero_40.png
cmd /c bin\x64\Release\Runtime.exe %D% --rt-class-override 0 --frames 42 --shot-frame 41 --screenshot tests\actual\cls_hero_41.png
cmd /c bin\x64\Release\Runtime.exe %D% --rt-class-override 3 --frames 41 --shot-frame 40 --screenshot tests\actual\cls_prop_40.png
cmd /c bin\x64\Release\Runtime.exe %D% --rt-class-override 3 --frames 42 --shot-frame 41 --screenshot tests\actual\cls_prop_41.png
cmd /c bin\x64\Release\Editor.exe --img-diff tests\actual\cls_hero_40.png tests\actual\cls_prop_40.png --tol 0   (FAIL が期待値)
cmd /c bin\x64\Release\Runtime.exe %D% --rt-restir-no-spatial --frames 41 --shot-frame 40 --screenshot tests\actual\nospatial_40.png
cmd /c bin\x64\Release\Editor.exe --img-diff tests\actual\nospatial_40.png tests\actual\flicker_on_40.png --tol 0
cmd /c bin\x64\Release\Runtime.exe %D% --rt-restir-visray --frames 41 --shot-frame 40 --screenshot tests\actual\visray_40.png
```

## 実装メモ (coder が追記)

## フィードバック履歴
