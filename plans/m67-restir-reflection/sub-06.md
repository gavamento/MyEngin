# sub-06: spatial reuse (ReflectionClass 駆動) + 可視レイ + `--rt-class-override` + チューニング UI

- 依存: sub-05
- 状態: OK (commit b8d17aa)
- 往復: 2

## やること

spec §4.3 の spatial、§4.1 の `--rt-class-override`、§4.4 のチューニング UI、§4.6 の coder 側。元計画 S4 + S5 の道具。

1. `rt_refl_restir_spatial.cs.hlsl` (**round 1 の裁定で改訂**、spec §4.2 / §4.3): 中心 reservoir (今フレーム側) のクラス `c0` →
   `k = gRsClass[c0].y`、`r = gRsClass[c0].x`。**`s = min(1, α / gRsRadiusAlphaRef)` で `r_eff = r · s`、1 px 未満なら
   タップ 0** (α は受け側の roughness²。`gRsRadiusAlphaRef` は CB、既定 `kRtRestirRadiusAlphaRef = kRtReflMaxRoughness² = 0.36`)。
   Vogel 螺旋 k 点 (`RtRestirVogelTap`、半径 `r_eff`、内部解像度 px)。**回転は画素ハッシュのみ** — seed を
   `uint3(px, kRtRestirTapSeed)` にしてフレーム項を外す (書き戻しを断ったので脱相関不要。回すと乗り換えフリッカーが倍)。
   候補ごとに: 画面内 → 今フレーム側の `geom` で `RtReprojectValid` (受け側の面一致) → 候補の `cls_n` で
   `length(offset) ≤ gRsClass[cls_n].x · s` → `L' = RtRestirSampleDir(r_n, P)` が `dot(L', N) > 0` →
   `J = RtRestirJacobian(xs_n, ns_n, P_n, P)` (`P_n` は今フレーム側の `rpos`) が範囲内 → `gRsVisRay != 0` なら
   `RtTraceAnyHit` → `RtReservoirMerge(..., mCap[cls_n], J, gRsJacobianMax, rnd)`。統合後 `RtRestirClampM` → resolve →
   **`reflRestirRt_` (u0) だけに書く。u1-u5 (reservoir の書き戻し) は削除**。
   `RtPasses`: 2 組を `RtHistory` と同じ ping-pong に (rt_refl は read を読み write へ、spatial は write を読む、フレーム末に
   flip。debug 12 / 14 は write 側)。`hasLast` の 3 箇所は不変。sub-05 の 2 パス往復 selftest は「履歴 = temporal の出力」
   の流れに合わせて書き換え (W を 0 にする変異は rt_refl 側の W に移す。M の伸びの検査はそのまま)。
   ループ上限は `MYE_RT_RESTIR_MAX_TAPS` (規則 9 に登録済み)。`kRtRestirRadiusAlphaRef` / `kRtRestirTapSeed` を `RtTypes.h` に、
   半径縮小の式 `RtRestirRadiusScale(alpha, ref)` を両言語ミラー + selftest (α = 0.36 → 1、0.25 → 0.694、0.01 → 0.028、
   0 → 0、負 / NaN → 0)。
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
8. ★spec §4.5: `isfinite()` / `isinf()` を使わない (fxc X3577)。タップごとの棄却 (画面外・幾何不一致・クラス半径・
   半球外・J 範囲外・可視レイ) は全て `RtReservoirMerge` に入る前か Merge 内の `w` ゲートで落とす = M 不加算。
9. ★sub-04 の申し送り: タップの挿入点は spatial の「M67f: ここに近傍タップ〜」のコメント位置 (自画素の Merge の直後、
   `RtRestirClampM` の前)。`gRsSpatialOn` / `gRsVisRay` / `gRsClassOverride` / `RtRestirClassParams(cls)` は CB とヘルパが
   用意済みで**まだ誰も読んでいない**。候補の p̂ は `RtRestirSampleDir(r_n, P)` で。`RtScene::Update` の override 引数と
   `--rt-class-override` の CLI はまだ無い (本サブで足す)。チューニング UI は `RenderSystem::rtReflRestirParams` を直接
   触れば効く (`svgfHistory` / `atrousIterations` は RtPasses 側で [1,32] / [0,4] にクランプ済み — UI の範囲を合わせる)。

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
3. A7 (round 1 の裁定で再定義、spec §5 A7): **被写体 2 つ × 5 条件 (off / temporal のみ / spatial 既定 / 一様 Hero /
   一様 Prop)** の表 (平均輝度 / フリッカー = 連続 2 フレームの平均絶対差 / 空間ノイズ) を実装メモに。
   条件は `--rt-anim-seed --rt-no-temporal --rt-no-svgf` (SVGF を外して ReSTIR の寄与だけを見る)。
   - **目標帯** = `--acoustic-demo --deferred --rt-refl --rt-restir`、frame 120 / 121 (`--frames 122 --shot-frame 120/121`)、
     矩形 = `--rt-debug 14` (frame 120) でプレイヤー (赤) / 敵 (橙) が床に映る領域を実測して決める (座標を実装メモに)。
     **(a) spatial 既定のフリッカー < temporal のみ** が機能の効果の証明。**不成立なら `RtReflRestirParams::spatial` の既定を 0
     にして** (CLI `--rt-restir-spatial` を足し、`--rt-restir-no-spatial` は残す)、その数値を実装メモと ADR (sub-07) に残す。
     **(d) `--rt-class-override 3` のフリッカー ≤ `0`**、override 0 と 3 の絵は `--tol 0` で FAIL (異なる)。
   - **鏡面** = `--render-demo` の矩形 (330,255)-(470,400)、frame 40 / 41: **(b) spatial 既定のフリッカー ≤ temporal のみ × 1.05、
     平均輝度 ±1%** (α 比例半径で spatial がほぼ切れる)。
   - **(c) 伝播なし**: 鏡面でクラス混在 (既定) の平均輝度が一様 Default の ±1% 以内。`--rt-debug 14` のクラス別画素数が
     **spatial on と off でビット一致** (round 2 で「frame 3 vs 40 で ±10%」から読み替え。round 1 は 9988 → 40432 で対照
     8264 → 11656 から外れていた = 伝播、round 2 は on/off とも 8396 → 11788 で完全一致 = temporal 由来のみ)。
4. (旧 4 は 3 に統合)
5. A7-c: `--rt-debug 14` で鏡面パッチに spin = 赤 / 柱 = 水色が映る (`tests\actual\probe_rtdebug14_spatial.png`)。
6. `--rt-restir --rt-restir-no-spatial` の絵が sub-05 時点の `--rt-restir` と `--tol 0` 一致 (spatial off = 純粋な
   temporal のまま。sub-05 の `flicker_on_40.png` と比較)。**round 2 では ping-pong 化で履歴の流れが変わるが、タップ 0 なら
   spatial の出力 = temporal の reservoir の resolve = round 1 と同じ値のはず** — 一致しなければ理由を報告 (塗り潰さない)。
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

### round 1 (SELF_EVAL の写し)

実装:
- `assets\shaders\rt_refl_restir_spatial.cs.hlsl` — 近傍タップ本体。中心 reservoir の
  クラスで半径 / タップ数を決め、Vogel 螺旋 (画素ごとに回転) で `[loop]` × `MYE_RT_RESTIR_MAX_TAPS`。
  棄却は全て Merge の前 (画面外 / 自画素 / `RtReprojectValid` / 候補クラスの半径 / 半球外 /
  J 範囲外 / 可視レイ) = M 不加算。`gRsSpatialOn` / `gRsVisRay` / t14 / t15 を読む側に回し、
  読み手のいなかった t9 (`gRsGbMark`) は宣言ごと削除
- `assets\shaders\rt_restir_cb.hlsli` — `RtRestirEffectiveClass` を新設して
  `RtRestirClassParams` に畳んだ (`gRsClassOverride` の読み手。上書きを切り替えた瞬間に
  古い reservoir のクラスも従う)
- `assets\shaders\rt_restir_common.hlsli` / `RtMath.h` — `RtRestirVogelTap` を両言語ミラーで追加
- `assets\shaders\rt_refl.cs.hlsl` — 履歴 UV の解像度を `gRfOutSize` (b2) から
  `gRsOutSize` (b3) へ (sub-05 申し送り)
- `RtScene::Update(instances, resources, classOverride)` / `RenderSystem.cpp` — クラス上書き
- `EngineLoop::Config` + 両 main — `--rt-restir-no-spatial` / `--rt-restir-visray` /
  `--rt-class-override N`
- `EditorApp.cpp` + `LocalizationTable.inl` — ReSTIR サブメニュー (12 文字列、en/ja)
- `RtSelfTest.cpp` — Vogel タップの不変量 (半径内 / 重複なし / 回転で回るだけ / 線形スケール /
  count=0 で NaN を出さない / Hero の全タップが Prop 半径の内側)

検証 (要点):
- ビルド Debug/Release 緑 (`MyeWarnAsError=true`)、`--selftest` exit 0、`check_rules` 0/0、
  `shot_verify.bat` **21 枚 PASS** (A1)
- A14 (受け入れ 2): spatial on frame 40 を 2 プロセスで撮って tol=0 PASS
- 受け入れ 6: `--rt-restir-no-spatial` frame 40 が sub-05 の `flicker_on_40.png` と **tol=0 一致**
- A7-a (受け入れ 3): `--rt-class-override 0` と `3` が maxDiff=205 / 31024 px で相違 = FAIL 期待どおり。
  `--rt-debug 13`(ReSTIR 不要) でも override 3 が 310850 px 変わり、範囲外 7 は off と tol=0 一致
- 受け入れ 7: visray で絵が変わり (maxDiff=190 / 27141 px)、`restir` が 4.786 → 8.570 ms (frames 20)
- **受け入れ 4 (A7-b) は不成立** — 下の「spatial のフリッカー / エネルギー」参照

### spatial のフリッカー / エネルギー (受け入れ条件 4 の不成立と原因調査)

`--rt-anim-seed` frame 40/41、鏡面パッチ (330,255)-(470,400)。

| 条件 (SVGF on = 製品構成) | 平均輝度 | (off 比) | フリッカー | 空間ノイズ |
|---|---|---|---|---|
| off | 83.370 | — | 0.20731 | 2.43637 |
| ReSTIR temporal のみ | 83.357 | -0.0% | **0.15323** | 2.48628 |
| ReSTIR spatial 既定 (クラス混在) | 90.395 | **+8.4%** | 0.49274 | 3.11645 |
| spatial + 一様 Default (override 4) | 80.956 | -2.9% | 0.45296 | 2.81460 |
| spatial + 一様 Hero (override 0) | 81.804 | -1.9% | 0.32141 | 2.73168 |
| spatial + 一様 Prop (override 3) | 79.026 | -5.2% | 0.25734 | 2.43690 |

`--rt-no-temporal --rt-no-svgf` (sub-05 と同じ手順) でも同じ向き:
off 0.91335 / temporal のみ 0.20609 / spatial 既定 1.20617 / Hero 1.40677 / Prop 1.01020。
→ **Prop ≤ Hero は成立、Hero ≤ off は不成立**。

原因の切り分け (仮説を 3 つ潰した):
1. 「フレーム可変のタップ回転がフリッカー源」→ 回転をフレーム固定にする実験で
   patch 1.206 → 0.612。**半分は説明するが off (0.913) より良くはならない**
2. 「受け側の粗さが違う画素から借りている」→ 粗さゲート (|Δroughness| > 0.1 で棄却) を
   入れた実験の出力が**ビット一致** = タップは全て同じ材質に落ちている。**棄却**
3. 「M 不加算の規則 (spec §4.2) の明るい側への偏り」→ 教科書の biased 変種
   (p̂=0 でも M を数える) にした実験で 90.395 → 90.349。**ほぼ無変化 = 棄却**
4. 残った説明 = **クラス混在による M 上限の非対称**。`--rt-debug 14` (反射像側のクラス) の
   画素数で確認:

   | | Hero | Prop | Default |
   |---|---|---|---|
   | frame 3 spatial off | 0 | 8264 | 23520 |
   | frame 3 spatial on | 120 | 9988 | 23400 |
   | frame 40 spatial off | 0 | 11656 | 22720 |
   | **frame 40 spatial on** | 12 | **40432** | **2472** |

   統合の重みが `min(M', mCap[cls'])` に比例するので、mCap 32 の Prop 候補は
   mCap 16 の Default 中心より 2 倍重い。spatial の結果は組 A へ書き戻されて次フレームの
   temporal 候補になるため、**Prop のサンプルが 1 フレームあたり最大 radius[Prop]=12px ずつ
   伝播し、40 フレームで画面の 94% を占拠する**。spec §4.3 の「Hero のサンプルは 2px より
   遠くへ運ばれない」は 1 ホップでは成り立つが、フレームを跨ぐと成り立たない。
   spec §7 が S5 の観測項目に挙げた「クラス境界のアーティファクト」がこの形で出ている。

**式は spec §4.2 / §4.3 のとおりに実装してあり、勝手な緩和は入れていない** (クラス間の
線形補間は sub-06 の「やらないこと」)。判断は planner へ (SELF_EVAL の不安・質問)。

### 画像 (tests\actual、gitignore、再撮影可)

`spatial_on_{40,41}` / `nospatial_{40,41}` / `cls_{hero,prop}_{40,41}` / `ns_{hero,prop}_{40,41}` /
`visray_40` / `m67f_off_{40,41}` / `svgf_{off,temporal,spatial,def,hero,prop}_{40,41}` /
`exp_fixrot_{40,41}` (実験) / `exp_countm_{40,41}` (実験) / `exp_rough_{40,41}` (実験) /
`probe_rtdebug14_spatial` / `m67f_d14_{nospatial_03,spatial_03,nospatial_40}` /
`m67f_d13_{off,prop,bad}` / `m67f_m12_{spatial,nospatial}_40` / `m67f_editor`

### round 2 (FIX_REQUEST の指摘 1〜6)

指摘ごとの対応:
- **#1 spatial の書き戻しを断つ** — `rt_refl_restir_spatial.cs.hlsl` から u1-u5 と
  `RtRestirWriteEmpty` と pack 前の W 再計算を削除し、出力は `gRsOut` (u0) だけにした。
  `RtPasses.h::RtReservoirSlot` に `int write` を足して `RtHistory` と同じ ping-pong に
  (rt_refl は `set[1-write]` を読んで `set[write]` へ、spatial は `set[write]` を読むだけ、
  `restirRan` の後で `slot.write = 1 - slot.write`)。`EnsureReservoirs` のリサイズで
  `write = 0` に戻す。デバッグ 12 / 14 の SRV は `set[slot.write]` (= rt_refl の出力) から。
  `hasLast` の 3 箇所は不変。`RenderRestirSpatial` は UAV を 1 本しか張らない。
  selftest の 2 パス往復は「履歴 = パス 1 の出力」に書き換え (`prev = r`)、M の伸びは
  `r` と `s` の**両方**で検査、W = 0 の変異は rt_refl 側の W に移した。
- **#2 半径を受け側 α に比例** — `RtRestirRadiusScale(alpha, ref) = min(1, α/ref)`
  (α<=0 / ref<=0 / NaN は 0) を `rt_restir_common.hlsli` ⇄ `RtMath.h` のミラーで追加。
  spatial は `radiusEff = radius[c0] * s`、`radiusEff < 1` でタップ 0、候補側の半径制限も
  `radius[cls_n] * s`。`kRtRestirRadiusAlphaRef = kRtReflMaxRoughness² = 0.36` を
  `RtTypes.h` に、CB に `gRsRadiusAlphaRef` (+ 明示パディング float3)、UI にスライダ
  (0.01〜1.0、`Restir_AlphaRef`)。selftest で 0.36→1 / 0.25→0.694 / 0.01→0.028 /
  0→0 / 負→0 / NaN→0 / ref=0→0 と「鏡面で Default 8px が 1px を切る」「目標帯で
  Prop 12px が 1px を超える」を固定。
- **#3 タップ回転をフレームで回さない** — seed を
  `uint3(px, MYE_RT_RESTIR_TAP_SEED)` に。`kRtRestirTapSeed = 23` を `RtTypes.h` と
  `rt_restir_common.hlsli` に置き、`check_rules.ps1` の `$constGroups` に登録 (規則 9)。
- **#4 目標帯での計測** — 下の表。**規則どおり `RtReflRestirParams::spatial` の既定を 0 に**
  した。CLI `--rt-restir-spatial` を追加、`--rt-restir-no-spatial` は残す
  (`--rt-restir-visray` は可視レイがタップの中でしか撃たないので spatial も一緒に立てる)。
  selftest の既定検査を `def.spatial == 0` + `def.radiusAlphaRef == kRtRestirRadiusAlphaRef` に。
- **#5 受け入れ条件 6** — ping-pong 後も `--tol 0` で一致した (下の検証)。
- **#6** round 1 の実験画像は消していない (`exp_*` / `svgf_*` / `m67f_*` が残っている)。

#### 目標帯 = `--acoustic-demo` の床 (粗さ 0.5、α 0.25)

矩形は `--rt-debug 14` (frame 120、`tests\actual\r2_ac_d14_120.png`) の実測で決めた:
プレイヤー (赤 231,147,147、544 px) の塊が **(176,400)-(320,484)**、敵 (橙 231,213,99、264 px) が
**(450,96)-(690,210)**、床全体が **(200,90)-(720,480)**。

**合成画像 (最終絵) の指標は 5 条件とも ±0.4% しか動かない** — 床の粗さ 0.5 では
`RtReflWeight` が IBL と半々に混ぜ、さらに非金属の F0 が小さいので反射の寄与が画素の数 % しか
無く、frame 120→121 の差はほぼ歩行者の実際の移動になる。判断材料にならないので、
**`--rt-debug 11` (= 反射レーンだけ。`--rt-no-svgf --rt-no-temporal` なので ReSTIR の resolve が
そのまま出る)** で測り直した。両方を載せる。

合成画像 (frame 120/121、床全体):

| 条件 | 平均輝度 | フリッカー | 空間ノイズ |
|---|---|---|---|
| off | 27.004 | 0.48232 | 0.38408 |
| temporal のみ | 27.006 | 0.46633 | 0.37429 |
| spatial on (既定クラス) | 27.002 | 0.46810 | 0.37869 |
| 一様 Hero | 27.004 | 0.46686 | 0.37579 |
| 一様 Prop | 27.000 | 0.46723 | 0.38137 |

反射レーンだけ (`--rt-debug 11`、frame 120/121):

| 条件 | player 平均/フリッカー | enemy 平均/フリッカー | 床全体 平均/フリッカー |
|---|---|---|---|
| off | 23.051 / **6.00391** | 22.468 / **4.26343** | 27.340 / **2.94255** |
| temporal のみ | 23.380 / **0.34157** | 22.704 / **0.23935** | 27.659 / **0.18081** |
| spatial on (既定クラス) | 23.355 / 0.36728 | 22.335 / 0.34335 | 27.513 / 0.25461 |
| spatial on + 一様 Hero | 23.351 / 0.51585 | 22.576 / 0.46750 | 27.569 / 0.34677 |
| spatial on + 一様 Prop | 23.319 / 0.23672 | 22.444 / 0.25463 | 27.459 / 0.17013 |

→ **(a) は不成立**: spatial on のフリッカーは 3 矩形とも temporal 単独より大きい
(0.367 > 0.342 / 0.343 > 0.239 / 0.255 > 0.181)。spec §7 U7 の規則に従い
**`spatial` の既定を 0 (off) にした**。ノブ (UI トグル / クラス表 / α 基準) と CLI は残す。
理由の理解: MIS 重みを持たない biased 合成では候補の重みが `p̂_our/p̂_neighbour · wSum_n · J` に
なり、近傍ごとに違う p̂ 比がそのまま重みの分散になる。unbiased 化 (MIS) は spec §3 の
「やらない」なので v1 では解けない。

出荷構成 (spatial off = 既定) でのクラス別 (`--rt-debug 11`、frame 120/121):

| 条件 | player | enemy | 床全体 |
|---|---|---|---|
| 既定 (temporal のみ) | 0.34157 | 0.23935 | 0.18081 |
| 一様 Hero (M 上限 8) | 0.58063 | 0.42027 | 0.32265 |
| 一様 Prop (M 上限 32) | **0.17736** | **0.14239** | **0.09988** |

→ **(d) 成立**: `override 3 (Prop)` ≤ `override 0 (Hero)` が 3 矩形とも成立。
合成画像でも override 0 と 3 は `--tol 0` で **FAIL** (maxDiff=3 / 8927 px) = クラスが効いている。
★S5 / M67h 向けの所見: **既定クラス表の M 上限は目標帯では低すぎる** — 一様 Prop (32) が
既定混在より 1.8〜1.9 倍良い。半径・タップではなく M 上限が効いている。

#### 鏡面 = `--render-demo` の矩形 (330,255)-(470,400)、frame 40/41

| 条件 | 平均輝度 (off 比) | フリッカー | 空間ノイズ |
|---|---|---|---|
| off | 83.314 (—) | 0.91335 | 2.59919 |
| temporal のみ | 83.377 (+0.08%) | 0.20609 | 2.51041 |
| spatial on (既定クラス) | 83.223 (-0.11%) | **0.19839** | 2.49789 |
| spatial on + 一様 Hero | 83.352 (+0.05%) | 0.28943 | 2.51013 |
| spatial on + 一様 Prop | 83.405 (+0.11%) | 0.18048 | 2.53272 |
| spatial on + 一様 Default | 83.231 (-0.10%) | 0.24248 | 2.50815 |

→ **(b) 成立**: spatial on のフリッカー 0.19839 ≤ temporal 単独 × 1.05 (= 0.21639)、
平均輝度の差は -0.18% (±1% 以内)。round 1 は 1.20617 / +8.4% だった。
α 比例半径で鏡面板そのものはタップ 0 になり (8px × 0.028 = 0.22px < 1)、
矩形内の粗い面 (チェッカー床など) だけが spatial に乗る。

→ **(c) 成立 (混在)**: 既定混在 83.223 と一様 Default 83.231 の差は **-0.01%** (±1% 以内)。
round 1 は +8.4% だった。

→ **(c) クラス画素数**: `--rt-debug 14` の Prop 画素は frame 3 で 8396、frame 40 で 11788
(**+40.4%**) で、条件文の ±10% には収まらない。ただし **spatial off の対照が
まったく同じ 8396 → 11788 (画素数がビット一致)** なので、この増加は **temporal 再利用による
もので spatial の伝播ではない**ことが確定した — 書き戻しを断った結果、デバッグ 14 が読む
reservoir は rt_refl の出力だけになり、**spatial の有無で 1 画素も変わらない**。
round 1 は 9988 → 40432 (+305%) で、spatial off の対照 (8264 → 11656) から大きく外れていた。
±10% の閾値は「temporal 単独でもクラス分布は動く」ことを織り込んでいなかったと考えられる
(planner へ: 判定は「spatial on と off が一致すること」に読み替えるのが正確)。

#### GPU 時間 (WARP / Release / `--frames 20`、render-demo)

| 条件 | refl | denoise | restir |
|---|---|---|---|
| off | 5.929 | 27.519 | 0.000 |
| `--rt-restir` (既定 = spatial off) | 6.520 | 23.327 | **2.320** |
| `--rt-restir-spatial` | 7.694 | 23.747 | **2.826** |
| `--rt-restir-visray` | 6.577 | 26.970 | **6.166** |

#### 検証 (round 2、最終ビルドで実行)

- MSBuild Debug / Release (`/p:MyeWarnAsError=true`) → 0 warning / 0 error
- `bin\x64\Debug\Editor.exe --selftest` → exit 0
  (`restir: radius scale alpha 0.25 -> 0.694 / 0.01 -> 0.028`、`temporal loop M = 16`、
   `vogel taps max |offset| / radius = 0.9682`)
- `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning (規則 9 に `kRtRestirTapSeed` 追加)
- `tools\shot_verify.bat` → **21 枚 PASS** (A1。`demo_render_rtrefl` / `_rtgi` は tol=0)
- 受け入れ 6 / 指摘 #5: `--rt-restir-no-spatial` frame 40 も、既定 (`--rt-restir` 単体) frame 40 も
  sub-05 の `flicker_on_40.png` と **`--tol 0` PASS** (maxDiff=0) — ping-pong 化しても
  タップ 0 の値は変わらない (書き戻していた値は元々 rt_refl の出力と同一だった)
- A14: 既定 (spatial off) / `--rt-restir-spatial` の両方で frame 40 を 2 プロセス撮影 → tol=0 PASS
- 受け入れ 7: `--rt-restir-visray` は spatial on と maxDiff=30 / 980 px 相違、restir 2.826 → 6.166 ms。
  `--rt-restir-spatial` は既定 off と maxDiff=166 / 20745 px 相違 = spatial が実際に効いている
- 受け入れ 5 / A7(e): `--rt-debug 14` (`tests\actual\r2_d14_03.png` / `r2_d14_40.png`) で
  Hero 赤 384 px / Prop 水色 8396 px / Default 灰 27840 px (frame 3)
- 受け入れ 8 / A13: Editor が既定と `--rt-restir-spatial --rt-restir-visray` の両方で exit 0
  (`tests\actual\m67f_editor.png` / `m67f_editor_sp.png`)。**スライダの操作感は未確認** (reviewer へ)
- 受け入れ 9: `themeColor::*` 非接触

#### 画像 (tests\actual、gitignore、再撮影可)

round 2: `r2_m_{off,temporal,spatial,hero,prop,def}_{40,41}` (鏡面 5+1 条件) /
`r2_a_*_{120,121}` (目標帯 合成) / `r2_b_*_{120,121}` (目標帯 debug11、spatial on) /
`r2_c_{def,hero,prop}_{120,121}` (目標帯 debug11、出荷構成) / `r2_ac_d14_120` (矩形決め) /
`r2_d14_{03,40}` と `r2_d14ns_{03,40}` (伝播の対照) / `r2f_{default,sp,visray}_40` /
`r2_cimg_{hero,prop}_120` / `m67f_editor{,_sp}`。
round 1 の比較基準 (`exp_{fixrot,rough,countm}_*` / `svgf_*` / `m67f_*` / `probe_rtdebug14_spatial`) は
消さずに残してある。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-09-05)。受け入れ条件 1・2・3(旧)・5・6・7・9 は緑、**4 (A7-b) 不成立** = must。
  coder の測定 (鏡面パッチ、SVGF 有/無の 2 構成 × 5 条件、仮説 3 つの棄却実験) は妥当で、原因の切り分けも正しい方向:
  (1) 書き戻し + フレーム回転で採用サンプルの乗り換えと拡散 (一様クラスでも temporal 単独より悪い)、(2) 鏡面では p̂ が
  小さいタップが M を薄める (暗化)、(3) 被写体が鏡面パッチだけで目標帯 (粗さ 0.5) が未計測。
  裁定 = (C) 書き戻しを断って ping-pong + α 比例半径 + 回転のフレーム固定 + 目標帯での計測で既定 on/off を決める
  (spec §4.2 / §4.3 / A7 / §7 U7)。却下: (A) 基準緩和、(B) `min(mCap)`、(D) 線形補間 (理由は spec §8)。
  [逸脱] 6 件と `gRsOutSize` 寄せ、Vogel selftest → 全て承認。不安 2 (UI 操作) → reviewer の実機。不安 3 (CLI 含意) → 承認。
- round 2: VERDICT OK (planner、2026-09-05)。must 1〜4 を実装と計測で確認: (1) 書き戻し無し + ping-pong
  (`RtPasses.cpp:810-849` の flip、spatial は `u0` のみ) → 伝播消滅 (混在 −0.01%、デバッグ 14 の画素数が on/off でビット一致)、
  (2) α 比例半径 (`RtRestirRadiusScale` 両言語 + selftest 8 ケース) → 鏡面 0.198 ≤ 0.206 × 1.05、平均 −0.18%、
  (3) 回転のフレーム固定 (`kRtRestirTapSeed` 規則 9)、(4) 目標帯の計測 → U7 の規則で**既定 off**。
  golden 21 枚 tol=0 / A14 両モード / 受け入れ 6 (sub-05 の絵と tol=0) / 7 (visray・spatial の効きと GPU 時間) 緑。
  [逸脱] CB 240 B / offset 160 → spec §4.3 を本ラウンドで更新。[追加] `--rt-restir-visray` が spatial も含意 → 承認。
  [逸脱] 3(c) の読み替え → 承認 (spec A7 を更新)。[追加] `--rt-debug 11` での計測 → 承認 (合成画像では反射が数 % しか占めず
  判定材料にならない、は正しい観察)。不安 3 (M 上限が支配的) → spec §7 と sub-07 の ADR 項目に。不安 4 → reviewer の実機。
