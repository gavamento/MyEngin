# sub-12: 水面の波紋

- 依存: sub-03 (波 W1。spec §6)
- 状態: OK (統合待ち。W1 の最後)
- 往復: 1


## 並列の注意 (波 W1)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): PS の固定スロット (t32 / t33 / b6 / b7 / s8) を**読むだけ**。VS の変位はしない (VS の束縛の口は W2 の sub-14 が作る)。snapshot・コンポーネントは変えない。デモは `AddFieldDemoPond`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §4.3 の水面、§2 #18。歩く・斬る・物が落ちると水面に波紋が広がる (見た目だけ)。

1. 水面のシェーダ (`water_surface.hlsl`。Gerstner の M79 サーフェス `WaterGerstner.surface.hlsl` も対象にするかは coder が経路を確認して判断し、SELF_EVAL に書く) で、波紋チャネルを持つ 2D 層の `h` を引き、中心差分の勾配を法線に足す。VS の変位はしない (VS の束縛の口は W2 の sub-14 が作る。spec 付録 A.2)
2. 束縛: sub-02 の RG16F の配列 (R = 波紋) を、`water_surface.hlsl` の空いた t スロット (t0〜t8 が使用中、`:82-90`) に。番号は共有ヘッダへ
3. 浮力 (`WaterWaveComponent` の CPU 側) は**変えない**
4. デモ: `--field-demo` の末尾に浅い池 (水面 + 波紋チャネルの 2D 層、`baseY` を水面の高さに) を足し、デモの人の経路が池を横切るようにする

## やらないこと (このサブでは)

- 浮力・当たり判定への反映 (spec §2 #18)
- 水面の解析波 (Gerstner) の変更

## 触る場所 (planner の見立て)

- `assets\shaders\water_surface.hlsl`、`src\Engine\Renderer\Passes\WaterPass.cpp` (束縛)、必要なら `WaterGerstner.surface.hlsl`
- sub-02 の共有ヘッダ
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 23, 30。

- 23: 人が池を渡る前と渡った後の 2 枚で、水面の領域に差がある (`--img-diff`、静止状態 2 回の差の 5 倍以上)。`watersurface` / `waterwave` の selftest 不変
- 波紋チャネルの無いシーン (既存の水のデモ) の絵が変わらない (`--img-diff` で差 0 か、差の理由)
- 12: replay_verify `field` PASS

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='watersurface,waterwave,disturbance,render'; bin\x64\Debug\Editor.exe --selftest`
- スクショ (Runtime.exe)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL sub-12 (round 1): 実装済み・ビルド Release/Debug 成功・check_rules 0 error。スクショ (最終ビルド版) は scratchpad\m96-shots\sub12f_*.png。受け入れ 23: 静止 2 回 PASS (diffPixels=0)、波紋 0 の池 + 新旧シェーダ tol=0 PASS (120/330 フレーム)、波紋ありは旧シェーダと差 (最大 28、76285 画素)。selftest / replay_verify は未実行 (W2 統合後)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-09)。water_surface.hlsl の法線だけ (VS 変位なし、浮力不変)、新しい束縛なし。波紋あり 120 / 330 で新旧の差 (maxDiff 28 / 5)、ripple 0 で新旧ビット一致、静止 2 回で差 0。転送の条件を「波紋だけの層」にも広げた逸脱を受理。Gerstner サーフェスへの波紋は sub-14 (サーフェスシェーダの予約の持ち主) へ回した

## 中断メモ (2026-11-08)

- worktree: C:\HAL\MyEngin\.claude\worktrees\agent-aa741273890b74858 / ブランチ: worktree-agent-aa741273890b74858
- 前提: worktree は master (M96a-c) より古い基点だったので `git merge master` でマージコミットを 1 本作った (統合時に注意)

### 終わったこと
- `assets\shaders\water_surface.hlsl`: `WaterRippleGradient` を追加 (gDfldAux の R の中心差分、PSMain で法線へ加算。勾配 0 なら法線不変)。t32/t33/s8/b6 は ground_fog.hlsli の宣言を再利用 (新しい束縛なし)。VS 変位なし
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (UpdateDisturbanceField): 転送の条件を「霧のある層」から「霧のある層 or 状態つきで波紋配列を持つ層」へ拡張 [追加。付録 A に無いが、波紋だけの層は転送されないため必須]
- `src\Engine\Engine\Demo\DemoContentField.cpp`: `AddFieldDemoPond` + `BuildFieldShowcaseScene` 末尾に 1 行
- `src\Engine\Engine\Disturbance\DisturbanceFieldSelfTest.cpp`: `RunDisturbanceWaterRippleTests` + スイート末尾の呼び出し (selftest は未実行)
- check_rules: 0 error (59 warn は既存)。Release / Debug ビルド成功 (ただしデモの最終調整前の版)
- Gerstner サーフェス (`WaterGerstner.surface.hlsl`) は ground_fog を include しておらず束縛もないので対象外 (SELF_EVAL に書く)

### 途中のこと
- デモの池の配置調整: 広場の霧 (1 m 厚) が水面を覆うので、人の通り道を霧ごと晴らす方針 (densityDelta -1.0)。最終配置は pond (5,0,-2) / 人 z=-3.5,-0.5 / 速度 2.0,2.5 / radius 1.6 に編集済みだが、**この版はまだビルド・撮影していない**
- スクショ: 旧配置で worktree の cache\q30.png, q150.png あり (池が遠く小さい)。WARP が重く 240/300 フレームは 80 秒でタイムアウト (負荷のせい。タイムアウトを 300 秒程度に)
- 本来の保存先 scratchpad\m96-shots\sub12_*.png は未保存

### 次にやること
1. Release / Debug を再ビルド (Runtime.exe が動いていると LNK1104。自分の worktree の PID だけ止める)
2. before (~30) / 通過中 (~120) / 通過後 (~330) / 減衰後 の 4 枚を撮り sub12_*.png で保存。波紋が見えなければ ripple (0.08) を調整
3. 受け入れ 23: --img-diff で前後差と静止 2 回の差を比較。波紋の無い既存の水のデモの絵が変わらないこと
4. selftest / replay_verify は実行しない (指示どおり)。SELF_EVAL を返す

### 触ったファイル
- assets\shaders\water_surface.hlsl
- src\Engine\Engine\Rendering\RenderSystem.cpp
- src\Engine\Engine\Demo\DemoContentField.cpp
- src\Engine\Engine\Disturbance\DisturbanceFieldSelfTest.cpp
- plans\m96-disturbance-field\sub-12.md (このメモ)
