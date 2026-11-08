# sub-04: 流れのソルバ (移流・投影・風・並列・デバッグ描画・計測)

- 依存: sub-03
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
