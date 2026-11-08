# sub-02: 霧の描画 (2D) と `--field-demo`、replay ジョブ `field`

- 依存: sub-01
- 状態: OK (commit 2687227)
- 往復: 1

## やること

spec §4.3 の「霧」「編集中」「描画への受け渡し」と、デモ・replay ジョブ。**このサブの一番の仕事は spec §7 R1 の判定**
(フロクセルで薄い層の霧の割れ目が見えるか、尾を引きすぎないか) を数値と絵で出すこと。

1. アップロード: RenderSystem (Engine 層) が Scene の `DisturbanceState` から 2D 層 (最大 4 枚) を POD `DisturbanceFieldUpload` に詰め、Renderer 層の新しいパス (`DisturbanceFieldPass`、`AcousticVolumePass` 型) が Texture2DArray `RGBA16F` (vx, vz, ρ, τ) と `RG16F` (R = 波紋 h、G = 基準面の高さのずれ = 列の基準高さ − baseY。sub-10 までは常に 0) へ転送する。寸法は有効な層の最大値、層ごとの UV 範囲と原点・cellSize・baseY・thickness・見た目の欄は CB。通番が前回と同じなら転送しない。float → half は `DirectXPackedVector` の変換など既存にあるもの
   - 3D (sub-07) と粒子・水・地形・草 (sub-13〜13) が同じパスと CB を使う前提で、CB とテクスチャの束縛番号を 1 か所 (C++ と HLSL の共有ヘッダ / 定数) に決める。規則 9 の対象に入れる
   - 状態が無い場 (編集中・Play 前) は、テクスチャを読まず `restDensity` の定数として CB に載せる。描画レーンは sim の状態を作らない
2. フロクセル: `froxel_inject.cs.hlsl:92-93` に場の密度を足す (spec §4.3 の式。`p(y)` と `edgeFade` は C++ の `DisturbanceSampleAt` の 2D の高さの形と同じ式。`p(y)` の基準は baseY + G チャネルのずれ)。散乱に `σ_field × fogColor × fogBrightness`。太陽光 (`fogSunLight`) と自動有効 (`autoFroxel`) は sub-08 — このサブではカメラの `froxelOn` が true のときだけ描く。`FroxelInjectCB` を変えるなら `FroxelPass.cpp:44` の `static_assert(2720)` と HLSL を同時に更新 (場の CB を別の b スロットにするなら 2720 は不変でよい)
3. デモ `--field-demo` (霧の広場): `ShowcaseScenes.cpp` の表の末尾に `{ L"--field-demo", false, L"cache\\field_showcase.scene.json", nullptr, &BuildPlain<&BuildFieldShowcaseScene>, false }`、組み立ては新規 `src\Engine\Engine\Demo\DemoContentField.cpp`。中身: 平らな地面、2D 層の場 1 個 (濃さ + 跡)、`DisturbanceSource` 付きの球 3 個を物理の初速で霧の中へ転がす (スクリプト無しで決定的)、カメラに `CameraPostFx` (`froxelOn = true`、`froxelDensity = 0` = 場の霧だけ)、点光源 1 つ。sub-05 以降で人・爆発を足すので、要素ごとに関数を分ける (memory `demo-entity-order-is-rng-stream`: 後から足す物は関数の末尾)
4. replay_verify のジョブ `field`: `tools\replay_verify.bat` の `:job_feel` (`:400-402`) の後に `:job_field` (`cache\field_showcase.scene.json` を消してから `call :chain cache\golden_field.rep "--field-demo" "--field-demo"`)、`:84` の `-Jobs` 一覧と `:87` の PASS 文、`:157` 付近の mismatch 診断。`tools\mcp\src\verify.rs:15` の `JOBS` (件数 23 → 24、`:251` の assert)
5. (Inspector の注記と、フロクセルの自動有効は sub-08。このサブでは足さない)
6. 計測と判定 (spec §5 の 10):
   - `Runtime.exe --field-demo --screenshot cache\field_t060.png --shot-frame 60` (通過前)、球が通った直後、静止状態へ戻った後の 3 枚
   - 筋の領域と触っていない領域の平均輝度の差 (`Editor.exe --img-diff` か一時プローブ)、戻るまでのフレーム数
   - テンポラルの尾: `--froxel-no-temporal` との比較で、変化が 90% 収まるまでのフレーム数
   - 薄い層の縦解像度: カメラ距離 10 / 20 m で層の厚さ 1 m が何セルにかかるかを `--froxel-dump` か計算で示す
   - 判定に使った画像のパスを SELF_EVAL に書く。「霧として見えない / 尾が 30 フレームを超える」なら**専用描画を足さずに**「不安・質問」で報告する

## やらないこと (このサブでは)

- 流れの移流 (sub-04)。このサブの霧は「晴れて戻る」だけで渦は巻かない
- 地面の層専用の描画 (R1 で必要と判定されたら planner が別サブを足す)
- 3D の描画 (sub-07)、粒子・水・地形・草 (sub-13〜13)
- 平行光の注入とフロクセルの自動有効 (sub-08)
- 地形の起伏に沿う基準面 (sub-10。G チャネルの受け口だけ作る)

## 触る場所 (planner の見立て)

- `src\Engine\Renderer\Passes\AcousticVolumePass.h/.cpp` を型紙に `DisturbanceFieldPass.h/.cpp` (Renderer 層、POD だけ受ける)
- `src\Engine\Engine\Rendering\RenderSystem.cpp:935-949` (フロクセルの有効判定) の近くで詰め替えと転送。フロクセルが off なら転送しない
- `src\Engine\Renderer\Passes\FroxelPass.cpp` / `.h` (SRV と CB の受け渡し)、`assets\shaders\froxel_inject.cs.hlsl`、新しい共有ヘッダ (例 `assets\shaders\disturbance_common.hlsli`)
- `src\Engine\Engine\Demo\ShowcaseScenes.cpp:75-76`、`DemoContent.h`、新規 `DemoContentField.cpp`。既存の `BuildFogShowcaseScene` (`DemoContentVfx.cpp:404-`) の光・カメラの置き方を参考に
- `tools\replay_verify.bat`、`tools\run_parallel.ps1` (一覧があれば)、`tools\mcp\src\verify.rs`
- `src\Engine\Engine\App\EngineCliSelfTest.cpp:442` 付近 (`--feel-demo` の FindShowcase の断言と同じものを `--field-demo` に)

## 受け入れ条件 (このサブ)

spec §5 の 10, 11, 12, 30。

- 10: 上の 6 の数値と画像 3 枚 + テンポラル on / off。筋の領域の平均輝度の差が、触っていない領域とのノイズ (同じ領域を静止状態で 2 回撮った差) の 5 倍以上
- 11: Editor の Scene View (グローバルのフロクセル on) で Play 前に霧が見える画像 1 枚
- 12: replay_verify 全ジョブ PASS (`field` を含む)、`cargo test` (tools\mcp) PASS
- フロクセルを使う既存のデモ (`--fog-demo`) の絵が変わらない (場が無ければ σ に 0 を足すだけ。`--img-diff` で差 0 か、差があるなら理由)
- 30: 新しい文字列の両言語

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance,render,enginecli'; bin\x64\Debug\Editor.exe --selftest`
- 上の 6 のスクショ一式 (Runtime.exe は Release で撮ってよい)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`cargo test --manifest-path tools\mcp\Cargo.toml`

## 実装メモ (coder が追記)

### SELF_EVAL sub-02 (round 1)

- 実装: DisturbanceFieldPass (Renderer、POD の転送指示 + Texture2DArray RGBA16F / RG16F + CB 336B)、disturbance_common.hlsli (CB・束縛番号・DisturbanceFogAt)、froxel_inject への足し込み、FroxelPass の b1 / t1 / t2 / s1 束縛、RenderSystem::UpdateDisturbanceField、DisturbanceRender (詰め替え)、DisturbanceParams (sim と描画で場の切り方を共有)、--field-demo (DemoContentField.cpp)、replay ジョブ field、規則 9 に 6 グループ
- R1 判定 (スクショ・数値) は SELF_EVAL 本文を参照 (司会が planner へ中継した版が正本)
- 検証: Debug / Release ビルド警告 0、check_rules (増えた error なし。既存の harness.md CRLF 1 件は司会の編集物)、cargo test 49 PASS。selftest と replay_verify は運用指示で未実行

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-08)。R1 = 霧は見える・尾は理論 22 フレーム → 専用描画なし。横縞は許容の裁定 ([ユーザーに聞ける])。ノイズは同フレーム内の領域差で測る (19.6 / 0.03)。Play / Stop の Editor 操作確認は sub-17 の手動確認項目へ。selftest / replay_verify は運用どおり sub-07 後
