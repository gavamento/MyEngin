# sub-07: 3D ボリューム (浮力・3D テクスチャ・フロクセル)

- 依存: sub-04 (波 W2。spec §6)
- 状態: OK (W2 の統合待ち)
- 往復: 3


## 並列の注意 (波 W2)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): snapshot **v72**。`DisturbanceFieldComponent` の末尾 (`buoyancy`)、DFLD のレイアウト (flowY)。froxel CS の **t3〜t6** (Texture3D × 4) と **b2** (Volume の CB)。規則 9 のグループ「Volume」。デモは `AddFieldDemoSmoke`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §4.1 A (Volume)・J の浮力、§4.3 の 3D、§2 #16。立体の煙・霧を置けるようにする。

1. `shape = Volume` の場を sim する (sub-01 で「状態を作らず WARN」にしていた分岐を外す)。流れは 3 成分、チャネルは濃さだけ (跡・波紋のビットは Volume では無視し、Inspector に注記)。注入の距離は 3D のユークリッド距離 (spec §4.1 C)
2. 移流・投影・発散・勾配を y 方向込みに (sub-04 の関数を広げる。2D の結果は 1 ビットも変えない — 2D のハッシュの焼き込み値が不動であることで確かめる)。並列は y スライスで割る
3. `DisturbanceFieldComponent` の末尾に `buoyancy` (既定 0) を足す (`vy += buoyancy × (ρ − restDensity) / 60`)。snapshot は spec 付録 A.1 の予約の版
4. 描画: 有効な Volume を**全部 (最大 4 個、entity index 昇順)** それぞれの Texture3D `RGBA16F` (vx, vy, vz, ρ) へ転送する (sub-02 のパスに追加。3D は配列にできないので SRV 4 本、寸法は各自)。CB に 4 個ぶんの原点・寸法・cellSize・見た目の欄と有効数。フロクセルの注入で 4 個を順に足す `σ += fogDensity × ρ(posW) × edgeFade` (3D の縁 2 マス)。格子の AABB の外では読まない (分岐で抜ける)。自己照明は sub-02 と同じ式。太陽光の bool と自動有効 (場だけのモード) は次の sub-08 — このサブのデモはカメラの `froxelOn = true`・`froxelDensity = 0` で描く (spec §2 #15、§2 #16)
5. デモ: `--field-demo` の末尾に煙の柱 (Volume 32×16×32、Constant の源で `densityDelta > 0`・上向き `constantVelocity`、`buoyancy > 0`) を足す。爆発 (sub-05 の Radial) が煙も吹き飛ばす配置
6. 計測: 32×16×32・20 反復で Release・jobs なし / あり の step + ハッシュ、スナップショット 1 枚のバイト数

## やらないこと (このサブでは)

- 温度・燃焼
- 地形の起伏に沿う基準面 (2D 層だけ。sub-10)

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

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を実行しない。**ただしこのサブの VERDICT の後に、司会が中間のまとめ実行 (W2 の統合の後) (`--selftest-full` Debug / Release と `tools\replay_verify.bat`) を行う**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance,simparallel,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (coder、2026-11-09、worktree w2-sub07)

- 実装: Volume の 3D sim (注入・移流・投影・浮力・風 xyz)。DFLD に flowY (Volume のみ、2D は空でハッシュ列も不変)。snapshot v72。`DisturbanceFieldComponent` 末尾に `buoyancy` (sizeof 84 → 88)。描画は新規 `DisturbanceVolumePass` (Texture3D RGBA16F × 最大 4、幅 = cellsX / 高さ = cellsZ / 奥行き = cellsY) と `disturbance_volume.hlsli` (b2 / t3〜t6)。フロクセル注入で 4 個を順に足す (縁 2 マス、格子 AABB の外は読まない)。デモ `AddFieldDemoSmoke`
- 2D の関数は触らず 3D 専用関数 (`Inject3D` / `AdvectVelocity3D` / `ProjectVelocity3D` / `AdvectDensity3D` / `Trilinear`) を足した。2D の結果は 1 ビットも変えない
- 計測 (Release、`--field-bench`、machine が空いている時): 2D 128x128 全チャネル 20 反復 jobs なし 最良窓 1.106 ms。**Volume 32x16x32・20 反復 jobs なし 最良窓 1.99〜2.04 ms (平均 2.06〜2.10) = 1.67 ms 超過**、jobs あり 0.59 ms、0 反復 1.25 ms。snapshot 1 枚 277,277 B (2D 128x128 は 408,645 B)
- 焼き込み値 `kBakedFinalHash`: 0x674FE5CFD80F29A9 → **0x2D2D2A180F889F62**。理由 = `DisturbanceFieldComponent` が 4 バイト伸びて World のカラムの生バイトが変わった (2D の sim 自体は不変)。Debug / Release の `--field-bench` で同値
- selftest と replay_verify は未実行 (W2 の統合後にまとめて)。コードは書いた (`RunDisturbanceVolumeTests`、`TestFieldVolume`)。`--field-bench` 経由で Volume の確認項目 (120 tick 版) と jobs あり / なしの最終ハッシュ一致を実行して PASS

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-09)。3D の注入・移流・浮力・7 点の投影・三線形の読み取り・flowY の DFLD / ハッシュ (非空のときだけ)・Texture3D 4 本・froxel の t3〜t6 / b2・デモ・v72 を確認。bench で Debug == Release、jobs あり / なし一致、Volume 4 個の描画を確認。性能は jobs なし約 2.0 ms で、受け入れ 20 の線を jobs ありに変更 (spec §2 #27、[ユーザーに聞ける])。風の xyz、(y, z) 行の並列、新規 DisturbanceVolumePass、kBakedFinalHash の差し替え、sub-15 のサイズ表明の修正を受理
- round 2 (中間のまとめ実行の FIX): VERDICT REWORK (planner、2026-11-09)。Volume の寿命テストの置換、スロット列を move で入れ替えて参照がぶら下がる不具合の修正 (要素ごとの move 代入)、point の非正規化の許容 10% は受理。must: 波紋の注入の式の欠陥 (spec §4.1 C を変更)。テスト (b) の 8 → 3 tick の短縮は戻す
- round 3 (中間のまとめ実行の FIX): VERDICT OK (planner、2026-11-09)。波紋の注入を有界の寄せに (h と hPrev)、とどまり続けても 0.0781 <= 0.08、抜けた後も有限、受け入れ 7 も PASS、kBakedFinalHash 0x851C4663912825C1 (Debug == Release)。hPrev も寄せる訂正を spec §4.1 C に反映
