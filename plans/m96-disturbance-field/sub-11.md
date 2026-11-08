# sub-11: 粒子 (CPU / GPU)

- 依存: sub-07 (波 W3。spec §6)
- 状態: 未着手
- 往復: 0


## 並列の注意 (波 W3)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): snapshot **v74**。`ParticleEmitterComponent` の末尾と登録。粒子 CS の **t3** (2D 配列) と **t4〜t7** (3D × 4)。規則 9 のグループ「粒子」。デモは `AddFieldDemoDust`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §4.3 の粒子、§4.2 の `ParticleEmitterComponent.fieldInfluence`。砂ぼこり・煙の粒子が場の流れに乗る。

1. `ParticleEmitterComponent` の末尾に `fieldInfluence` (float、既定 0、範囲 0〜10。単位 1/s = 場の流れへ寄せる速さ) を足す。snapshot は spec 付録 A.1 の予約の版
2. CPU 粒子 (`CpuParticleBackend.cpp`、ハッシュ対象の経路): 積分 (`:440-445` の gravity + wind の加速度) に `fieldInfluence × (場の流れ − 粒子の速度)` を足す。場の値は `DisturbanceSampleAt` で、粒子の位置で引く (この tick の step 後の状態。粒子は step の後に回る = spec §2 の Tick 位置)。`fieldInfluence = 0` のエミッタは場を引かない (既存の結果をビット一致で保つ)。粒子のループの並列 (ADR-028) と順序を壊さない — 場は読むだけ
3. GPU 粒子 (`particle_sim.cs.hlsl`、描画レーン): sub-02 の 2D 配列と sub-07 の 3D テクスチャ (最大 4 本) を空いた t スロットに束縛し、同じ式で速度を寄せる。束縛番号は sub-02 の共有ヘッダに足す
4. デモ: `--field-demo` の末尾に、CPU と GPU の砂ぼこりのエミッタ (`fieldInfluence > 0`) を霧の広場に置く

## やらないこと (このサブでは)

- 粒子が場を押し返す (粒子は源にならない)
- `fieldInfluence` 以外の粒子の挙動の変更

## 触る場所 (planner の見立て)

- `src\Engine\Core\Ecs\Components.h` の `ParticleEmitterComponent` 末尾、`Components.cpp` の登録
- `src\Engine\Engine\Particles\CpuParticleBackend.cpp:437-503` (積分とハッシュ)、エミッタの記述子へ値を写す所
- `assets\shaders\particle_sim.cs.hlsl` (t0〜t2 が使用中)、GPU 粒子のディスパッチ側
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 22, 30。

- 22: (a) `fieldInfluence > 0` の CPU 粒子が、一様な流れの場の中で流れの速度へ近づく (selftest、理論値 ± 10%) (b) 2 ワールド一致・snapshot 往復・jobs あり / なし一致 (c) `--particle-demo` の hash 列が着手前と一致 (replay_verify `jobsab` の particle と `demo`) (d) GPU 粒子が流れに乗る絵 1 枚
- 12: replay_verify `field`・`jobsab` PASS

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='particle,disturbance,simparallel,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
