# sub-04: 流れのソルバ (移流・投影・風・並列・デバッグ描画・計測)

- 依存: sub-03
- 状態: OK (W1 の統合の先頭)
- 往復: 2

## やること

spec §4.1 B の 5 (移流) / 6 / 7 (濃さの移流)、§4.1 I の風、§4.1 J、§2 #9 の並列、§4.3 のデバッグ描画。これで霧が渦を巻き、風に流れる。

1. `DisturbanceFieldComponent` の末尾に `pressureIterations` (int32、既定 20、範囲 0〜64) と `windScale` (既定 1.0) を足す。snapshot は着手時の版 + 1 (版コメント、`AcousticAudioSelfTest.cpp:129`)
2. 速度の半ラグランジュ移流 → 目標速度 (`PhysicsEnvironmentComponent.windVelocity × windScale`、xz) への減衰 → 速さの上限 → 投影 (発散 → Jacobi `pressureIterations` 回、圧力の初期値 0、格子外 0 → 勾配を引く) → 濃さの移流 → 復元。跡は移流しない (spec §2 #17)
   - 格子外の補間値は静止状態 (流れは目標速度、濃さは `restDensity`) = 開いた境界。PhysicsEnvironment が無ければ風 0
3. 並列: 移流・投影の各反復・発散・勾配・復元・波紋を z 行で `jobs::ParallelRanges` に割る (`JobSystem.h:16-38`)。注入は直列のまま。作業配列 (圧力・発散・ダブルバッファ) は状態ではないので、ハッシュ・スナップショットに入れない (確保し直しを毎 tick しない)
4. デバッグ描画: `drawDebug` の場で、4 マスおきに速度の矢印 (長さ = 速さ × 定数) と濃さの色の十字を `debugLines` に積む (`TickRunner.cpp:625-651` の nav / perception と同じ: 出力レーン、`!ts.resim` のときだけ)
5. replay_verify の `:job_jobsab` (`:411-415`) に `call :jobsab_one cache\jobsab_field.rep "--field-demo"` を足す。`SimParallelSelfTest.cpp` に場の jobs あり / なし一致を足すか、`disturbance` スイートで同じことを断言する
6. 計測 (spec §5 の 15): Release・jobs なし / あり で、128×128・全チャネル・20 反復の step と、ハッシュの ms (数百 tick の平均)。スナップショット 1 枚のバイト数。Debug の `disturbance` スイートの所要時間 (2 秒を超えたら重い断言を Full 層の別スイート `disturbancefull` へ)

## やらないこと (このサブでは)

- 3D (sub-07。移流・投影の関数は y 方向を足せる形にしておくだけ)
- 突風・場所ごとに変わる風 (spec §3 後回し)。`SampleWind` と空力は変えない (読むのは `PhysicsEnvironmentComponent` の欄だけ)
- 渦の強調 (vorticity confinement) — 必要なら planner が判断する

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Disturbance\` の step
- `src\Engine\Core\Ecs\Components.h` / `.cpp` の `DisturbanceFieldComponent` 末尾
- `src\Engine\Engine\Loop\TickRunner.cpp:625-651` (デバッグ線)
- `tools\replay_verify.bat:411-415`
- `src\Engine\Engine\Demo\DemoContentField.cpp` — `PhysicsEnvironmentComponent` に弱い風を足すかは coder の判断 (足したら replay は録り直し不要 = 毎回録る)

## 受け入れ条件 (このサブ)

spec §5 の 3 (移流込みで再表明), 13, 14, 15, 16, 30。

- 3: sub-01 の単調性の断言が移流・投影ありで PASS (双線形の移流は凸結合なので最大偏差は増えない)
- 13: 源が通った直後の tick で、投影前と投影後の発散の最大値を比べて 10% 以下。源の進行方向の左右で速度の回転 (∂vz/∂x − ∂vx/∂z) の符号が逆。`drawDebug` のスクショ 1 枚 (`Runtime.exe --field-demo`、矢印が見える角度)
- 14: jobs あり / なし で 600 tick 毎 tick ハッシュ一致 (selftest) + replay_verify `jobsab` PASS
- 15: Release・jobs なし 1.67 ms/tick 以下 (step + ハッシュ)。超えたら合格にせず数値を報告 (planner が既定値を判断)
- 16: 風 (2, 0, 0) m/s・`windScale = 1` で、静止状態に置いた霧の濃い塊が +x へ流れる (重心の移動量が 600 tick で 理論値 ± 20%)。`windScale = 0` で動かない。replay `physics` / `joints` PASS
- 霧の絵: sub-02 と同じ 3 枚を撮り直し、渦が見える 1 枚を足す

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance,simparallel,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release
- 計測は Release で
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL sub-04 (round 1) は司会への返答に記載。中断メモ (末尾) の「次にやること」1〜4 は完了 (kBakedFinalHash 差し替え済み、Debug / Release ビルド済み、check_rules 0 error)。

## フィードバック履歴

## 中断メモ (2026-11-08)

司会の指示で一時停止。ファイルは戻していない。

終わったこと:
- コード一式は書いて Release でビルド済み (Editor / Runtime、AnimatorControllerSelfTest 以外の警告なし)。Debug ビルドは未実行
- Components.h/.cpp: DisturbanceFieldComponent に pressureIterations (20) / windScale (1.0) を末尾追加 (76 -> 84 B)。SimSnapshot v71、AcousticAudioSelfTest の版の表明を 71 に
- DisturbanceParams / DisturbanceState.h: 新欄の切り方、SolverScratch (作業領域、状態ではない)、DisturbanceAppendDebugLines の宣言
- DisturbanceStep.cpp: 速度の半ラグランジュ移流 + 風への減衰 + 上限 -> 投影 (発散 -> Jacobi -> 勾配) -> 濃さの移流 -> 復元 -> 波紋。z 行で jobs::ParallelRanges に割る。注入は直列
- DisturbanceDebugDraw.cpp (新規、gen_project_files 済み) + TickRunner.cpp から !ts.resim で呼ぶ
- tools/replay_verify.bat: jobsab に --field-demo (record/verify と diag 欄)。未実行
- SimParallelSelfTest.cpp: TestField (600 tick、jobs あり/なしの毎 tick ハッシュ一致)
- DisturbanceFieldSelfTest.cpp: 既存テストを移流・投影ありに合わせて調整、SolverChecks (発散 10% 以下・渦の符号・風)、RunDisturbanceFieldBenchmark、--field-bench (EditorMain.cpp)
- DemoContentField.cpp: 弱い風 (0.4, 0, 0) を足した

実測 (Release、--field-bench。他セッションのビルドで CPU が埋まっていて揺れる):
- 発散: 投影前 0.3138 -> 20 反復後 0.0098 (比 0.031)。隣り合うマスの Jacobi だと 64 反復でも 0.19 で頭打ちだったので、幅 2 の 5 点に変えた (差分として SELF_EVAL に書く)
- 渦: 経路の左右の回転 +0.60 / -0.87 (符号が逆)
- 風 (2,0,0) 600 tick: 重心の移動 19.46 m (理論 19.37)、windScale 0 で 0、0.5 で 9.73
- 放射インパルスは投影で最大の流れが 1.73 -> 0.21 に減る (爆発の流れが弱まる。planner 判断事項)
- 性能 128x128・全チャネル・20 反復・jobs なし: step 約 1.5〜2.0 ms + hash 約 0.43 ms = 約 1.9〜2.4 ms で 1.67 ms 超過 (受け入れ 15 は不合格として報告)。jobs あり 約 0.9〜1.0 ms。反復 0 でも step 約 1.25 ms。スナップショット 408,633 B
- 2 ワールドシナリオの最終ハッシュ @600 = 0xD2D198026566A0CB (最適化の前後で同一 = ビット不変)

途中のこと:
- DisturbanceFieldSelfTest.cpp の kBakedFinalHash はまだ旧値 0x92713D47B7CE447A。上の新値へ差し替える (未実施)
- スクショ: shoot.ps1 -Frames 10,100,200,400 -Prefix sub04_ をバックグラウンドで実行中 (cache\sub04_*.png)。未回収・未保存

次にやること:
1. cache\sub04_*.png を scratchpad\m96-shots\sub04_*.png へ移す (0 バイトでないか確認)
2. kBakedFinalHash を差し替える
3. 渦が見える 1 枚: DemoContentField の AddField に一時的に drawDebug = true を入れて撮影 (frame 100 付近)、撮影後に戻す。戻したら Release を再ビルド
4. Debug ビルド (警告 0 を確認)、check_rules (前回 0 error / 既存 59 warning)
5. 性能: 必要なら planner へ既定値の相談 (hash が 0.43 ms を占める、反復 0 でも 1.66 ms 近い)
6. SELF_EVAL を書く (差分に [追加] 幅 2 の Jacobi、デモの弱い風、--field-bench、既存テストの調整、放射インパルスが投影で弱まること)

触ったファイル: plans の司会分に加え src\Editor\App\EditorMain.cpp、src\Engine\Core\Ecs\Components.h/.cpp、src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp、src\Engine\Engine\Demo\DemoContentField.cpp、src\Engine\Engine\Disturbance\ (DisturbanceDebugDraw.cpp 新規、DisturbanceFieldSelfTest.cpp/.h、DisturbanceParams.cpp/.h、DisturbanceState.h、DisturbanceStep.cpp)、src\Engine\Engine\Loop\SimParallelSelfTest.cpp、TickRunner.cpp、src\Engine\Engine\Replay\SimSnapshot.h、tools
eplay_verify.bat。生成物: build\Engine.vcxproj / .filters (新規ファイル追加のため)

ビルドの状態: Release の MyEngine.sln は最新のコードでビルド済み (Runtime.exe 22:13)。Debug 未ビルド。selftest / replay_verify は未実行 (運用どおり)。
- round 1: VERDICT REWORK (planner、2026-11-08)。移流・投影・風・行並列・デバッグ線・bench を確認。must: (1) 場のハッシュを HashStateBytes に (FoldFloats の 1 GB/s が受け入れ 15 を落としている) (2) Radial を発散源にする (spec §4.1 J)。should: 発散・勾配を前進 / 後退差分の組にして隣接 5 点の Jacobi と一貫させる案を試す (幅 2 の Jacobi は格子が 4 つの部分格子に分かれ、市松の圧力が減衰しない)
- round 2: VERDICT OK (planner、2026-11-08)。#1 ハッシュを HashStateBytes に (0.43 → 0.032 ms)、最良の窓 1.171 ms で受け入れ 15 合格 (平均 1.299、最悪 1.912 は並列ビルドの重なり)。#2 Radial の発散源で反復 20 でも外向きの流れが残り 30 tick で消える。#3 前進 / 後退差分は 20 反復で 22% 残り不採用、幅 2 の 5 点を維持 (逸脱を受理)。市松のムラの目視と、移流後の絵の撮り直し (#4) は sub-17 の手動確認と W2 後の中間確認へ
