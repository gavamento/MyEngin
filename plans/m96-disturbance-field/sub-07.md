# sub-07: 3D ボリューム (浮力・3D テクスチャ・フロクセル)

- 依存: sub-06
- 状態: 未着手
- 往復: 0

## やること

spec §4.1 A (Volume)・J の浮力、§4.3 の 3D、§2 #16。立体の煙・霧を置けるようにする。

1. `shape = Volume` の場を sim する (sub-01 で「状態を作らず WARN」にしていた分岐を外す)。流れは 3 成分、チャネルは濃さだけ (跡・波紋のビットは Volume では無視し、Inspector に注記)。注入の距離は 3D のユークリッド距離 (spec §4.1 C)
2. 移流・投影・発散・勾配を y 方向込みに (sub-04 の関数を広げる。2D の結果は 1 ビットも変えない — 2D のハッシュの焼き込み値が不動であることで確かめる)。並列は y スライスで割る
3. `DisturbanceFieldComponent` の末尾に `buoyancy` (既定 0) を足す (`vy += buoyancy × (ρ − restDensity) / 60`)。snapshot は着手時の版 + 1
4. 描画: 有効な Volume を**全部 (最大 4 個、entity index 昇順)** それぞれの Texture3D `RGBA16F` (vx, vy, vz, ρ) へ転送する (sub-02 のパスに追加。3D は配列にできないので SRV 4 本、寸法は各自)。CB に 4 個ぶんの原点・寸法・cellSize・見た目の欄と有効数。フロクセルの注入で 4 個を順に足す `σ += fogDensity × ρ(posW) × edgeFade` (3D の縁 2 マス)。格子の AABB の外では読まない (分岐で抜ける)。太陽光・自己照明・自動有効は sub-03 の規則を Volume にも同じく当てる (spec §2 #14 / #15、§2 #16)
5. デモ: `--field-demo` の末尾に煙の柱 (Volume 32×16×32、Constant の源で `densityDelta > 0`・上向き `constantVelocity`、`buoyancy > 0`) を足す。爆発 (sub-05 の Radial) が煙も吹き飛ばす配置
6. 計測: 32×16×32・20 反復で Release・jobs なし / あり の step + ハッシュ、スナップショット 1 枚のバイト数

## やらないこと (このサブでは)

- 温度・燃焼
- 地形の起伏に沿う基準面 (2D 層だけ。sub-09)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Disturbance\` の step と注入
- `src\Engine\Core\Ecs\Components.h` / `.cpp` の末尾欄
- sub-02 で作った `DisturbanceFieldPass` と共有ヘッダ、`froxel_inject.cs.hlsl`
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 20, 30。

- 20: 浮力ありで濃さの重心が毎 tick 上がる (源を止めた後も一定 tick)。`buoyancy = 0` で上がらない。jobs あり / なし一致、2 ワールド 600 tick 一致、snapshot 往復 100 tick 一致。Volume を 4 個置いた場面で 4 個とも描かれる (スクショ)。Release・jobs なし 1 個 1.67 ms/tick 以下 (超えたら数値を報告)。フロクセルの注入パスの GPU 時間を Volume 0 / 1 / 4 個で報告 (`--render-stats-dump` か GpuTimer)
- 2D の既存の断言 (sub-01 / sub-04 の焼き込み値) が不動
- 12: replay_verify `field`・`jobsab` PASS
- 絵: 煙の柱と、爆発で吹き飛んだ直後の 2 枚

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を実行しない。**ただしこのサブの VERDICT の後に、司会が中間のまとめ実行 (`--selftest-full` Debug / Release と `tools\replay_verify.bat`) を行う**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-14 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance,simparallel,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
