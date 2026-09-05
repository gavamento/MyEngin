# sub-04: reservoir の配管 (初期化 + resolve、再利用なし) + デバッグ 12 / 14 + `--rt-restir`

- 依存: sub-02, sub-03
- 状態: OK (commit: 司会が記入)
- 往復: 1

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

### round 1

```
SELF_EVAL: sub-04 (round 1)
実装:
  - assets/shaders/rt_common.hlsli:RtFirstHit / RtTraceRadianceFirstHit — 反射レイの 1 周目の
    (pos, nrm, inst, cls) を out で返す本体。既存 RtTraceRadianceLod はこれを呼ぶだけのラッパに。
    ループ内の追加は b==0 の代入 4 行のみ (計算行は 1 つも触っていない)。ミス = pos=rd / nrm=0 /
    inst=-1 / cls=4。
  - assets/shaders/rt_reproject.hlsli (新規) — RtClipToPrevUv / RtHistoryUv / RtReprojectValid /
    RtAdvanceHistory / RtTemporalAlpha / RtLuminance を rt_temporal.cs.hlsl から**純移動**。
    RtLuminance は `MYE_RT_LUMINANCE_DEFINED` の同名ガードで包んだ。
  - assets/shaders/rt_temporal.cs.hlsl — 上の 6 関数を削除し `#include "rt_reproject.hlsli"` に置換。
  - assets/shaders/rt_restir_cb.hlsli (新規、★仕様に無い 3 本目) — `cbuffer RtRestirCB : b3` の
    唯一の宣言 + `RtRestirClassParams(cls)` (範囲外は中立クラス) + `RtRestirSampleDir(r, P)`
    (xs から方向を復元。スカイは xs そのもの)。理由は「仕様との差分」[追加] 1。
  - assets/shaders/rt_refl.cs.hlsl — b3 と u1-u5 と t11-t15 を追加。トレースは gRsOn に関わらず
    RtTraceRadianceFirstHit を**1 回だけ**呼び、u0 に float4(Ls,1) を書いてから `if (gRsOn == 0) return;`。
    on 側は初期 reservoir 4 行 (w=lum / wSum=w / M=1 / W=RtRestirWeight) → pack して u1-u5、
    geom = float4(N, |P-cam|)、rpos = float4(P, 0)。ジオメトリ無し / roughness 超過は
    `RtRestirWriteEmpty` (M=0 / cls=-1 / geom.w=0)。
  - assets/shaders/rt_refl_restir_spatial.cs.hlsl (新規) — B (t11-t15) と G-Buffer (t7-t10) を読み、
    自画素 reservoir を J=1 / rnd=0 で `RtReservoirMerge` → `RtRestirClampM` → `RtRestirResolve` →
    u0 (reflRestirRt_)、書き戻し u1-u5 (組 A、rpos = 自画素の G-Buffer P)。タップ 0 (挿入点をコメントで明示)。
  - assets/shaders/rt_blit.hlsl — mode 4 (a = ReflectionClass の 5 色、`rgb == 0` = スカイは黒) と
    s1 の点サンプラ (mode 4 だけ使用。整数の線形補間で存在しないクラス色が出るのを防ぐ)。
  - RtPasses.h/.cpp — `struct RtRestirCB` (224 B、`offsetof(classTable)==144` も static_assert) /
    `RtReservoirSet` (pos R32G32B32A32 / rad / nrm / geom R16G16B16A16 / rpos R32G32B32A32) /
    `RtReservoirSlot` × kHistorySlots (遅延確保、リサイズで hasLast=false) / `EnsureReservoirs` /
    `RenderRestirSpatial` / `restirCS_` / `restirCB_` / `restirTimer_` / `RestirGpuMs()` /
    `reflRestirRt_` / `pointClamp_`。`RtReflResult` に reservoirM / reservoirCls (組 A の rad / nrm)。
    `RenderReflection` は restirOn のとき refl → spatial → Accumulate(reflRestirRt_) → Denoise。
    `UnbindCompute` を t15 / u5 まで拡張。`RenderDebug` に 12 (Blit mode 1、param=kRtRestirMaxM) と
    14 (Blit mode 4) を 11 の直後・13 より前に追加。
  - RenderTypes.h — RenderView 末尾 append `int32_t rtReflRestir` / `RtReflRestirParams
    rtReflRestirParams`、RtTypes.h を include。
  - RenderSystem.h/.cpp — `rtReflRestir` / `rtReflRestirParams` を持ち毎フレーム写す。
    `rtDebugMode ∈ {12,14}` で view.rtReflRestir を強制 1。`RtRestirGpuMs()`。
  - DeferredPath.cpp — `needRefl` に 12 / 14 を追加 (10 / 11 と同じ扱い)。
  - EngineLoop.h/.cpp — Config::rtRestir、`[rt]` ログに `/ restir %.3f ms`。
  - EditorMain.cpp / RuntimeMain.cpp — `--rt-restir`。
  - EditorApp.cpp — RT デバッグに mode 12 / 14 の項目と `ReSTIR Reflection` トグル。
  - ProfilerWindow.cpp — `rt refl` 行に `%6.3f ms restir` を追加 (表示条件に 12 / 14 も)。
  - LocalizationTable.inl — Menu_RtRestir / Menu_RtDbgReservoirM / Menu_RtDbgReflClassRefl (en/ja)。
仕様との差分:
  - [追加] 新シェーダ 3 本目 `rt_restir_cb.hlsli`。sub-04 は 2 本 (rt_reproject / spatial) の想定だったが、
    `RtRestirCB` (224 B) を rt_refl と spatial の両方が読むので宣言を 2 か所に写経することになる。
    CB のずれは「絵は出るが再利用パラメータが全部でたらめ」という静かな壊れ方をするため、
    M58d の terrain_common.hlsli と同じ流儀で宣言を 1 か所に括り出した。`RtRestirSampleDir` も
    「2 本が 1 文字も違わないこと」が A5 の前提なので同居させている。
  - [追加] **p̂ を評価する方向は「撃った L」ではなく「保存した xs から復元した方向」**
    (`RtRestirSampleDir`)。レイ原点を法線方向へ eps ずらしているので両者は最大 1e-3 rad ずれ、
    粗さ 0.10 の面では pdf が 10% 以上変わる = 再利用ゼロでも絵が動く。次段が xs しか知らない以上、
    初期 reservoir 側を次段に合わせるのが唯一の整合解 (spec §4.2 の `L = normalize(xs − P_q)` そのもの)。
  - [追加] **spatial の resolve に「候補を 1 つも採れなかったら 1spp をそのまま通す」フォールバック**
    (`(r.M > 0) ? RtRestirResolve(r, wSum) : center.Ls`)。spec §4.2 は「M = 0 なら 0」だが、
    そのままだと補間法線が視線の裏へ回った画素 (現行の rt_refl が「鏡面方向で代用する」と
    書いているシルエット際。--render-demo frame 3 で**実測 950 テクセル / 画面 7715 px**) が
    p̂ = 0 → W = 0 → **永久に黒**になる (temporal / spatial を足しても受け側の p̂ が 0 なので
    全候補が落ちる)。A5 が落ちるだけでなく実際の画質劣化なので、「ReSTIR は 1spp を置き換えるが、
    置き換えられない画素は置き換えない」を不変量にした。reservoir 側は M = 0 のまま書き戻す
    (採点できなかったサンプルを次フレームへ渡さない)。**planner の裁定が要る** → 不安・質問 1。
  - [追加] rt_refl のトレース呼び出しは gRsOn の分岐の**外**で 1 回だけ。分岐の中で
    RtTraceRadianceLod / RtTraceRadianceFirstHit を別々に呼ぶと BVH トラバーサルが 2 度
    インライン展開され、fxc が X4714 (レジスタ超過 → 性能低下) を出す (実測。ベース版では出ない)。
    ラッパは本体を呼ぶだけなので返る値はビット同一。
  - [追加] `RtPasses` の b3 は **ReSTIR off でも毎フレーム上げて張る** (on = 0)。UnbindCompute は
    CB を外さないので「off なら張らない」にすると前フレームの gRsOn = 1 が残り、トグルを切った
    次のフレームがまだ ReSTIR 経路を走る (自己レビューで発見。golden は毎回新プロセスなので出ない)。
  - [追加] `DeferredPath` の `needRefl` に 12 / 14 を追加 (sub-04 の「触る場所」に DeferredPath は
    無かったが、これが無いと `--rt-refl` 無しでデバッグ 12 / 14 が何も映らない)。
  - [追加] `view.rtReflRestirParams.svgfHistory` / `atrousIterations` を RtPasses 側で
    [1, kRtTemporalMaxHistory] / [0, 4] にクランプ (M67f のスライダが壊れた値を入れても潰れないため)。
  - [追加] rt_blit に点サンプラ (s1) を追加し mode 4 だけで使う。線形補間だとクラス番号が
    混ざって「存在しないクラスの色」が境界に出る。Blit は RenderDebug からしか呼ばれないので
    golden への影響はゼロ (実測でも 21 枚不変)。
  - [未実装] spatial の `[loop]` 骨組みは置いていない (中身が空だと死コードになるため、
    挿入点をコメントで明示するに留めた)。sub-04 の「置いてよい」に従った選択。
検証:
  - MSBuild Debug / Release x64 → 両方成功、警告 0
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0 (44 スイート全 PASS)
  - `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning
  - `tools\shot_verify.bat` → **21 枚全 PASS**。`demo_render_rtrefl` / `demo_render_rtgi` は
    tol=0 で maxDiff=0 diffPixels=0 anyDiff=0/518400 (= ReSTIR off がビット一致。A1)
  - A5: `--render-demo --deferred --rt-refl --rt-no-temporal --rt-no-svgf` の off / on を
    `--img-diff --tol 1` → **PASS: maxDiff=1 diffPixels=0 anyDiff=120/518400**
    (120 px が 1 LSB だけ違う = p̂ の往復 (fl(p̂·(lum/p̂))) の丸め。tol=0 では 120 px FAIL)
  - A14: `--rt-restir` を 2 回撮って `--tol 0` → **PASS: maxDiff=0 diffPixels=0**
  - 分解検証: `--rt-debug 10` (rt_refl の u0 = 生 1spp) の off / on を tol=0 → **PASS** =
    トレースのラッパ化と reservoir 書き出しは 1spp をビット単位で変えていない。
    `--rt-debug 11` (合成前の反射バッファそのもの) の off / on は tol=1 で PASS
    (maxDiff=1 / 超過 0 px / anyDiff=583。半解像度のテクセル差がバイリニアで広がった数)
  - デバッグ 12 (`tests\actual\probe_rtdebug12.png`): 全面赤。画素実測でジオメトリ =
    (231,72,0) = M=1 (緑 72/255 → linear 0.0625 = 2·(1/32))、背景・roughness 超過 = (231,0,0) = M=0
  - デバッグ 14 (`tests\actual\probe_rtdebug14.png`): 黒 490820 px (スカイの映り込み) /
    灰 (205,205,205) 20388 px = Default / 水色 (147,224,231) 6908 px = Prop (柱) /
    赤 (231,147,147) 284 px = Hero (rdemo_spin)。**spin = 赤・柱 = 水色・空 = 黒** を確認
  - デバッグ 12 は `--rt-refl` 無しでも同一画像 (needRefl の 12/14 追加が効いている)
  - A11 計測 (WARP / Release / **frames 20**):
      render-demo  restir on : refl 4.621 ms (+ denoise 23.064) / **restir 1.498 ms**
      render-demo  restir off: refl 4.056 ms (+ denoise 21.241) / **restir 0.000 ms**
      acoustic     restir on : refl 8.112 ms (+ denoise 16.008) / **restir 1.526 ms**
      (sub-01 の S0 ベースライン: render-demo refl 4.275 / denoise 22.646、acoustic refl 7.992 /
       denoise 18.492。off の値はその run-to-run の幅に収まっている)
  - fxc 直接コンパイル (10.0.26100) で rt_gi / rt_shadow / rt_debug / rt_temporal / rt_variance /
    rt_atrous / rt_shadow_filter / rt_refl / rt_refl_restir_spatial / rt_blit → **警告 0**
  - Debug ビルド (D3D11 デバッグレイヤ on) で `--rt-refl --rt-restir` を実走 → D3D の警告・エラー 0
    (= reservoir を SRV と UAV で同時に張るハザードは無い)
  - `Editor.exe --render-demo --deferred --rt-refl --rt-restir --screenshot` → exit 0、
    restir 0.224 ms (エディタ経路 = viewKey 別スロットでも動く)
  - **未実行**: `replay_verify.bat` (sim 非接触。A10 は sub-07)、`collab_verify` / `crash_verify` /
    `net_verify` (無関係)、ProfilerWindow と RT デバッグメニューの目視 (GUI。コード読みのみ)
自己採点 (1-5):
  仕様適合: 4 — sub-04 の 1〜11 は全て実装し受け入れ条件 1〜7 を満たした。ただし [追加] が 8 件
    (うち 2 件は spec 本文の記述と食い違う: p̂ の評価方向 / M=0 のときの resolve 出力) あり、
    planner の裁定が要る。差分は全部上に書いた。
  正しさ: 4 — golden 21 枚 tol=0、A5 tol=1 PASS、A14 tol=0 PASS、デバッグ 12/14 を画素実測、
    D3D デバッグレイヤ緑、fxc 警告 0 まで確認した。5 にしないのは (a) A5 が tol=0 では
    120 px 落ちる (仕様の許容内だが完全一致ではない)、(b) temporal 経路 (t11-t15 / gRsHistValid /
    gRsPrevViewProj) は宣言と配線だけで**一度も実行していない** (sub-05 で初めて通る) ため。
  コード品質: 4 — CB 宣言を 1 か所に括り出し、off 経路が読んで分かる形 (早期 return 1 つ) に
    保った。日本語コメントは「なぜ」と踏んだ罠 (X4714 / b3 の張りっぱなし / 点サンプラ) を書いた。
    5 にしないのは、spatial シェーダに sub-06 用の未使用宣言 (t9 / t14 / t15、gRsSpatialOn 等) が
    残っていること。
  テスト: 3 — 機械検証は golden 21 + A5 + A14 + デバッグ 12/14 の画素実測まで届いたが、
    **新規に足した自動テストは 0**。追加した C++ は D3D バインドとレイアウトで、レイアウトは
    static_assert 2 本 (224 B / offsetof 144) が守っているものの、A5 / A14 / デバッグ 12/14 の
    確認手順は手打ちのままで shot_verify にも selftest にも載っていない (golden 化は sub-07 の A12)。
不安・質問:
  1. **[追加] の「候補を採れなかった画素は 1spp を通す」を仕様として認めてほしい** (spec §4.2 の
     Resolve は「M = 0 なら 0」)。認めないなら A5 は達成不能で、かつ ReSTIR on でシルエット際が
     永久に黒くなる (実測 950 テクセル)。代替案は 2 つあるがどちらも大きい:
     (a) `RtGgxVndfPdf` の `ndotv <= 1e-6 → 0` を下限クランプに変える = sub-03 の CPU ミラーと
         selftest を書き換える。`ndotl <= 0 → 0` は残るので完全には消えない。
     (b) reservoir に wSum を持たせる (rpos.w の「予備」を使う) = spec §4.2 の保存表の変更。
     私は現在の実装 (フォールバック) が最小で可逆だと考えている。
  2. A5 は tol=1 で PASS だが **tol=0 では 120 px が 1 LSB 落ちる**。原因は p̂ の往復
     (W = lum/p̂ を掛け直す) の丸めで、構造的に避けられない。sub-07 で ReSTIR on の golden
     (`demo_render_rtrefl_restir`) を撮るときは **tol=0 で撮り直す**ので問題にならないはず
     (run-to-run は A14 のとおり完全一致)。この理解で合っているか確認したい。
  3. 新シェーダ 3 本目 (`rt_restir_cb.hlsli`) を認めてほしい (差分 [追加] 1)。認められないなら
     CB 宣言を rt_refl と spatial に写経する形へ戻す。
  4. `RtRestirCB` に `gRsOutSize` / `gRsGbSize` / `gRsCameraPos` を入れた (spec §4.3 の項目には
     無い)。spatial パスが RtReflCB (b2) を読まずに済ませるため = CB 宣言の写経を 1 本に抑えるため。
触ったファイル:
  - assets/shaders/rt_common.hlsli
  - assets/shaders/rt_refl.cs.hlsl
  - assets/shaders/rt_temporal.cs.hlsl
  - assets/shaders/rt_blit.hlsl
  - assets/shaders/rt_reproject.hlsli (新規) + .meta
  - assets/shaders/rt_restir_cb.hlsli (新規) + .meta
  - assets/shaders/rt_refl_restir_spatial.cs.hlsl (新規) + .meta
  - src/Engine/Renderer/RayTracing/RtPasses.h
  - src/Engine/Renderer/RayTracing/RtPasses.cpp
  - src/Engine/Renderer/RenderTypes.h
  - src/Engine/Renderer/DeferredPath.cpp
  - src/Engine/Engine/RenderSystem.h
  - src/Engine/Engine/RenderSystem.cpp
  - src/Engine/Engine/EngineLoop.h
  - src/Engine/Engine/EngineLoop.cpp
  - src/Engine/Core/LocalizationTable.inl
  - src/Editor/EditorMain.cpp
  - src/Editor/EditorApp.cpp
  - src/Editor/Windows/ProfilerWindow.cpp
  - src/Runtime/RuntimeMain.cpp
  - plans/m67-restir-reflection/sub-04.md (この実装メモ)
申し送り:
  - (→ sub-05) temporal の入口は全部用意済み: `gRsPrevPos/Rad/Nrm/Geom/Rpos` (t11-t15) は
    rt_refl に**宣言もバインドも済み** (組 A)、`gRsHistValid` / `gRsUseVelocity` /
    `gRsPrevViewProj` / `gRsPrevCameraPos` / `gRsDepthThreshold` / `gRsNormalThreshold` /
    `gRsJacobianMax` も CB に入って充填済み。velocity SRV (t16) だけ未配線。
    **どれも一度も実行していない** ので、sub-05 が最初の実走者になる。
  - (→ sub-05) 受け側の再投影は `rt_reproject.hlsli` の `RtHistoryUv` / `RtReprojectValid` を
    そのまま使える (rt_refl は include 済み)。組 A の `geom` は RtHistory.geom と同レイアウト。
  - (→ sub-05 / 06) **p̂ の評価方向は必ず `RtRestirSampleDir(r, P)`** を使うこと。撃った L で
    評価すると再利用ゼロでも絵が動く (実測 10% 級)。rt_restir_cb.hlsli に 1 本だけ置いてある。
  - (→ sub-06) spatial のタップ挿入点は `rt_refl_restir_spatial.cs.hlsl` の
    「M67f: ここに近傍タップ〜」のコメント位置。`gRsSpatialOn` / `gRsVisRay` /
    `gRsClassOverride` / `RtRestirClassParams(cls)` は用意済みで**まだ誰も読んでいない**。
    `[loop]` の上限に `MYE_RT_RESTIR_MAX_TAPS` を使うこと (規則 9 の登録を形骸化させない)。
  - (→ sub-06) チューニング UI が `RenderSystem::rtReflRestirParams` を直接触れば効く
    (毎フレーム RenderView へ写している)。`svgfHistory` / `atrousIterations` は RtPasses 側で
    [1,32] / [0,4] にクランプ済み。クラス表は 1 画素も検証していない (タップ 0 のため)。
  - (→ sub-06 / 07) `--rt-class-override` の CLI と `RtScene::Update` の引数はまだ無い
    (`gRsClassOverride` は CB に居るがシェーダは読んでいない)。
  - (→ sub-07) CLAUDE.md の CLI 一覧に `--rt-restir` を載せる (sub-01 の申し送りの `--rt-*` 8 本と
    一緒に)。RT デバッグのモード表 (engine_spec) も 12 / 13 / 14 で更新が要る。
  - (→ sub-07) reservoir は 480×270 で 1 スロットあたり約 14.6 MB (5 枚 × 2 組 × 48 B/px)。
    viewKey ごとに遅延確保するので、エディタで SceneView と GameView を両方出すと約 29 MB。
    ADR に載せるならこの数字。
  - (→ reviewer) 「ReSTIR off = 現行とビット一致」の証拠は golden 21 枚 (tol=0 の
    `demo_render_rtrefl` / `demo_render_rtgi`) と、`--rt-debug 10` の off/on tol=0 一致。
    「M=1 で等価」の証拠は A5 (maxDiff=1 / 超過 0 px)。ReSTIR on の run-to-run は A14 で tol=0。
    画像は `tests\actual\` の m1_off / m1_on / m1_on2 / probe_rtdebug12 / probe_rtdebug14 /
    d10_off / d10_on / d11_off / d11_on (gitignore 配下、再撮影可)。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-09-05)。受け入れ条件 1〜7 を SELF_EVAL の検証欄で確認 (Debug/Release 警告 0 /
  selftest / check_rules 0-0 / golden 21 枚 tol=0 / A5 tol=1 PASS (maxDiff=1、超過 0) / A14 tol=0 / debug 12・14 の画素実測 /
  `[rt]` restir 1.498 ms (frames 20) / fxc 警告 0 / D3D デバッグレイヤ 0)。planner が `rt_refl_restir_spatial.cs.hlsl:83-104`
  (フォールバック)、`rt_refl.cs.hlsl:130-161` (トレース 1 回・`RtRestirSampleDir` で p̂)、`rt_restir_cb.hlsli`、
  `DeferredPath.cpp:1107` (`needRefl` に 10/11 が元からある)、`RtPasses.cpp:168-169, 789-791` (static_assert ×2、b3 を毎フレーム)
  を読んで裏取り。[追加] 8 件は全て承認し spec に取り込んだ (§4.2 フォールバックと p̂ の方向 / §4.3 の 3 本目と CB 項目 /
  §4.4 の needRefl)。[未実装] `[loop]` 骨組み → 「置いてよい」の範囲内。テスト軸 3 (新規自動テスト 0) は正直な採点として
  受け入れ — A5 は sub-04 時点にしか成立しない性質で恒久テストにならず、A14 とパイプラインの固定は sub-07 の golden が担う。
