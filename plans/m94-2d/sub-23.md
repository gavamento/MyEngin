# sub-23: `--2d-demo`、golden `2d` / `2d_lights`、replay_verify `2d`

- 依存: sub-01 〜 sub-22 (全部)
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 4.1.14、4.4 (性能)、5. 38-39

## やること
1. アセット生成 `tools\gen_2d_assets.py` (Python、PNG を Pillow 無しの純標準ライブラリ (zlib + struct) で書く): `assets\2d\tiles.png` (16 px タイル 8 種: 地面上 / 中 / 左縁 / 右縁 / 一方通行板 / 壊れるブロック / 背景 2 種) + `.meta` (filter point、PPU 16、sprites 8)、`assets\2d\hero.png` (32 px、Idle 2 コマ + Walk 4 コマ) + `.meta`、`assets\2d\crate.png` (16 px)、`assets\2d\bg_far.png` / `bg_near.png` (256×64)、`hero_normal.png` (法線)、`assets\2d\hero_idle.spriteanim.json` / `hero_walk.spriteanim.json`、`hero.controller.json` (v2、`"sprite"` ステート 2 つ、float パラメータ `speed` で遷移、Walk の tick 6 に `sound` イベント = 既存のサウンドアセットが無ければ `noise`)、`assets\2d\level.tilepalette.json` (ルール 4 種、アニメ 1 種、hp 3 のブロック)、`assets\2d\level.tilemap.json` (3 レイヤー 60×20)、`assets\2d\hill.spriteshape.json`、`assets\2d\hill_profile.spriteshape.json`。生成は決定的 (seed 固定)。外部素材は使わない。
2. `BuildTwoDShowcaseScene(EngineContext&)` (`DemoContent.cpp`、`kShowcases` に `{ L"--2d-demo", false, L"cache\\2d_showcase.scene.json", nullptr, &BuildPlain<&BuildTwoDShowcaseScene>, false }`。`--2d-night` オプションは `EngineCli` に足し、Build 関数が Light2D を Global 暗 + Spot 2 で組む): spec 4.1.14 の全部品。主人公は `PlayerInputComponent` + GameLogic の小さなスクリプト (`src\GameLogic\Scripts\TwoDHero.cpp`: 左右で `moveInput.x`、ジャンプで `jumpSpeed`、下で `ignoreOneWay`、`speed` パラメータを `AnimatorSetFloat`、攻撃で `TilemapDamageCell` の前方セル) + `--synth-input` の合成入力 (既存 `--flow-demo` の流儀)。敵は GridAgent2D + BT (MoveTo 主人公)。
3. golden: `shot_verify.bat` に `call :shot 2d --2d-demo` (frame 3) と `call :shot 2d_lights --2d-demo --2d-night` (`set SHOT=... --frames 125 --shot-frame 120` の既存流儀)。tol 既定。`tests\golden\2d.png` / `2d_lights.png` を `tests\actual\` からコピー (全部 `--update` しない)。
4. replay_verify: ジョブ `2d` (`:job_2d`: `del cache\2d_showcase.scene.json` → `call :chain cache\golden_2d.rep "--2d-demo --synth-input" "--2d-demo"`)。`-Jobs` 列、`:failed` の mismatch 分岐、シーン数のコメント、`tools\mcp\src\verify.rs` のジョブ表 (規則 15-e)。
5. 不変量と性能: `--selftest` に `TwoDDemoSelfTest` (デモシーンを HeadlessSim で 3000 tick 回し、全 plane2D 剛体の z / vz / wx / wy / qx / qy を検査。Debug / Release のハッシュ焼き込み)。`Runtime.exe --2d-demo --warp --frames 300` の平均フレーム時間 (`PerfDrawBenchmark` の流儀でログ) を Release で記録 → 16 ms 以内。
6. `docs\test_checklists.md` の M94 節に手動確認項目 (sub-24 が仕上げるので下書きでよい)。
9. (sub-22 の申し送り) Create → 2D → Camera 2D が CameraComponent.orthoSize を `refResolutionY / (2 × assetsPPU)` (既定 320×180 / PPU 100 なら 0.9) で初期化しているか確認し、していなければ 1 行足す (spec 4.1.12)。デモのカメラは orthoSize 5.625 (PPU 16 / ref 180)、Camera2DFollow.boundsFromTilemap = タイルマップ、useBounds = true。全体 selftest で NavAgent 庭の焼き値 8581BEE4A676337C を確認する。コンポーネントの TypeId は着手時の末尾から (スキーマ登録の型と衝突しないこと)。
8. (sub-14 の申し送り) 供給口 `Collect` を毎クエリ呼ぶコスト: デモで 1 tick に撃つ Raycast / BoxCast の本数と、物理 + スクリプト + Perception の 1 tick 時間をログ。1 tick の sim が 2 ms を超えたら、`StaticShapeSource` の全体 Collect を tick 単位でキャッシュする (tick 境界で無効化)。
7. (spec R11) `Runtime.exe --2d-demo --froxel --deferred --screenshot` を 1 枚撮り、正射影 + froxel で壊れない (真っ黒・NaN にならない) ことを目視。golden には登録しない。

## やらないこと (このサブでは)
- engine_spec / ADR / README (sub-24)、新機能の追加 (見つかった不具合は該当サブの範囲として「仕様との差分」に出す)

## 触る場所 (planner の見立て)
- 新規 `tools\gen_2d_assets.py`、`assets\2d\**`、`src\GameLogic\Scripts\TwoDHero.cpp`、`src\Engine\Engine\Demo\TwoDDemoSelfTest.{h,cpp}`
- `src\Engine\Engine\Demo\DemoContent.{h,cpp}`、`ShowcaseScenes.cpp:24-69`、`src\Engine\Engine\App\EngineCli.cpp`
- `tools\shot_verify.bat` (:117-417 の末尾)、`tests\golden\2d.png` / `2d_lights.png`、`tools\replay_verify.bat` (:84, :job_*, :failed, コメント)、`tools\mcp\src\verify.rs`
- `EditorMain.cpp`、vcxproj (GameLogic / Engine)、`docs\test_checklists.md`
- 参考: `demo-entity-order-is-rng-stream` (生成順は RNG ストリーム — デモへ足すなら末尾)、`screenshot-probe-recipes`、`replay-verify-triage`

## 受け入れ条件 (このサブ)
1. (spec 38、**チェックポイント = sub-21〜23 の分、spec 4-5**) `tools\shot_verify.bat` が 32 枚 PASS (既存 30 枚は maxDiff=0)、`tools\replay_verify.bat` の `2d` を含む全ジョブ PASS、`Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`check_rules` 15-e が 0。割れたら sub-21〜23 のどれが原因かを特定して SELF_EVAL に書く。
2. (spec 39) `TwoDDemoSelfTest` PASS (Debug / Release)、300 フレーム平均のログ (SELF_EVAL に数値)。
3. golden 2 枚を目視できる形で `plans\m94-2d\screenshots\` にもコピー (reviewer 用)。
4. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` に `TwoDDemoSelfTest` を足して PASS、版 bump なし。

## 検証コマンド
- `python -I tools\gen_2d_assets.py`
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`pwsh tools\gen_project_files.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
**チェックポイント (このサブで回す。golden を新設するので必須)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\shot_verify.bat` (32 枚)、`tools\replay_verify.bat` (`2d` 込み)
- `bin\x64\Release\Runtime.exe --2d-demo --warp --no-audio --frames 300` (PowerShell ツールから)

## 実装メモ (coder が追記)

## フィードバック履歴
