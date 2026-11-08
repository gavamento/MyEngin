# sub-18: review-1 の修正 (復元後の描画の通番、速度の上限、Fast 層の時間、静止状態の流れ、コメント)

- 依存: sub-17 (W5 の後。master の上でメインの作業ツリーで行う。並列なし)
- 状態: OK (コミット待ち)
- 往復: 2

## やること

`plans\m96-disturbance-field\review-1.md` の coder 宛ての指摘 #1〜#5 / #7 を直す。spec §4.1 C / G、§5 の 37。#6 は planner が「許す」と裁定した (spec §2 #28、§4.4) ので、コードは変えず ADR-036 に 1 行足すだけ。

1. (#1、major) 復元後の描画の通番: `RestoreSimSnapshot` (`SimSnapshot.cpp:802` / `:820`) が新品の `DisturbanceState` へ読み込むので、`renderSerial_` が常に 1 に戻り、`DisturbanceFieldPass` (`:197`) / `DisturbanceVolumePass` (`:92`) が「同じ通番」と見て転送を省く。**復元の直前の scene 側の通番を引き継いで +1 する** (例: 代入の前に `scene.Disturbance()` の通番を読み、代入した後に「それ + 1」を書く。`DisturbanceState::Read` のコメントの意図どおりにする)。通番は状態ではないので、ハッシュ・DFLD の中身は変えない
   - テスト: 中身の違う blob A / B を続けて復元すると、通番が A の後 < B の後 で、どちらも復元の前より大きい。そこから同じ tick 数進めても通番が一致しない (review-1 のプローブの再現)
2. (#2、major) 大きな速度で落ちる: spec §4.1 C に足した 2 つの守りを入れる
   - 注入の目標速度 `u` (源の速度・Constant の速度・インパルスの dir × speed・Radial / Cone) の長さを `cellSize × 60` で切ってから混ぜる (2D と Volume の両方)。Radial の発散源の差分も切った後の値で積む
   - 移流の逆追跡 (`DisturbanceStep.cpp:491-495`、`:633-636`、3D の `MakeTriCoord`) で、`FloorToInt` の前にマス座標を `[-2, n + 1]` 程度へ切る (補間は格子外 = 静止状態なので、切っても値は変わらない)
   - 入口 3 つ (`EngineApiTable.cpp:558` の FieldAddImpulse、`AnimatorController.cpp:1347` のアニメイベントの speed、`Components.cpp:2109` の constantVelocity) は非有限だけを拒否する (有限の大きな値は上の切り詰めで安全になる。範囲の UI は今のまま)
   - テスト: 2D / Volume に speed 1e11 / 1e30 / −1e30 の Point / Radial / Cone を積んで 10 step。落ちず、全マス有限で、流れの長さが `cellSize × 60` 以下
3. (#3、major) Fast 層の所要時間: `MYE_SELFTEST_ONLY=disturbance` の Debug が 22.3 秒 (spec §4.4 の線は 2 秒)。重い項目 (Volume の並走・追従・起伏・粒子・波紋の長い tick・snapshot 往復など) を `disturbancelong` (Full) へ移す。移す前と後の Debug / Release の所要時間を、他のビルドが走っていない状態で測って報告する。2 秒に収まらなければ、残った項目と時間を書いて返す (planner が判断する)。Full に移した項目も `--selftest-full` で必ず走ること
4. (#4、minor) `DisturbanceSampleAt` (`DisturbanceStep.cpp:1400-1402`、`:1422-1423`) の格子外の流れを、0 ではなく静止状態 (WindTarget = 風 × windScale。2D は xz、Volume は xyz) として補間する。移流が使う値と同じ関数から取る
   - テスト: 風 (2, 0, 0)・300 tick の場で、縁から 0.1 マスの vx が 2.0 (± 1%)
5. (#5、minor) `ShiftArray` (`DisturbanceStep.cpp:1181-1183`) で、追従で入ってきたマスの流れを 0 ではなく目標速度で埋める (濃さは restDensity、跡・波紋は 0 のまま)
   - テスト: 風 (2, 0, 0) で相手を 8 マス動かした直後、入ってきた列の vx が 2.0 (± 1%)
6. (#7、minor) `TickRunner.cpp:251-253` の RunOneTick の説明コメントと、その直後に入った `BuildClipEventImpulse` の順を直す (`BuildClipEventImpulse` をコメントより上へ)
7. (#6) ADR-036 の決定に「インパルスを組む境界の `std::cos` は許す (物理の前例、spec §2 #28)」を 1 行足す
8. 焼き込み値: 2 / 4 / 5 で `kBakedFinalHash` などが動くなら、付録 A.2 どおり `--field-bench` で Debug / Release の値を採り、一致を確かめてから差し替える (旧値 → 新値と理由)

## やらないこと (このサブでは)

- 入口の UI の範囲の変更 (`constantVelocity` の範囲など)
- `coneCos` の近似化 (#6 は許す裁定)
- review-1 で指摘されていない調整値 (霧の濃さ・戻りの速さ)

## 受け入れ条件 (このサブ)

spec §5 の 12, 26, 28, 37。

- 37 (a)〜(f) をテストで (c は計測で) 示す
- **これは最後のサブなので、selftest と replay_verify をこのサブで回す**: `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full` が FAIL 0、`tools\replay_verify.bat` (PowerShell から) が 24 / 24、`tools\check_rules.ps1` 0 error、`tools\shot_verify.bat` の FAIL が既存の `flow_title` だけ
- `Editor.exe --selftest` (既定層、Debug) の全体の所要時間も報告する (AGENTS §7 の 60 秒の目安)

## 検証コマンド

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance'; bin\x64\Debug\Editor.exe --selftest` の所要時間 (移す前 / 後)
- `bin\x64\Release\Editor.exe --field-bench`、`bin\x64\Debug\Editor.exe --field-bench`
- 上の受け入れの全部

## 実装メモ (coder が追記)

SELF_EVAL: sub-18 (round 1) の要点
- #1: `DisturbanceState::InheritRenderSerial` を足し、`RestoreSimSnapshot` が差し替える直前に scene 側の通番 + 1 を入れる。ハッシュ・DFLD は不変
- #2: `LimitInjectionSpeed` (Inject / Inject3D の入口で目標速度を cellSize × 60 に切る。非有限は 0)、`ClampCellCoord` (Bilinear / MakeTriCoord でマス座標を [-2, n+1] へ)。入口 3 つは既存の QueueImpulse / FiniteOr が非有限を拒否済みなので変更なし
- #3: Fast の disturbance は Debug 22.3 秒 → 2.5 秒 (Release 0.42 秒)。追従・粒子・草・地形・地形の跡・2D / Volume の往復・ソルバ・Volume の浮力 / 風 / 並走 (600 tick) を disturbancelong へ。Fast の並走は 30 tick / Volume 20 tick の短縮版
- #4 / #5: DisturbanceSampleAt の格子外 = 風 (WindTarget / WindTarget3D)、ShiftArray の流れの rest = 目標速度
- #7: BuildClipEventImpulse をコメントの上へ。#6: ADR-036 D3 に 1 行
- kBakedFinalHash 0x0D0CBC4DCBEE185E → 0x67C1AB58E919A515 (場面の歩く源が 400 tick ごとの巻き戻しで約 960 m/s を注入していたのが #2 の切り詰めで変わった。--field-bench Debug == Release)

SELF_EVAL: sub-18 (round 2) の要点 (司会が写した)
- 既定層の層分け (M95 の規則): Debug `--selftest` 2 回で 67288 / 66820 ms → simparallel (11260 ms)・watersurface (7832 ms) を Full へ (N+2) → 48623 / 49455 ms (103 suites 0 failed)。SelfTestSuites.cpp のコメントを「上位 12 本」に。`--selftest-full` Debug 116 suites 0 failed (移した 2 本も PASS)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-09)。#1〜#7 の修正、新テスト、kBakedFinalHash 0x67C1AB58E919A515 (Debug == Release)、selftest-full 両構成 0 failed、replay 24 / 24、shot は既存の flow_title だけ、を確認。disturbance の Fast 2.5 秒は受理 (spec の線を緩めた)。must: 既定層全体が Debug 66.6 秒で、M95 の層分けの規則 (55 秒超なら N+1) を割っているので、Full の本数を増やして直す
- round 2: VERDICT OK (planner、2026-11-09)。M95 の層分けの規則どおり simparallel (11.3 秒) → watersurface (7.8 秒) を Full へ (N = 12)、既定層 67.3 → 49.5 秒 (2 回の大きい方)、移したスイートは --selftest-full で PASS。simparallel が既定層から外れるのは規則の想定内 (時間だけで決める規則。決定論の番人は CI の --selftest-full と replay_verify の jobsab が受け持つ)
